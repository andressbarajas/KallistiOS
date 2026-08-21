/* KallistiOS ##version##

   arch/xbox/hardware/usb/ohci.h
   Copyright (C) 2026 Cypress
*/

#ifndef __XBOX_USB_OHCI_H
#define __XBOX_USB_OHCI_H

#include <stddef.h>
#include <stdint.h>

struct xbox_usb_host;

typedef struct __attribute__((packed, aligned(16))) ohci_ed {
    volatile uint32_t control;
    volatile uint32_t tail_pointer;
    volatile uint32_t head_pointer;
    volatile uint32_t next_ed;
} ohci_ed_t;

typedef struct __attribute__((packed, aligned(16))) ohci_td {
    volatile uint32_t control;
    volatile uint32_t current_buffer;
    volatile uint32_t next_td;
    volatile uint32_t buffer_end;
} ohci_td_t;

typedef struct __attribute__((packed, aligned(256))) ohci_hcca {
    volatile uint32_t interrupt_table[32];
    volatile uint16_t frame_number;
    volatile uint16_t pad;
    volatile uint32_t done_head;
    uint8_t reserved[116];
} ohci_hcca_t;

typedef struct ohci_registers {
    volatile uint32_t revision;
    volatile uint32_t control;
    volatile uint32_t command_status;
    volatile uint32_t interrupt_status;
    volatile uint32_t interrupt_enable;
    volatile uint32_t interrupt_disable;
    volatile uint32_t hcca;
    volatile uint32_t period_current_ed;
    volatile uint32_t control_head_ed;
    volatile uint32_t control_current_ed;
    volatile uint32_t bulk_head_ed;
    volatile uint32_t bulk_current_ed;
    volatile uint32_t done_head;
    volatile uint32_t frame_interval;
    volatile uint32_t frame_remaining;
    volatile uint32_t frame_number;
    volatile uint32_t periodic_start;
    volatile uint32_t low_speed_threshold;
    volatile uint32_t root_hub_descriptor_a;
    volatile uint32_t root_hub_descriptor_b;
    volatile uint32_t root_hub_status;
    volatile uint32_t root_hub_port_status[];
} ohci_registers_t;

_Static_assert(sizeof(ohci_ed_t) == 16, "OHCI ED must be 16 bytes");
_Static_assert(sizeof(ohci_td_t) == 16, "OHCI TD must be 16 bytes");
_Static_assert(sizeof(ohci_hcca_t) == 256, "OHCI HCCA must be 256 bytes");

int ohci_host_init(struct xbox_usb_host *host);
void ohci_host_shutdown(struct xbox_usb_host *host);
int ohci_control_transfer(struct xbox_usb_host *host,
                          uint8_t address, int low_speed,
                          uint16_t max_packet_size,
                          const void *setup,
                          void *data, size_t length,
                          size_t *transferred,
                          unsigned int timeout_ms);
int ohci_interrupt_transfer(struct xbox_usb_host *host,
                            uint8_t address, int low_speed,
                            uint8_t endpoint, int direction_in,
                            uint16_t max_packet_size,
                            uint8_t interval,
                            uint8_t *data_toggle,
                            void *data, size_t length,
                            size_t *transferred,
                            unsigned int timeout_ms);
uint32_t ohci_root_port_status(struct xbox_usb_host *host,
                               unsigned int port);
int ohci_root_port_power(struct xbox_usb_host *host, unsigned int port);
int ohci_root_port_reset(struct xbox_usb_host *host, unsigned int port);
void ohci_root_port_clear_changes(struct xbox_usb_host *host,
                                  unsigned int port, uint32_t changes);

#endif /* __XBOX_USB_OHCI_H */
