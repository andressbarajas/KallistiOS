/* KallistiOS ##version##

   arch/xbox/hardware/usb/usb_private.h
   Copyright (C) 2026 Cypress
*/

#ifndef __XBOX_USB_PRIVATE_H
#define __XBOX_USB_PRIVATE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdatomic.h>
#include <stdint.h>

#include <kos/mutex.h>
#include <kos/thread.h>

#include <xbox/usb.h>
#include <xbox/usb/controller.h>

#include "ohci.h"

#define USB_HOST_COUNT              2U
#define USB_ROOT_PORT_COUNT         4U
#define USB_MAX_HUB_PORTS           8U
#define USB_MAX_TOPOLOGY_DEPTH      4U
#define USB_CONFIGURATION_MAX_SIZE  1024U
#define USB_CONTROL_TIMEOUT_MS      250U
#define USB_PORT_DEBOUNCE_MS        100U
#define USB_PORT_RESET_MS           50U
#define USB_POLL_INTERVAL_MS        4U

#define USB_DIRECTION_IN            0x80U
#define USB_ENDPOINT_NUMBER_MASK    0x0fU
#define USB_ENDPOINT_TRANSFER_MASK  0x03U
#define USB_ENDPOINT_CONTROL        0x00U
#define USB_ENDPOINT_ISOCHRONOUS    0x01U
#define USB_ENDPOINT_BULK           0x02U
#define USB_ENDPOINT_INTERRUPT      0x03U

#define USB_REQUEST_GET_STATUS        0U
#define USB_REQUEST_CLEAR_FEATURE     1U
#define USB_REQUEST_SET_FEATURE       3U
#define USB_REQUEST_SET_ADDRESS       5U
#define USB_REQUEST_GET_DESCRIPTOR    6U
#define USB_REQUEST_SET_CONFIGURATION 9U

#define USB_DESCRIPTOR_DEVICE        1U
#define USB_DESCRIPTOR_CONFIGURATION 2U
#define USB_DESCRIPTOR_INTERFACE     4U
#define USB_DESCRIPTOR_ENDPOINT      5U
#define USB_DESCRIPTOR_HUB           0x29U
#define USB_DESCRIPTOR_XID           0x42U

#define USB_CLASS_HUB                0x09U
#define USB_CLASS_XID                0x58U
#define USB_SUBCLASS_XID             0x42U

#define USB_PORT_CONNECTION          (1U << 0)
#define USB_PORT_ENABLE              (1U << 1)
#define USB_PORT_SUSPEND             (1U << 2)
#define USB_PORT_OVERCURRENT         (1U << 3)
#define USB_PORT_RESET               (1U << 4)
#define USB_PORT_POWER               (1U << 8)
#define USB_PORT_LOW_SPEED           (1U << 9)
#define USB_PORT_CHANGE_CONNECTION   (1U << 16)
#define USB_PORT_CHANGE_ENABLE       (1U << 17)
#define USB_PORT_CHANGE_SUSPEND      (1U << 18)
#define USB_PORT_CHANGE_OVERCURRENT  (1U << 19)
#define USB_PORT_CHANGE_RESET        (1U << 20)
#define USB_PORT_CHANGE_MASK         0x001f0000U

typedef struct __attribute__((packed)) usb_setup_packet {
    uint8_t request_type;
    uint8_t request;
    uint16_t value;
    uint16_t index;
    uint16_t length;
} usb_setup_packet_t;

typedef struct __attribute__((packed)) usb_device_descriptor {
    uint8_t length;
    uint8_t descriptor_type;
    uint16_t usb_version;
    uint8_t device_class;
    uint8_t device_subclass;
    uint8_t device_protocol;
    uint8_t max_packet_size_zero;
    uint16_t vendor_id;
    uint16_t product_id;
    uint16_t device_version;
    uint8_t manufacturer_string;
    uint8_t product_string;
    uint8_t serial_string;
    uint8_t configuration_count;
} usb_device_descriptor_t;

typedef struct __attribute__((packed)) usb_configuration_descriptor {
    uint8_t length;
    uint8_t descriptor_type;
    uint16_t total_length;
    uint8_t interface_count;
    uint8_t configuration_value;
    uint8_t configuration_string;
    uint8_t attributes;
    uint8_t max_power;
} usb_configuration_descriptor_t;

typedef struct __attribute__((packed)) usb_interface_descriptor {
    uint8_t length;
    uint8_t descriptor_type;
    uint8_t interface_number;
    uint8_t alternate_setting;
    uint8_t endpoint_count;
    uint8_t interface_class;
    uint8_t interface_subclass;
    uint8_t interface_protocol;
    uint8_t interface_string;
} usb_interface_descriptor_t;

typedef struct __attribute__((packed)) usb_endpoint_descriptor {
    uint8_t length;
    uint8_t descriptor_type;
    uint8_t endpoint_address;
    uint8_t attributes;
    uint16_t max_packet_size;
    uint8_t interval;
} usb_endpoint_descriptor_t;

typedef struct xbox_usb_endpoint {
    uint8_t address;
    uint8_t attributes;
    uint8_t interval;
    uint8_t data_toggle;
    uint16_t max_packet_size;
} xbox_usb_endpoint_t;

typedef struct xbox_usb_interface {
    uint8_t number;
    uint8_t alternate_setting;
    uint8_t interface_class;
    uint8_t interface_subclass;
    uint8_t interface_protocol;
    uint8_t endpoint_count;
    xbox_usb_endpoint_t endpoints[4];
} xbox_usb_interface_t;

typedef enum usb_device_kind {
    USB_DEVICE_KIND_NONE,
    USB_DEVICE_KIND_GENERIC,
    USB_DEVICE_KIND_HUB,
    USB_DEVICE_KIND_XID
} usb_device_kind_t;

struct xbox_usb_host {
    mutex_t transfer_lock;
    unsigned int index;
    uint8_t pci_slot;
    uint8_t irq;
    uint16_t saved_pci_command;
    uint32_t saved_bar;
    uint32_t saved_control;
    uint32_t saved_interrupt_enable;
    uint32_t saved_hcca;
    uint32_t saved_control_head;
    uint32_t saved_bulk_head;
    uint32_t saved_frame_interval;
    uint32_t saved_periodic_start;
    uint32_t saved_low_speed_threshold;
    uint32_t saved_root_hub_descriptor_a;
    uint32_t saved_root_hub_descriptor_b;
    uintptr_t mmio_base;
    ohci_registers_t *registers;
    ohci_hcca_t *hcca;
    uint32_t hcca_physical;
    uint8_t root_port_count;
    bool initialized;
};

struct xbox_usb_device {
    bool allocated;
    bool connected;
    usb_device_kind_t kind;
    uint8_t address;
    uint8_t port;
    uint8_t depth;
    xbox_usb_speed_t speed;
    uint16_t endpoint_zero_max_packet;
    uint16_t vendor_id;
    uint16_t product_id;
    uint8_t device_class;
    uint8_t configuration;
    struct xbox_usb_host *host;
    struct xbox_usb_device *parent;
    xbox_usb_interface_t interface;
    void *driver_data;
};

struct xbox_controller {
    bool connected;
    unsigned int slot;
    struct xbox_usb_device *device;
    xbox_usb_endpoint_t input_endpoint;
    xbox_usb_endpoint_t output_endpoint;
    uint8_t xid_type;
    uint8_t xid_subtype;
    xbox_controller_state_t state;
};

typedef struct usb_global_state {
    mutex_t lock;
    bool initialized;
    atomic_bool stop_requested;
    atomic_bool initial_scan_complete;
    uint8_t next_address;
    kthread_t *worker;
    struct xbox_usb_host hosts[USB_HOST_COUNT];
    struct xbox_usb_device devices[XBOX_USB_MAX_DEVICES];
    struct xbox_controller controllers[XBOX_CONTROLLER_MAX_COUNT];
} usb_global_state_t;

extern usb_global_state_t usb_state;

uint32_t usb_pci_read32(uint8_t slot, uint8_t function, uint8_t offset);
void usb_pci_write32(uint8_t slot, uint8_t function, uint8_t offset,
                     uint32_t value);
int usb_virtual_to_physical(const void *address, uint32_t *physical);
int usb_dma_range_valid(const void *address, size_t size);
void *usb_dma_allocate(size_t alignment, size_t size);
void usb_dma_free(void *allocation);

int usb_control_request(struct xbox_usb_device *device,
                        uint8_t request_type, uint8_t request,
                        uint16_t value, uint16_t index,
                        void *data, uint16_t length,
                        size_t *transferred);
int usb_interrupt_request(struct xbox_usb_device *device,
                          xbox_usb_endpoint_t *endpoint,
                          void *data, size_t length,
                          size_t *transferred,
                          unsigned int timeout_ms);
int usb_enumerate_port(struct xbox_usb_host *host,
                       struct xbox_usb_device *parent,
                       uint8_t port, xbox_usb_speed_t speed);
struct xbox_usb_device *usb_find_child(struct xbox_usb_host *host,
                                       struct xbox_usb_device *parent,
                                       uint8_t port);
void usb_disconnect_device(struct xbox_usb_device *device);

int usb_hub_bind(struct xbox_usb_device *device,
                 const xbox_usb_interface_t *interface);
void usb_hub_unbind(struct xbox_usb_device *device);
void usb_hub_poll_all(void);

int usb_xid_bind(struct xbox_usb_device *device,
                 const xbox_usb_interface_t *interface);
void usb_xid_unbind(struct xbox_usb_device *device);
void usb_xid_poll_all(void);

#endif /* __XBOX_USB_PRIVATE_H */
