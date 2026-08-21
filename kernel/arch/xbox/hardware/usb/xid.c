/* KallistiOS ##version##

   arch/xbox/hardware/usb/xid.c
   Copyright (C) 2026 Cypress
*/

#include <kos/dbglog.h>
#include <kos/mutex.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <xbox/usb/controller.h>

#include "usb_private.h"

#define XID_REQUEST_TYPE_DESCRIPTOR 0xc1U
#define XID_INPUT_REPORT_SIZE       20U
#define XID_OUTPUT_REPORT_SIZE      6U
#define XID_GAMEPAD_TYPE            0x01U
#define XID_GAMEPAD_SUBTYPE_DUKE    0x01U
#define XID_GAMEPAD_SUBTYPE_S       0x02U
#define XID_POLL_TIMEOUT_MS         8U

typedef struct __attribute__((packed)) xid_descriptor {
    uint8_t length;
    uint8_t descriptor_type;
    uint16_t xid_version;
    uint8_t type;
    uint8_t subtype;
    uint8_t max_input_report;
    uint8_t max_output_report;
    uint16_t alternate_products[4];
} xid_descriptor_t;

typedef struct __attribute__((packed)) xid_input_report {
    uint8_t report_id;
    uint8_t length;
    uint16_t buttons;
    uint8_t analog[8];
    int16_t left_x;
    int16_t left_y;
    int16_t right_x;
    int16_t right_y;
} xid_input_report_t;

typedef struct __attribute__((packed)) xid_output_report {
    uint8_t report_id;
    uint8_t length;
    uint16_t low_frequency;
    uint16_t high_frequency;
} xid_output_report_t;

_Static_assert(sizeof(xid_descriptor_t) == 16,
               "XID descriptor must be 16 bytes");
_Static_assert(sizeof(xid_input_report_t) == XID_INPUT_REPORT_SIZE,
               "XID gamepad input report must be 20 bytes");
_Static_assert(sizeof(xid_output_report_t) == XID_OUTPUT_REPORT_SIZE,
               "XID gamepad output report must be 6 bytes");

static struct xbox_controller *xid_allocate_controller(void) {
    unsigned int index;

    for(index = 0; index < XBOX_CONTROLLER_MAX_COUNT; ++index) {
        if(!usb_state.controllers[index].connected) {
            struct xbox_controller *controller =
                &usb_state.controllers[index];

            memset(controller, 0, sizeof(*controller));
            controller->slot = index;
            return controller;
        }
    }
    return NULL;
}

static void xid_decode_report(struct xbox_controller *controller,
                              const xid_input_report_t *report) {
    xbox_controller_state_t *state = &controller->state;

    state->buttons = report->buttons;
    state->a = report->analog[0];
    state->b = report->analog[1];
    state->x = report->analog[2];
    state->y = report->analog[3];
    state->black = report->analog[4];
    state->white = report->analog[5];
    state->left_trigger = report->analog[6];
    state->right_trigger = report->analog[7];
    state->left_x = report->left_x;
    state->left_y = report->left_y;
    state->right_x = report->right_x;
    state->right_y = report->right_y;
    ++state->sequence;
}

int usb_xid_bind(struct xbox_usb_device *device,
                 const xbox_usb_interface_t *interface) {
    xid_descriptor_t descriptor;
    struct xbox_controller *controller;
    size_t transferred;
    unsigned int endpoint_index;

    if(!device || !interface)
        return -1;

    /*
     * The v1 controller API intentionally covers only the Duke and
     * Controller S standard-gamepad reports. Other Xbox XID devices use
     * different controls and, in some cases, different report sizes:
     * wheels (0x10), arcade sticks (0x20), light guns (0x50), the DVD
     * remote receiver (type 0x03), and Steel Battalion (type 0x80).
     * Leave those devices generically enumerated until dedicated drivers
     * and public APIs are implemented.
     */
    if(usb_control_request(device, XID_REQUEST_TYPE_DESCRIPTOR,
                           USB_REQUEST_GET_DESCRIPTOR,
                           USB_DESCRIPTOR_XID << 8,
                           interface->number,
                           &descriptor, sizeof(descriptor),
                           &transferred) != 0 ||
       transferred != sizeof(descriptor) ||
       descriptor.length != sizeof(descriptor) ||
       descriptor.descriptor_type != USB_DESCRIPTOR_XID ||
       descriptor.type != XID_GAMEPAD_TYPE ||
       (descriptor.subtype != XID_GAMEPAD_SUBTYPE_DUKE &&
        descriptor.subtype != XID_GAMEPAD_SUBTYPE_S) ||
       descriptor.max_input_report != XID_INPUT_REPORT_SIZE ||
       descriptor.max_output_report != XID_OUTPUT_REPORT_SIZE)
        return -1;

    controller = xid_allocate_controller();
    if(!controller)
        return -1;
    controller->device = device;
    controller->xid_type = descriptor.type;
    controller->xid_subtype = descriptor.subtype;

    for(endpoint_index = 0;
        endpoint_index < interface->endpoint_count; ++endpoint_index) {
        const xbox_usb_endpoint_t *endpoint =
            &interface->endpoints[endpoint_index];

        if((endpoint->attributes & USB_ENDPOINT_TRANSFER_MASK) !=
                USB_ENDPOINT_INTERRUPT)
            continue;
        if(endpoint->address & USB_DIRECTION_IN)
            controller->input_endpoint = *endpoint;
        else
            controller->output_endpoint = *endpoint;
    }
    if((controller->input_endpoint.address &
            USB_ENDPOINT_NUMBER_MASK) == 0U ||
       (controller->output_endpoint.address &
            USB_ENDPOINT_NUMBER_MASK) == 0U ||
       controller->input_endpoint.max_packet_size <
            XID_INPUT_REPORT_SIZE ||
       controller->output_endpoint.max_packet_size <
            XID_OUTPUT_REPORT_SIZE) {
        memset(controller, 0, sizeof(*controller));
        return -1;
    }

    controller->connected = true;
    device->driver_data = controller;
    device->kind = USB_DEVICE_KIND_XID;
    dbglog(DBG_INFO,
           "usb: XID gamepad type %u subtype %u assigned controller %u\n",
           controller->xid_type, controller->xid_subtype, controller->slot);
    return 0;
}

void usb_xid_unbind(struct xbox_usb_device *device) {
    struct xbox_controller *controller;
    unsigned int slot;

    if(!device || !device->driver_data)
        return;
    controller = device->driver_data;
    slot = controller->slot;
    memset(controller, 0, sizeof(*controller));
    controller->slot = slot;
    device->driver_data = NULL;
    device->kind = USB_DEVICE_KIND_GENERIC;
}

void usb_xid_poll_all(void) {
    unsigned int index;

    for(index = 0; index < XBOX_CONTROLLER_MAX_COUNT; ++index) {
        struct xbox_controller *controller =
            &usb_state.controllers[index];
        xid_input_report_t report;
        size_t transferred = 0;

        if(!controller->connected || !controller->device ||
           !controller->device->connected)
            continue;
        memset(&report, 0, sizeof(report));
        if(usb_interrupt_request(controller->device,
                                 &controller->input_endpoint,
                                 &report, sizeof(report), &transferred,
                                 XID_POLL_TIMEOUT_MS) == 0 &&
           transferred == sizeof(report) &&
           report.length == sizeof(report) &&
           report.report_id == 0U)
            xid_decode_report(controller, &report);
    }
}

size_t xbox_controller_count(void) {
    size_t count = 0;
    unsigned int index;

    if(!usb_state.initialized)
        return 0;
    mutex_lock(&usb_state.lock);
    for(index = 0; index < XBOX_CONTROLLER_MAX_COUNT; ++index)
        count += usb_state.controllers[index].connected;
    mutex_unlock(&usb_state.lock);
    return count;
}

xbox_controller_t *xbox_controller_get(size_t requested) {
    xbox_controller_t *result = NULL;
    unsigned int index;

    if(!usb_state.initialized)
        return NULL;
    mutex_lock(&usb_state.lock);
    for(index = 0; index < XBOX_CONTROLLER_MAX_COUNT; ++index) {
        if(usb_state.controllers[index].connected) {
            if(requested-- == 0U) {
                result = &usb_state.controllers[index];
                break;
            }
        }
    }
    mutex_unlock(&usb_state.lock);
    return result;
}

static bool xid_controller_handle_valid(
    const xbox_controller_t *controller) {
    uintptr_t pointer = (uintptr_t)controller;
    uintptr_t first = (uintptr_t)&usb_state.controllers[0];
    uintptr_t last =
        (uintptr_t)&usb_state.controllers[XBOX_CONTROLLER_MAX_COUNT];

    return pointer >= first && pointer < last &&
           (pointer - first) % sizeof(usb_state.controllers[0]) == 0U;
}

bool xbox_controller_is_connected(const xbox_controller_t *controller) {
    bool connected;

    if(!usb_state.initialized ||
       !xid_controller_handle_valid(controller))
        return false;
    mutex_lock(&usb_state.lock);
    connected = controller->connected && controller->device &&
                controller->device->connected;
    mutex_unlock(&usb_state.lock);
    return connected;
}

int xbox_controller_get_state(const xbox_controller_t *controller,
                              xbox_controller_state_t *state) {
    int result = -1;

    if(!state || !usb_state.initialized ||
       !xid_controller_handle_valid(controller))
        return -1;
    mutex_lock(&usb_state.lock);
    if(controller->connected && controller->device &&
       controller->device->connected) {
        *state = controller->state;
        result = 0;
    }
    mutex_unlock(&usb_state.lock);
    return result;
}

int xbox_controller_set_rumble(xbox_controller_t *controller,
                               uint16_t low_frequency,
                               uint16_t high_frequency) {
    xid_output_report_t report;
    size_t transferred;
    int result = -1;

    if(!usb_state.initialized ||
       !xid_controller_handle_valid(controller))
        return -1;
    mutex_lock(&usb_state.lock);
    if(!controller->connected || !controller->device ||
       !controller->device->connected ||
       controller->output_endpoint.max_packet_size <
           sizeof(report))
        goto out;

    report.report_id = 0;
    report.length = sizeof(report);
    report.low_frequency = low_frequency;
    report.high_frequency = high_frequency;
    if(usb_interrupt_request(controller->device,
                             &controller->output_endpoint,
                             &report, sizeof(report), &transferred,
                             USB_CONTROL_TIMEOUT_MS) == 0 &&
       transferred == sizeof(report))
        result = 0;

out:
    mutex_unlock(&usb_state.lock);
    return result;
}
