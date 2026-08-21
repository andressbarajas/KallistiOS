/* KallistiOS ##version##

   arch/xbox/hardware/usb/hub.c
   Copyright (C) 2026 Cypress
*/

#include <kos/dbglog.h>
#include <kos/thread.h>
#include <kos/timer.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "usb_private.h"

#define USB_REQUEST_TYPE_CLASS_DEVICE_IN 0xa0U
#define USB_REQUEST_TYPE_CLASS_PORT_IN   0xa3U
#define USB_REQUEST_TYPE_CLASS_PORT_OUT  0x23U

#define HUB_FEATURE_PORT_RESET           4U
#define HUB_FEATURE_PORT_POWER           8U
#define HUB_FEATURE_C_PORT_CONNECTION    16U
#define HUB_FEATURE_C_PORT_ENABLE        17U
#define HUB_FEATURE_C_PORT_SUSPEND       18U
#define HUB_FEATURE_C_PORT_OVERCURRENT   19U
#define HUB_FEATURE_C_PORT_RESET         20U

typedef struct __attribute__((packed)) usb_hub_descriptor {
    uint8_t length;
    uint8_t descriptor_type;
    uint8_t port_count;
    uint16_t characteristics;
    uint8_t power_on_delay;
    uint8_t controller_current;
    uint8_t removable[2];
    uint8_t power_control_mask[2];
} usb_hub_descriptor_t;

typedef struct usb_hub {
    struct xbox_usb_device *device;
    xbox_usb_endpoint_t status_endpoint;
    uint8_t port_count;
    uint8_t power_on_delay;
} usb_hub_t;

static int usb_hub_request(usb_hub_t *hub, uint8_t request_type,
                           uint8_t request, uint16_t value,
                           uint16_t port, void *data, uint16_t length,
                           size_t *transferred) {
    return usb_control_request(hub->device, request_type, request,
                               value, port, data, length, transferred);
}

static int usb_hub_get_port_status(usb_hub_t *hub, uint8_t port,
                                   uint32_t *status) {
    size_t transferred;

    *status = 0;
    return usb_hub_request(hub, USB_REQUEST_TYPE_CLASS_PORT_IN,
                           USB_REQUEST_GET_STATUS, 0, port,
                           status, sizeof(*status), &transferred) == 0 &&
           transferred == sizeof(*status) ? 0 : -1;
}

static int usb_hub_set_port_feature(usb_hub_t *hub, uint8_t port,
                                    uint16_t feature) {
    size_t transferred;

    return usb_hub_request(hub, USB_REQUEST_TYPE_CLASS_PORT_OUT,
                           USB_REQUEST_SET_FEATURE, feature, port,
                           NULL, 0, &transferred);
}

static void usb_hub_clear_port_feature(usb_hub_t *hub, uint8_t port,
                                       uint16_t feature) {
    size_t transferred;

    usb_hub_request(hub, USB_REQUEST_TYPE_CLASS_PORT_OUT,
                    USB_REQUEST_CLEAR_FEATURE, feature, port,
                    NULL, 0, &transferred);
}

static int usb_hub_reset_port(usb_hub_t *hub, uint8_t port,
                              uint32_t *status) {
    uint64_t deadline;

    if(usb_hub_set_port_feature(hub, port,
                                HUB_FEATURE_PORT_RESET) != 0)
        return -1;
    deadline = timer_ms_gettime64() + 200U;

    do {
        thd_sleep(10U);
        if(usb_hub_get_port_status(hub, port, status) != 0)
            return -1;
        if(*status & USB_PORT_CHANGE_RESET) {
            usb_hub_clear_port_feature(hub, port,
                                       HUB_FEATURE_C_PORT_RESET);
            return (*status & USB_PORT_ENABLE) ? 0 : -1;
        }
    } while(timer_ms_gettime64() < deadline);

    return -1;
}

static void usb_hub_clear_changes(usb_hub_t *hub, uint8_t port,
                                  uint32_t status) {
    if(status & USB_PORT_CHANGE_CONNECTION)
        usb_hub_clear_port_feature(hub, port,
                                   HUB_FEATURE_C_PORT_CONNECTION);
    if(status & USB_PORT_CHANGE_ENABLE)
        usb_hub_clear_port_feature(hub, port,
                                   HUB_FEATURE_C_PORT_ENABLE);
    if(status & USB_PORT_CHANGE_SUSPEND)
        usb_hub_clear_port_feature(hub, port,
                                   HUB_FEATURE_C_PORT_SUSPEND);
    if(status & USB_PORT_CHANGE_OVERCURRENT)
        usb_hub_clear_port_feature(hub, port,
                                   HUB_FEATURE_C_PORT_OVERCURRENT);
    if(status & USB_PORT_CHANGE_RESET)
        usb_hub_clear_port_feature(hub, port,
                                   HUB_FEATURE_C_PORT_RESET);
}

static void usb_hub_poll(usb_hub_t *hub) {
    uint8_t port;

    for(port = 1U; port <= hub->port_count; ++port) {
        uint32_t status;
        struct xbox_usb_device *child =
            usb_find_child(hub->device->host, hub->device, port);

        if(usb_hub_get_port_status(hub, port, &status) != 0)
            continue;

        if(!(status & USB_PORT_CONNECTION)) {
            if(child)
                usb_disconnect_device(child);
        }
        else if(!child) {
            thd_sleep(USB_PORT_DEBOUNCE_MS);
            if(usb_hub_get_port_status(hub, port, &status) == 0 &&
               (status & USB_PORT_CONNECTION) &&
               usb_hub_reset_port(hub, port, &status) == 0) {
                usb_enumerate_port(
                    hub->device->host, hub->device, port,
                    (status & USB_PORT_LOW_SPEED)
                        ? XBOX_USB_SPEED_LOW : XBOX_USB_SPEED_FULL);
            }
        }

        usb_hub_clear_changes(hub, port, status);
    }
}

int usb_hub_bind(struct xbox_usb_device *device,
                 const xbox_usb_interface_t *interface) {
    usb_hub_descriptor_t descriptor;
    usb_hub_t *hub;
    size_t transferred;
    unsigned int endpoint_index;
    uint8_t port;

    if(!device || !interface)
        return -1;
    hub = calloc(1, sizeof(*hub));
    if(!hub)
        return -1;
    hub->device = device;

    if(usb_hub_request(hub, USB_REQUEST_TYPE_CLASS_DEVICE_IN,
                       USB_REQUEST_GET_DESCRIPTOR,
                       USB_DESCRIPTOR_HUB << 8, 0,
                       &descriptor, sizeof(descriptor),
                       &transferred) != 0 ||
       transferred < 7U || descriptor.descriptor_type != USB_DESCRIPTOR_HUB ||
       descriptor.port_count == 0U ||
       descriptor.port_count > USB_MAX_HUB_PORTS)
        goto fail;
    hub->port_count = descriptor.port_count;
    hub->power_on_delay = descriptor.power_on_delay;

    for(endpoint_index = 0;
        endpoint_index < interface->endpoint_count; ++endpoint_index) {
        const xbox_usb_endpoint_t *endpoint =
            &interface->endpoints[endpoint_index];

        if((endpoint->attributes & USB_ENDPOINT_TRANSFER_MASK) ==
                USB_ENDPOINT_INTERRUPT &&
           (endpoint->address & USB_DIRECTION_IN)) {
            hub->status_endpoint = *endpoint;
            break;
        }
    }
    if(hub->status_endpoint.max_packet_size == 0U)
        goto fail;

    device->driver_data = hub;
    device->kind = USB_DEVICE_KIND_HUB;
    for(port = 1U; port <= hub->port_count; ++port)
        usb_hub_set_port_feature(hub, port, HUB_FEATURE_PORT_POWER);
    thd_sleep((unsigned int)hub->power_on_delay * 2U + 20U);
    usb_hub_poll(hub);
    dbglog(DBG_INFO, "usb: hub at address %u has %u ports\n",
           device->address, hub->port_count);
    return 0;

fail:
    free(hub);
    return -1;
}

void usb_hub_unbind(struct xbox_usb_device *device) {
    if(!device)
        return;
    free(device->driver_data);
    device->driver_data = NULL;
    device->kind = USB_DEVICE_KIND_GENERIC;
}

void usb_hub_poll_all(void) {
    unsigned int index;

    for(index = 0; index < XBOX_USB_MAX_DEVICES; ++index) {
        struct xbox_usb_device *device = &usb_state.devices[index];

        if(device->connected && device->kind == USB_DEVICE_KIND_HUB &&
           device->driver_data)
            usb_hub_poll(device->driver_data);
    }
}
