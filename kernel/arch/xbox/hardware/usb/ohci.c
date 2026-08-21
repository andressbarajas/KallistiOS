/* KallistiOS ##version##

   arch/xbox/hardware/usb/ohci.c
   Copyright (C) 2026 Cypress
*/

#include <kos/dbglog.h>
#include <kos/mutex.h>
#include <kos/thread.h>
#include <kos/timer.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "ohci.h"
#include "usb_private.h"

#define PCI_COMMAND_OFFSET             0x04U
#define PCI_BAR0_OFFSET                0x10U
#define PCI_COMMAND_MEMORY             (1U << 1)
#define PCI_COMMAND_BUS_MASTER         (1U << 2)
#define PCI_BAR_MEMORY_MASK            0xfffffff0U
#define XBOX_PHYSICAL_MAP_BASE         0x80000000U

#define OHCI_CONTROL_PLE               (1U << 2)
#define OHCI_CONTROL_CLE               (1U << 4)
#define OHCI_CONTROL_BLE               (1U << 5)
#define OHCI_CONTROL_STATE_MASK        (3U << 6)
#define OHCI_CONTROL_STATE_RESET       (0U << 6)
#define OHCI_CONTROL_STATE_OPERATIONAL (2U << 6)
#define OHCI_CONTROL_IR                (1U << 8)

#define OHCI_COMMAND_RESET             (1U << 0)
#define OHCI_COMMAND_CONTROL_FILLED    (1U << 1)

#define OHCI_INTERRUPT_WDH             (1U << 1)
#define OHCI_INTERRUPT_UE              (1U << 4)
#define OHCI_INTERRUPT_RHSC            (1U << 6)
#define OHCI_INTERRUPT_OC              (1U << 30)
#define OHCI_INTERRUPT_MIE             (1U << 31)
#define OHCI_INTERRUPT_ALL             0x4000007fU

#define OHCI_ED_FUNCTION_ADDRESS(a)    ((uint32_t)(a) & 0x7fU)
#define OHCI_ED_ENDPOINT(e)            (((uint32_t)(e) & 0x0fU) << 7)
#define OHCI_ED_DIRECTION_TD           (0U << 11)
#define OHCI_ED_DIRECTION_OUT          (1U << 11)
#define OHCI_ED_DIRECTION_IN           (2U << 11)
#define OHCI_ED_LOW_SPEED              (1U << 13)
#define OHCI_ED_SKIP                   (1U << 14)
#define OHCI_ED_MAX_PACKET(m)          (((uint32_t)(m) & 0x7ffU) << 16)
#define OHCI_ED_HEAD_HALTED            (1U << 0)
#define OHCI_ED_HEAD_CARRY             (1U << 1)
#define OHCI_ED_POINTER_MASK           0xfffffff0U

#define OHCI_TD_ROUNDING               (1U << 18)
#define OHCI_TD_DIRECTION_SETUP        (0U << 19)
#define OHCI_TD_DIRECTION_OUT          (1U << 19)
#define OHCI_TD_DIRECTION_IN           (2U << 19)
#define OHCI_TD_DELAY_INTERRUPT_NONE   (0U << 21)
#define OHCI_TD_TOGGLE_DATA0           (2U << 24)
#define OHCI_TD_TOGGLE_DATA1           (3U << 24)
#define OHCI_TD_CC_SHIFT               28U
#define OHCI_TD_CC_MASK                (0x0fU << OHCI_TD_CC_SHIFT)
#define OHCI_TD_CC_NO_ERROR            0U
#define OHCI_TD_CC_DATA_UNDERRUN       9U
#define OHCI_TD_CC_NOT_ACCESSED        14U

#define OHCI_FRAME_INTERVAL            0x2edfU
#define OHCI_FRAME_FSMPS(fi)           ((((fi) - 210U) * 6U / 7U) << 16)
#define OHCI_PERIODIC_START            ((OHCI_FRAME_INTERVAL * 9U) / 10U)
#define OHCI_LOW_SPEED_THRESHOLD       0x0628U

#define OHCI_ROOT_DESCRIPTOR_A_NDP     0xffU
#define OHCI_ROOT_DESCRIPTOR_A_NPS     (1U << 9)
#define OHCI_ROOT_DESCRIPTOR_A_POTPGT  (0xffU << 24)

typedef struct __attribute__((aligned(32))) ohci_control_layout {
    ohci_ed_t endpoint;
    ohci_td_t setup;
    ohci_td_t data;
    ohci_td_t status;
    ohci_td_t tail;
    usb_setup_packet_t setup_packet;
} ohci_control_layout_t;

typedef struct __attribute__((aligned(32))) ohci_interrupt_layout {
    ohci_ed_t endpoint;
    ohci_td_t data;
    ohci_td_t tail;
} ohci_interrupt_layout_t;

static void ohci_memory_barrier(void) {
    __asm__ volatile("" : : : "memory");
    __sync_synchronize();
}

static int ohci_wait_register(volatile uint32_t *reg, uint32_t mask,
                              uint32_t expected, unsigned int timeout_ms) {
    uint64_t deadline = timer_ms_gettime64() + timeout_ms;

    while((*reg & mask) != expected) {
        if(timer_ms_gettime64() >= deadline)
            return -1;
        thd_pass();
    }

    return 0;
}

static int ohci_physical(const void *address, uint32_t *physical) {
    if(usb_virtual_to_physical(address, physical) != 0)
        return -1;

    return *physical < 0x04000000U ? 0 : -1;
}

static uint32_t ohci_td_cc(const ohci_td_t *td) {
    return (td->control & OHCI_TD_CC_MASK) >> OHCI_TD_CC_SHIFT;
}

static int ohci_prepare_buffer(ohci_td_t *td, void *buffer, size_t length) {
    uint32_t first;
    uint32_t last;

    if(length == 0U) {
        td->current_buffer = 0;
        td->buffer_end = 0;
        return 0;
    }
    if(!buffer || length > 8192U ||
       usb_dma_range_valid(buffer, length) != 0 ||
       ohci_physical(buffer, &first) != 0 ||
       ohci_physical((uint8_t *)buffer + length - 1U, &last) != 0)
        return -1;

    /* A general TD may describe only its starting and ending physical pages. */
    if((last >> 12) - (first >> 12) > 1U)
        return -1;

    td->current_buffer = first;
    td->buffer_end = last;
    return 0;
}

static size_t ohci_td_transferred(const ohci_td_t *td, size_t requested) {
    uint32_t current = td->current_buffer;
    uint32_t end = td->buffer_end;
    size_t remaining;

    if(requested == 0U || current == 0U)
        return requested;
    if((current >> 12) == (end >> 12))
        remaining = (size_t)(end - current) + 1U;
    else
        remaining = (size_t)(0x1000U - (current & 0x0fffU)) +
                    (size_t)(end & 0x0fffU) + 1U;

    return remaining <= requested ? requested - remaining : 0U;
}

static int ohci_wait_transfer(struct xbox_usb_host *host,
                              ohci_ed_t *endpoint,
                              ohci_td_t *last_td,
                              ohci_td_t *last_data_td,
                              size_t requested, size_t *transferred,
                              unsigned int timeout_ms) {
    uint64_t deadline = timer_ms_gettime64() + timeout_ms;
    uint32_t tail = endpoint->tail_pointer & OHCI_ED_POINTER_MASK;

    while((endpoint->head_pointer & OHCI_ED_POINTER_MASK) != tail) {
        uint32_t status = host->registers->interrupt_status;

        if(status & OHCI_INTERRUPT_UE) {
            host->registers->interrupt_status = status;
            return -1;
        }
        if(endpoint->head_pointer & OHCI_ED_HEAD_HALTED)
            return -1;
        if(timer_ms_gettime64() >= deadline)
            return -1;
        thd_pass();
    }

    if(!last_td ||
       (ohci_td_cc(last_td) != OHCI_TD_CC_NO_ERROR &&
        ohci_td_cc(last_td) != OHCI_TD_CC_DATA_UNDERRUN))
        return -1;

    if(last_data_td) {
        uint32_t cc = ohci_td_cc(last_data_td);

        if(cc != OHCI_TD_CC_NO_ERROR && cc != OHCI_TD_CC_DATA_UNDERRUN)
            return -1;
        if(transferred)
            *transferred = ohci_td_transferred(last_data_td, requested);
    }
    else if(transferred) {
        *transferred = 0U;
    }

    host->registers->interrupt_status =
        OHCI_INTERRUPT_WDH | OHCI_INTERRUPT_UE;
    host->hcca->done_head = 0;
    return 0;
}

static void ohci_restore_controller_state(struct xbox_usb_host *host) {
    if(!host->registers)
        return;

    host->registers->interrupt_disable = OHCI_INTERRUPT_ALL |
                                         OHCI_INTERRUPT_MIE;
    host->registers->control &=
        ~(OHCI_CONTROL_CLE | OHCI_CONTROL_BLE | OHCI_CONTROL_PLE);
    ohci_memory_barrier();
    thd_sleep(2U);

    host->registers->interrupt_status = OHCI_INTERRUPT_ALL;
    host->registers->hcca = host->saved_hcca;
    host->registers->control_head_ed = host->saved_control_head;
    host->registers->bulk_head_ed = host->saved_bulk_head;
    host->registers->frame_interval = host->saved_frame_interval;
    host->registers->periodic_start = host->saved_periodic_start;
    host->registers->low_speed_threshold =
        host->saved_low_speed_threshold;
    host->registers->root_hub_descriptor_a =
        host->saved_root_hub_descriptor_a;
    host->registers->root_hub_descriptor_b =
        host->saved_root_hub_descriptor_b;
    host->registers->control = host->saved_control;
    host->registers->interrupt_disable = OHCI_INTERRUPT_ALL |
                                         OHCI_INTERRUPT_MIE;
    if(host->saved_interrupt_enable)
        host->registers->interrupt_enable = host->saved_interrupt_enable;
}

int ohci_host_init(struct xbox_usb_host *host) {
    uint32_t pci_command;
    uint32_t frame_interval;
    unsigned int port;

    if(!host || host->initialized)
        return -1;

    host->saved_pci_command =
        (uint16_t)usb_pci_read32(host->pci_slot, 0, PCI_COMMAND_OFFSET);
    host->saved_bar =
        usb_pci_read32(host->pci_slot, 0, PCI_BAR0_OFFSET);
    if(host->saved_bar == 0U || host->saved_bar == 0xffffffffU ||
       (host->saved_bar & 1U)) {
        dbglog(DBG_WARNING, "usb: OHCI%u invalid BAR %08lx\n",
               host->index, (unsigned long)host->saved_bar);
        return -1;
    }

    host->mmio_base = host->saved_bar & PCI_BAR_MEMORY_MASK;
    /*
     * MCPX normally reports its already-mapped 0xFEDxxxxx MMIO window.
     * A low physical BAR, as used by some test environments, is reached
     * through the Xbox kernel's inherited 0x80000000 direct map.
     */
    host->registers = (ohci_registers_t *)
        (host->mmio_base >= XBOX_PHYSICAL_MAP_BASE
             ? host->mmio_base
             : XBOX_PHYSICAL_MAP_BASE | host->mmio_base);
    if((host->registers->revision & 0xffU) != 0x10U) {
        dbglog(DBG_WARNING, "usb: OHCI%u invalid revision %08lx\n",
               host->index, (unsigned long)host->registers->revision);
        return -1;
    }

    pci_command = host->saved_pci_command |
                  PCI_COMMAND_MEMORY | PCI_COMMAND_BUS_MASTER;
    usb_pci_write32(host->pci_slot, 0, PCI_COMMAND_OFFSET, pci_command);

    host->saved_control = host->registers->control;
    host->saved_interrupt_enable = host->registers->interrupt_enable;
    host->saved_hcca = host->registers->hcca;
    host->saved_control_head = host->registers->control_head_ed;
    host->saved_bulk_head = host->registers->bulk_head_ed;
    host->saved_frame_interval = host->registers->frame_interval;
    host->saved_periodic_start = host->registers->periodic_start;
    host->saved_low_speed_threshold = host->registers->low_speed_threshold;
    host->saved_root_hub_descriptor_a =
        host->registers->root_hub_descriptor_a;
    host->saved_root_hub_descriptor_b =
        host->registers->root_hub_descriptor_b;

    if(host->registers->control & OHCI_CONTROL_IR) {
        host->registers->command_status = 1U << 3;
        if(ohci_wait_register(&host->registers->control,
                              OHCI_CONTROL_IR, 0, 500U) != 0) {
            dbglog(DBG_WARNING, "usb: OHCI%u ownership timeout\n",
                   host->index);
            goto fail_restore_pci;
        }
    }

    host->registers->interrupt_disable = OHCI_INTERRUPT_MIE;
    host->registers->control =
        (host->registers->control & ~OHCI_CONTROL_STATE_MASK) |
        OHCI_CONTROL_STATE_RESET;
    host->registers->command_status = OHCI_COMMAND_RESET;
    if(ohci_wait_register(&host->registers->command_status,
                          OHCI_COMMAND_RESET, 0, 100U) != 0) {
        dbglog(DBG_WARNING, "usb: OHCI%u reset timeout\n", host->index);
        goto fail_restore_pci;
    }

    host->hcca = usb_dma_allocate(256U, sizeof(*host->hcca));
    if(!host->hcca ||
       ohci_physical(host->hcca, &host->hcca_physical) != 0) {
        dbglog(DBG_WARNING, "usb: OHCI%u cannot allocate DMA HCCA\n",
               host->index);
        goto fail_hcca;
    }
    memset(host->hcca, 0, sizeof(*host->hcca));
    if(mutex_init(&host->transfer_lock, MUTEX_TYPE_NORMAL) != 0) {
        dbglog(DBG_WARNING, "usb: OHCI%u transfer mutex failed\n",
               host->index);
        goto fail_hcca;
    }

    frame_interval = OHCI_FRAME_INTERVAL |
                     OHCI_FRAME_FSMPS(OHCI_FRAME_INTERVAL);
    host->registers->hcca = host->hcca_physical;
    host->registers->control_head_ed = 0;
    host->registers->bulk_head_ed = 0;
    host->registers->done_head = 0;
    host->registers->frame_interval = frame_interval;
    host->registers->periodic_start = OHCI_PERIODIC_START;
    host->registers->low_speed_threshold = OHCI_LOW_SPEED_THRESHOLD;
    host->registers->interrupt_status = OHCI_INTERRUPT_ALL;
    host->registers->interrupt_disable = OHCI_INTERRUPT_MIE;

    host->root_port_count =
        (uint8_t)(host->registers->root_hub_descriptor_a &
                  OHCI_ROOT_DESCRIPTOR_A_NDP);
    if(host->root_port_count == 0U ||
       host->root_port_count > USB_ROOT_PORT_COUNT)
        host->root_port_count = USB_ROOT_PORT_COUNT;

    /* Xbox ports are always powered. NPS is also accepted by xemu's OHCI. */
    host->registers->root_hub_descriptor_a =
        (host->registers->root_hub_descriptor_a &
         ~OHCI_ROOT_DESCRIPTOR_A_POTPGT) |
        OHCI_ROOT_DESCRIPTOR_A_NPS;
    host->registers->control =
        (host->registers->control &
         ~(OHCI_CONTROL_STATE_MASK | OHCI_CONTROL_CLE |
           OHCI_CONTROL_BLE | OHCI_CONTROL_PLE)) |
        OHCI_CONTROL_STATE_OPERATIONAL;
    ohci_memory_barrier();
    thd_sleep(10U);

    for(port = 0; port < host->root_port_count; ++port)
        ohci_root_port_power(host, port);

    host->initialized = true;
    dbglog(DBG_INFO,
           "usb: OHCI%u PCI 00:%02x.0 MMIO %08lx, %u root ports\n",
           host->index, host->pci_slot, (unsigned long)host->mmio_base,
           host->root_port_count);
    return 0;

fail_hcca:
    usb_dma_free(host->hcca);
    host->hcca = NULL;
fail_restore_pci:
    ohci_restore_controller_state(host);
    usb_pci_write32(host->pci_slot, 0, PCI_COMMAND_OFFSET,
                    host->saved_pci_command);
    host->registers = NULL;
    return -1;
}

void ohci_host_shutdown(struct xbox_usb_host *host) {
    unsigned int index;

    if(!host || !host->initialized)
        return;

    mutex_lock(&host->transfer_lock);
    for(index = 0; index < 32U; ++index)
        host->hcca->interrupt_table[index] = 0;
    ohci_restore_controller_state(host);
    mutex_unlock(&host->transfer_lock);
    mutex_destroy(&host->transfer_lock);

    usb_pci_write32(host->pci_slot, 0, PCI_COMMAND_OFFSET,
                    host->saved_pci_command);
    usb_dma_free(host->hcca);
    host->hcca = NULL;
    host->registers = NULL;
    host->initialized = false;
}

int ohci_control_transfer(struct xbox_usb_host *host,
                          uint8_t address, int low_speed,
                          uint16_t max_packet_size,
                          const void *setup,
                          void *data, size_t length,
                          size_t *transferred,
                          unsigned int timeout_ms) {
    ohci_control_layout_t *layout;
    uint32_t endpoint_physical;
    uint32_t setup_physical;
    uint32_t data_physical;
    uint32_t status_physical;
    uint32_t tail_physical;
    int direction_in;
    int result = -1;

    if(!host || !host->initialized || !setup ||
       max_packet_size == 0U || max_packet_size > 1023U)
        return -1;

    layout = usb_dma_allocate(32U, sizeof(*layout));
    if(!layout)
        return -1;
    memset(layout, 0, sizeof(*layout));
    memcpy(&layout->setup_packet, setup, sizeof(layout->setup_packet));

    if(ohci_physical(&layout->endpoint, &endpoint_physical) != 0 ||
       ohci_physical(&layout->setup, &setup_physical) != 0 ||
       ohci_physical(&layout->data, &data_physical) != 0 ||
       ohci_physical(&layout->status, &status_physical) != 0 ||
       ohci_physical(&layout->tail, &tail_physical) != 0 ||
       ohci_prepare_buffer(&layout->setup, &layout->setup_packet,
                           sizeof(layout->setup_packet)) != 0)
        goto out;

    direction_in =
        (layout->setup_packet.request_type & USB_DIRECTION_IN) != 0;
    layout->setup.control =
        (OHCI_TD_CC_NOT_ACCESSED << OHCI_TD_CC_SHIFT) |
        OHCI_TD_DELAY_INTERRUPT_NONE |
        OHCI_TD_TOGGLE_DATA0 | OHCI_TD_DIRECTION_SETUP;
    layout->setup.next_td =
        length ? data_physical : status_physical;

    if(length) {
        layout->data.control =
            (OHCI_TD_CC_NOT_ACCESSED << OHCI_TD_CC_SHIFT) |
            OHCI_TD_DELAY_INTERRUPT_NONE | OHCI_TD_ROUNDING |
            OHCI_TD_TOGGLE_DATA1 |
            (direction_in ? OHCI_TD_DIRECTION_IN :
                            OHCI_TD_DIRECTION_OUT);
        layout->data.next_td = status_physical;
        if(ohci_prepare_buffer(&layout->data, data, length) != 0)
            goto out;
    }

    layout->status.control =
        (OHCI_TD_CC_NOT_ACCESSED << OHCI_TD_CC_SHIFT) |
        OHCI_TD_DELAY_INTERRUPT_NONE | OHCI_TD_TOGGLE_DATA1 |
        (direction_in ? OHCI_TD_DIRECTION_OUT : OHCI_TD_DIRECTION_IN);
    layout->status.next_td = tail_physical;
    layout->endpoint.control =
        OHCI_ED_FUNCTION_ADDRESS(address) | OHCI_ED_DIRECTION_TD |
        (low_speed ? OHCI_ED_LOW_SPEED : 0U) |
        OHCI_ED_MAX_PACKET(max_packet_size);
    layout->endpoint.head_pointer = setup_physical;
    layout->endpoint.tail_pointer = tail_physical;

    mutex_lock(&host->transfer_lock);
    host->registers->control_head_ed = endpoint_physical;
    host->registers->control |= OHCI_CONTROL_CLE;
    ohci_memory_barrier();
    host->registers->command_status = OHCI_COMMAND_CONTROL_FILLED;
    result = ohci_wait_transfer(host, &layout->endpoint, &layout->status,
                                length ? &layout->data : NULL,
                                length, transferred, timeout_ms);
    layout->endpoint.control |= OHCI_ED_SKIP;
    host->registers->control &= ~OHCI_CONTROL_CLE;
    host->registers->control_head_ed = 0;
    ohci_memory_barrier();
    thd_sleep(2U);
    mutex_unlock(&host->transfer_lock);

out:
    usb_dma_free(layout);
    return result;
}

int ohci_interrupt_transfer(struct xbox_usb_host *host,
                            uint8_t address, int low_speed,
                            uint8_t endpoint, int direction_in,
                            uint16_t max_packet_size,
                            uint8_t interval,
                            uint8_t *data_toggle,
                            void *data, size_t length,
                            size_t *transferred,
                            unsigned int timeout_ms) {
    ohci_interrupt_layout_t *layout;
    uint32_t endpoint_physical;
    uint32_t data_physical;
    uint32_t tail_physical;
    unsigned int schedule_interval;
    unsigned int index;
    int result = -1;

    if(!host || !host->initialized || endpoint > 15U || !data_toggle ||
       max_packet_size == 0U || max_packet_size > 1023U ||
       length == 0U || !data)
        return -1;

    layout = usb_dma_allocate(32U, sizeof(*layout));
    if(!layout)
        return -1;
    memset(layout, 0, sizeof(*layout));

    if(ohci_physical(&layout->endpoint, &endpoint_physical) != 0 ||
       ohci_physical(&layout->data, &data_physical) != 0 ||
       ohci_physical(&layout->tail, &tail_physical) != 0 ||
       ohci_prepare_buffer(&layout->data, data, length) != 0)
        goto out;

    layout->data.control =
        (OHCI_TD_CC_NOT_ACCESSED << OHCI_TD_CC_SHIFT) |
        OHCI_TD_DELAY_INTERRUPT_NONE | OHCI_TD_ROUNDING |
        (*data_toggle ? OHCI_TD_TOGGLE_DATA1 : OHCI_TD_TOGGLE_DATA0) |
        (direction_in ? OHCI_TD_DIRECTION_IN : OHCI_TD_DIRECTION_OUT);
    layout->data.next_td = tail_physical;
    layout->endpoint.control =
        OHCI_ED_FUNCTION_ADDRESS(address) |
        OHCI_ED_ENDPOINT(endpoint) |
        (direction_in ? OHCI_ED_DIRECTION_IN : OHCI_ED_DIRECTION_OUT) |
        (low_speed ? OHCI_ED_LOW_SPEED : 0U) |
        OHCI_ED_MAX_PACKET(max_packet_size);
    layout->endpoint.head_pointer = data_physical;
    layout->endpoint.tail_pointer = tail_physical;

    schedule_interval = 1U;
    while((schedule_interval << 1) <= interval &&
          schedule_interval < 32U)
        schedule_interval <<= 1;

    mutex_lock(&host->transfer_lock);
    for(index = 0; index < 32U; ++index)
        host->hcca->interrupt_table[index] =
            index % schedule_interval == 0U ? endpoint_physical : 0U;
    host->registers->control |= OHCI_CONTROL_PLE;
    ohci_memory_barrier();
    result = ohci_wait_transfer(host, &layout->endpoint, &layout->data,
                                &layout->data,
                                length, transferred, timeout_ms);
    if(result == 0) {
        size_t actual = transferred
            ? *transferred
            : ohci_td_transferred(&layout->data, length);
        size_t packet_count =
            actual ? (actual + max_packet_size - 1U) / max_packet_size : 1U;

        if(packet_count & 1U)
            *data_toggle ^= 1U;
    }
    layout->endpoint.control |= OHCI_ED_SKIP;
    host->registers->control &= ~OHCI_CONTROL_PLE;
    for(index = 0; index < 32U; ++index)
        host->hcca->interrupt_table[index] = 0;
    ohci_memory_barrier();
    thd_sleep(2U);
    mutex_unlock(&host->transfer_lock);

out:
    usb_dma_free(layout);
    return result;
}

uint32_t ohci_root_port_status(struct xbox_usb_host *host,
                               unsigned int port) {
    if(!host || !host->initialized || port >= host->root_port_count)
        return 0;
    return host->registers->root_hub_port_status[port];
}

int ohci_root_port_power(struct xbox_usb_host *host, unsigned int port) {
    if(!host || !host->registers || port >= host->root_port_count)
        return -1;

    host->registers->root_hub_port_status[port] = USB_PORT_POWER;
    thd_sleep(20U);
    return 0;
}

int ohci_root_port_reset(struct xbox_usb_host *host, unsigned int port) {
    uint32_t status;

    if(!host || !host->initialized || port >= host->root_port_count)
        return -1;
    status = ohci_root_port_status(host, port);
    if(!(status & USB_PORT_CONNECTION))
        return -1;

    host->registers->root_hub_port_status[port] = USB_PORT_RESET;
    if(ohci_wait_register(&host->registers->root_hub_port_status[port],
                          USB_PORT_CHANGE_RESET,
                          USB_PORT_CHANGE_RESET, 100U) != 0)
        return -1;
    status = ohci_root_port_status(host, port);
    host->registers->root_hub_port_status[port] =
        status & USB_PORT_CHANGE_MASK;
    return (status & USB_PORT_ENABLE) ? 0 : -1;
}

void ohci_root_port_clear_changes(struct xbox_usb_host *host,
                                  unsigned int port, uint32_t changes) {
    if(host && host->initialized && port < host->root_port_count)
        host->registers->root_hub_port_status[port] =
            changes & USB_PORT_CHANGE_MASK;
}
