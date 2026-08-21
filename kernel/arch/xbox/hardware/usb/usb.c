/* KallistiOS ##version##

   arch/xbox/hardware/usb/usb.c
   Copyright (C) 2026 Cypress
*/

#include <arch/arch.h>

#include <kos/dbglog.h>
#include <kos/mutex.h>
#include <kos/thread.h>
#include <kos/timer.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <xbox/usb.h>

#include "usb_private.h"

#define PCI_CONFIG_ADDRESS_PORT 0x0cf8U
#define PCI_CONFIG_DATA_PORT    0x0cfcU
#define PCI_ENABLE              0x80000000U

#define X86_PAGE_PRESENT        (1U << 0)
#define X86_PAGE_SIZE_4M        (1U << 7)
#define X86_PAGE_FRAME_4K       0xfffff000U
#define X86_PAGE_FRAME_4M       0xffc00000U
#define XBOX_PHYSICAL_RAM_LIMIT 0x04000000U
#define XBOX_PAGE_DIRECTORY     0xc0300000U
#define XBOX_PAGE_TABLES        0xc0000000U

#define USB_PCI_VENDOR_DEVICE_OFFSET 0x00U
#define USB_PCI_CLASS_OFFSET         0x08U
#define USB_PCI_CLASS_OHCI           0x0c0310U

usb_global_state_t usb_state;

/* Default auto-init hooks. KOS_INIT_FLAGS() may override these with strong
   pointers or leave the driver unreferenced when INIT_NO_USB is selected. */
int (*usb_init_weak)(void) __weak_symbol = usb_init;
void (*usb_shutdown_weak)(void) __weak_symbol = usb_shutdown;

static inline void usb_out32(uint16_t port, uint32_t value) {
    __asm__ volatile("outl %0, %1" : : "a"(value), "Nd"(port) : "memory");
}

static inline uint32_t usb_in32(uint16_t port) {
    uint32_t value;

    __asm__ volatile("inl %1, %0" : "=a"(value) : "Nd"(port) : "memory");
    return value;
}

uint32_t usb_pci_read32(uint8_t slot, uint8_t function, uint8_t offset) {
    uint32_t address = PCI_ENABLE |
                       ((uint32_t)slot << 11) |
                       ((uint32_t)function << 8) |
                       (offset & 0xfcU);
    irq_disable_scoped();
    usb_out32(PCI_CONFIG_ADDRESS_PORT, address);
    return usb_in32(PCI_CONFIG_DATA_PORT);
}

void usb_pci_write32(uint8_t slot, uint8_t function, uint8_t offset,
                     uint32_t value) {
    uint32_t address = PCI_ENABLE |
                       ((uint32_t)slot << 11) |
                       ((uint32_t)function << 8) |
                       (offset & 0xfcU);
    irq_disable_scoped();
    usb_out32(PCI_CONFIG_ADDRESS_PORT, address);
    usb_out32(PCI_CONFIG_DATA_PORT, value);
}

int usb_virtual_to_physical(const void *address, uint32_t *physical) {
    uintptr_t virtual_address = (uintptr_t)address;
    uint32_t directory_entry;
    uint32_t table_entry;

    if(!address || !physical)
        return -1;

    /*
     * The Xbox kernel exposes its active paging structures through the x86
     * recursive mapping at 0xc0000000 (PTEs) and 0xc0300000 (PDEs).
     * xbox-load-ip does not map arbitrary physical page-table pages through
     * 0x80000000, so walking CR3 through that window faults in hosted guests.
     */
    directory_entry = *(volatile const uint32_t *)(uintptr_t)
        (XBOX_PAGE_DIRECTORY +
         ((virtual_address >> 20) & 0x00000ffcU));
    if(!(directory_entry & X86_PAGE_PRESENT))
        return -1;

    if(directory_entry & X86_PAGE_SIZE_4M) {
        *physical = (directory_entry & X86_PAGE_FRAME_4M) |
                    (virtual_address & 0x003fffffU);
        return 0;
    }

    table_entry = *(volatile const uint32_t *)(uintptr_t)
        (XBOX_PAGE_TABLES + ((virtual_address >> 12) << 2));
    if(!(table_entry & X86_PAGE_PRESENT))
        return -1;

    *physical = (table_entry & X86_PAGE_FRAME_4K) |
                (virtual_address & 0x0fffU);
    return 0;
}

int usb_dma_range_valid(const void *address, size_t size) {
    uintptr_t start = (uintptr_t)address;
    uintptr_t end;
    uintptr_t page;
    uint32_t physical;
    uint32_t expected;

    if(!address || size == 0U || size - 1U > UINTPTR_MAX - start)
        return -1;
    end = start + size - 1U;

    if(usb_virtual_to_physical((const void *)start, &physical) != 0 ||
       physical >= XBOX_PHYSICAL_RAM_LIMIT)
        return -1;
    expected = physical & X86_PAGE_FRAME_4K;

    page = (start & ~(uintptr_t)0x0fffU) + 0x1000U;
    while(page <= end) {
        if(usb_virtual_to_physical((const void *)page, &physical) != 0 ||
           (physical & X86_PAGE_FRAME_4K) != expected + 0x1000U ||
           physical >= XBOX_PHYSICAL_RAM_LIMIT)
            return -1;
        expected = physical & X86_PAGE_FRAME_4K;
        page += 0x1000U;
    }

    return 0;
}

void *usb_dma_allocate(size_t alignment, size_t size) {
    void *allocation;
    size_t rounded;

    if(alignment < sizeof(void *) ||
       (alignment & (alignment - 1U)) != 0U ||
       size == 0U || size > SIZE_MAX - (alignment - 1U))
        return NULL;
    rounded = (size + alignment - 1U) & ~(alignment - 1U);
    allocation = aligned_alloc(alignment, rounded);
    if(!allocation)
        return NULL;
    if(usb_dma_range_valid(allocation, rounded) != 0) {
        free(allocation);
        return NULL;
    }
    return allocation;
}

void usb_dma_free(void *allocation) {
    free(allocation);
}

struct xbox_usb_device *usb_find_child(struct xbox_usb_host *host,
                                       struct xbox_usb_device *parent,
                                       uint8_t port) {
    unsigned int index;

    for(index = 0; index < XBOX_USB_MAX_DEVICES; ++index) {
        struct xbox_usb_device *device = &usb_state.devices[index];

        if(device->allocated && device->host == host &&
           device->parent == parent && device->port == port)
            return device;
    }
    return NULL;
}

static struct xbox_usb_device *usb_allocate_device(void) {
    unsigned int index;

    for(index = 0; index < XBOX_USB_MAX_DEVICES; ++index) {
        struct xbox_usb_device *device = &usb_state.devices[index];

        if(!device->allocated) {
            memset(device, 0, sizeof(*device));
            device->allocated = true;
            device->endpoint_zero_max_packet = 8U;
            return device;
        }
    }
    return NULL;
}

static bool usb_address_in_use(uint8_t address) {
    unsigned int index;

    for(index = 0; index < XBOX_USB_MAX_DEVICES; ++index) {
        if(usb_state.devices[index].allocated &&
           usb_state.devices[index].address == address)
            return true;
    }
    return false;
}

static int usb_allocate_address(void) {
    unsigned int attempts;

    for(attempts = 0; attempts < 127U; ++attempts) {
        uint8_t address = usb_state.next_address;

        if(++usb_state.next_address > 127U)
            usb_state.next_address = 1U;
        if(address != 0U && !usb_address_in_use(address))
            return address;
    }
    return -1;
}

int usb_control_request(struct xbox_usb_device *device,
                        uint8_t request_type, uint8_t request,
                        uint16_t value, uint16_t index,
                        void *data, uint16_t length,
                        size_t *transferred) {
    usb_setup_packet_t setup;

    if(!device || !device->allocated || !device->host)
        return -1;

    setup.request_type = request_type;
    setup.request = request;
    setup.value = value;
    setup.index = index;
    setup.length = length;
    return ohci_control_transfer(device->host, device->address,
                                 device->speed == XBOX_USB_SPEED_LOW,
                                 device->endpoint_zero_max_packet,
                                 &setup, data, length, transferred,
                                 USB_CONTROL_TIMEOUT_MS);
}

int usb_interrupt_request(struct xbox_usb_device *device,
                          xbox_usb_endpoint_t *endpoint,
                          void *data, size_t length,
                          size_t *transferred,
                          unsigned int timeout_ms) {
    if(!device || !device->connected || !endpoint)
        return -1;

    return ohci_interrupt_transfer(
        device->host, device->address,
        device->speed == XBOX_USB_SPEED_LOW,
        endpoint->address & USB_ENDPOINT_NUMBER_MASK,
        (endpoint->address & USB_DIRECTION_IN) != 0,
        endpoint->max_packet_size, endpoint->interval,
        &endpoint->data_toggle,
        data, length, transferred, timeout_ms);
}

static int usb_parse_configuration(struct xbox_usb_device *device,
                                   const uint8_t *bytes, size_t length) {
    const usb_configuration_descriptor_t *configuration;
    xbox_usb_interface_t *interface = &device->interface;
    size_t offset;
    bool selected_interface = false;

    if(length < sizeof(*configuration))
        return -1;
    configuration = (const usb_configuration_descriptor_t *)bytes;
    if(configuration->descriptor_type != USB_DESCRIPTOR_CONFIGURATION ||
       configuration->length < sizeof(*configuration) ||
       configuration->total_length > length)
        return -1;

    memset(interface, 0, sizeof(*interface));
    device->configuration = configuration->configuration_value;
    offset = configuration->length;

    while(offset + 2U <= configuration->total_length) {
        uint8_t descriptor_length = bytes[offset];
        uint8_t descriptor_type = bytes[offset + 1U];

        if(descriptor_length < 2U ||
           offset + descriptor_length > configuration->total_length)
            return -1;

        if(descriptor_type == USB_DESCRIPTOR_INTERFACE &&
           descriptor_length >= sizeof(usb_interface_descriptor_t)) {
            const usb_interface_descriptor_t *source =
                (const usb_interface_descriptor_t *)(bytes + offset);

            if(!selected_interface && source->alternate_setting == 0U) {
                interface->number = source->interface_number;
                interface->alternate_setting = source->alternate_setting;
                interface->interface_class = source->interface_class;
                interface->interface_subclass = source->interface_subclass;
                interface->interface_protocol = source->interface_protocol;
                selected_interface = true;
            }
            else if(selected_interface) {
                break;
            }
        }
        else if(descriptor_type == USB_DESCRIPTOR_ENDPOINT &&
                descriptor_length >= sizeof(usb_endpoint_descriptor_t) &&
                selected_interface &&
                interface->endpoint_count <
                    sizeof(interface->endpoints) /
                    sizeof(interface->endpoints[0])) {
            const usb_endpoint_descriptor_t *source =
                (const usb_endpoint_descriptor_t *)(bytes + offset);
            xbox_usb_endpoint_t *destination =
                &interface->endpoints[interface->endpoint_count++];

            destination->address = source->endpoint_address;
            destination->attributes = source->attributes;
            destination->max_packet_size =
                source->max_packet_size & 0x07ffU;
            destination->interval = source->interval;
        }

        offset += descriptor_length;
    }

    return selected_interface ? 0 : -1;
}

static int usb_bind_device(struct xbox_usb_device *device) {
    xbox_usb_interface_t *interface = &device->interface;
    int result = 0;

    device->kind = USB_DEVICE_KIND_GENERIC;
    if(device->device_class == USB_CLASS_HUB ||
       interface->interface_class == USB_CLASS_HUB)
        result = usb_hub_bind(device, interface);
    else if(interface->interface_class == USB_CLASS_XID &&
            interface->interface_subclass == USB_SUBCLASS_XID)
        result = usb_xid_bind(device, interface);

    if(result != 0)
        dbglog(DBG_WARNING,
               "usb: no class driver bound address %u (%04x:%04x)\n",
               device->address, device->vendor_id, device->product_id);

    /* Class-driver support is not a condition of valid USB enumeration. */
    return 0;
}

int usb_enumerate_port(struct xbox_usb_host *host,
                       struct xbox_usb_device *parent,
                       uint8_t port, xbox_usb_speed_t speed) {
    struct xbox_usb_device *device;
    usb_device_descriptor_t descriptor;
    usb_configuration_descriptor_t configuration;
    uint8_t first_descriptor[8];
    uint8_t *configuration_bytes = NULL;
    size_t transferred;
    int address;
    int result = -1;

    if(!host || usb_find_child(host, parent, port))
        return -1;
    device = usb_allocate_device();
    if(!device)
        return -1;

    device->host = host;
    device->parent = parent;
    device->port = port;
    device->depth = parent ? parent->depth + 1U : 0U;
    device->speed = speed;
    if(device->depth >= USB_MAX_TOPOLOGY_DEPTH)
        goto fail;

    if(usb_control_request(device, USB_DIRECTION_IN, USB_REQUEST_GET_DESCRIPTOR,
                           USB_DESCRIPTOR_DEVICE << 8, 0,
                           first_descriptor, sizeof(first_descriptor),
                           &transferred) != 0 ||
       transferred != sizeof(first_descriptor) ||
       first_descriptor[1] != USB_DESCRIPTOR_DEVICE ||
       first_descriptor[7] == 0U)
        goto fail;
    device->endpoint_zero_max_packet = first_descriptor[7];

    address = usb_allocate_address();
    if(address < 0)
        goto fail;
    if(usb_control_request(device, 0, USB_REQUEST_SET_ADDRESS,
                           (uint16_t)address, 0, NULL, 0,
                           &transferred) != 0)
        goto fail;
    device->address = (uint8_t)address;
    thd_sleep(2U);

    if(usb_control_request(device, USB_DIRECTION_IN,
                           USB_REQUEST_GET_DESCRIPTOR,
                           USB_DESCRIPTOR_DEVICE << 8, 0,
                           &descriptor, sizeof(descriptor),
                           &transferred) != 0 ||
       transferred != sizeof(descriptor) ||
       descriptor.descriptor_type != USB_DESCRIPTOR_DEVICE)
        goto fail;
    device->vendor_id = descriptor.vendor_id;
    device->product_id = descriptor.product_id;
    device->device_class = descriptor.device_class;

    if(descriptor.configuration_count == 0U ||
       usb_control_request(device, USB_DIRECTION_IN,
                           USB_REQUEST_GET_DESCRIPTOR,
                           USB_DESCRIPTOR_CONFIGURATION << 8, 0,
                           &configuration, sizeof(configuration),
                           &transferred) != 0 ||
       transferred != sizeof(configuration) ||
       configuration.total_length < sizeof(configuration) ||
       configuration.total_length > USB_CONFIGURATION_MAX_SIZE)
        goto fail;

    configuration_bytes =
        usb_dma_allocate(32U, configuration.total_length);
    if(!configuration_bytes)
        goto fail;
    if(usb_control_request(device, USB_DIRECTION_IN,
                           USB_REQUEST_GET_DESCRIPTOR,
                           USB_DESCRIPTOR_CONFIGURATION << 8, 0,
                           configuration_bytes, configuration.total_length,
                           &transferred) != 0 ||
       transferred != configuration.total_length ||
       usb_parse_configuration(device, configuration_bytes,
                               transferred) != 0)
        goto fail;

    if(usb_control_request(device, 0, USB_REQUEST_SET_CONFIGURATION,
                           device->configuration, 0, NULL, 0,
                           &transferred) != 0)
        goto fail;

    device->connected = true;
    if(usb_bind_device(device) != 0)
        goto fail;

    dbglog(DBG_INFO,
           "usb: address %u %04x:%04x on OHCI%u port %u%s\n",
           device->address, device->vendor_id, device->product_id,
           host->index, port, parent ? " (hub)" : "");
    result = 0;

fail:
    usb_dma_free(configuration_bytes);
    if(result != 0)
        memset(device, 0, sizeof(*device));
    return result;
}

void usb_disconnect_device(struct xbox_usb_device *device) {
    unsigned int index;

    if(!device || !device->allocated)
        return;

    for(index = 0; index < XBOX_USB_MAX_DEVICES; ++index) {
        if(usb_state.devices[index].allocated &&
           usb_state.devices[index].parent == device)
            usb_disconnect_device(&usb_state.devices[index]);
    }

    if(device->kind == USB_DEVICE_KIND_HUB)
        usb_hub_unbind(device);
    else if(device->kind == USB_DEVICE_KIND_XID)
        usb_xid_unbind(device);

    dbglog(DBG_INFO, "usb: address %u disconnected\n", device->address);
    memset(device, 0, sizeof(*device));
}

static void usb_poll_root_ports(void) {
    unsigned int host_index;

    for(host_index = 0; host_index < USB_HOST_COUNT; ++host_index) {
        struct xbox_usb_host *host = &usb_state.hosts[host_index];
        unsigned int port;

        if(!host->initialized)
            continue;
        for(port = 0; port < host->root_port_count; ++port) {
            uint32_t status = ohci_root_port_status(host, port);
            struct xbox_usb_device *device =
                usb_find_child(host, NULL, (uint8_t)(port + 1U));

            if(!(status & USB_PORT_CONNECTION)) {
                if(device)
                    usb_disconnect_device(device);
            }
            else if(!device) {
                thd_sleep(USB_PORT_DEBOUNCE_MS);
                status = ohci_root_port_status(host, port);
                if((status & USB_PORT_CONNECTION) &&
                   ohci_root_port_reset(host, port) == 0) {
                    status = ohci_root_port_status(host, port);
                    usb_enumerate_port(
                        host, NULL, (uint8_t)(port + 1U),
                        (status & USB_PORT_LOW_SPEED)
                            ? XBOX_USB_SPEED_LOW
                            : XBOX_USB_SPEED_FULL);
                }
            }

            if(status & USB_PORT_CHANGE_MASK)
                ohci_root_port_clear_changes(host, port, status);
        }
    }
}

static void *usb_worker(void *argument) {
    (void)argument;

    mutex_lock(&usb_state.lock);
    usb_poll_root_ports();
    usb_hub_poll_all();
    atomic_store_explicit(&usb_state.initial_scan_complete, true,
                          memory_order_release);
    mutex_unlock(&usb_state.lock);

    while(!atomic_load_explicit(&usb_state.stop_requested,
                                memory_order_acquire)) {
        mutex_lock(&usb_state.lock);
        usb_poll_root_ports();
        usb_hub_poll_all();
        usb_xid_poll_all();
        mutex_unlock(&usb_state.lock);
        thd_sleep(USB_POLL_INTERVAL_MS);
    }
    return NULL;
}

int usb_init(void) {
    unsigned int index;
    unsigned int initialized_hosts = 0;

    if(usb_state.initialized)
        return -1;
    memset(&usb_state, 0, sizeof(usb_state));
    atomic_init(&usb_state.stop_requested, false);
    atomic_init(&usb_state.initial_scan_complete, false);
    usb_state.next_address = 1U;
    if(mutex_init(&usb_state.lock, MUTEX_TYPE_NORMAL) != 0)
        return -1;

    for(index = 0; index < USB_HOST_COUNT; ++index) {
        struct xbox_usb_host *host = &usb_state.hosts[index];
        uint32_t identity;
        uint32_t class_code;

        host->index = index;
        host->pci_slot = (uint8_t)(2U + index);
        host->irq = index == 0U ? 1U : 9U;
        identity = usb_pci_read32(host->pci_slot, 0,
                                  USB_PCI_VENDOR_DEVICE_OFFSET);
        class_code = usb_pci_read32(host->pci_slot, 0,
                                    USB_PCI_CLASS_OFFSET) >> 8;
        if(identity == 0xffffffffU || identity == 0U ||
           class_code != USB_PCI_CLASS_OHCI) {
            dbglog(DBG_WARNING,
                   "usb: PCI 00:%02x.0 is not OHCI (%08lx, class %06lx)\n",
                   host->pci_slot, (unsigned long)identity,
                   (unsigned long)class_code);
            continue;
        }
        if(ohci_host_init(host) == 0)
            ++initialized_hosts;
    }

    if(initialized_hosts == 0U)
        goto fail;

    usb_state.initialized = true;
    usb_state.worker = thd_create(false, usb_worker, NULL);
    if(!usb_state.worker)
        goto fail;
    if(usb_wait_scan(3000U) != 0)
        dbglog(DBG_WARNING, "usb: initial device scan timed out\n");
    return 0;

fail:
    atomic_store_explicit(&usb_state.stop_requested, true,
                          memory_order_release);
    if(usb_state.worker) {
        thd_join(usb_state.worker, NULL);
        usb_state.worker = NULL;
    }
    for(index = USB_HOST_COUNT; index-- > 0U;)
        ohci_host_shutdown(&usb_state.hosts[index]);
    mutex_destroy(&usb_state.lock);
    memset(&usb_state, 0, sizeof(usb_state));
    return -1;
}

void usb_shutdown(void) {
    unsigned int index;

    if(!usb_state.initialized)
        return;

    atomic_store_explicit(&usb_state.stop_requested, true,
                          memory_order_release);
    if(usb_state.worker) {
        thd_join(usb_state.worker, NULL);
        usb_state.worker = NULL;
    }
    for(index = 0; index < XBOX_USB_MAX_DEVICES; ++index) {
        if(usb_state.devices[index].allocated &&
           !usb_state.devices[index].parent)
            usb_disconnect_device(&usb_state.devices[index]);
    }
    for(index = USB_HOST_COUNT; index-- > 0U;)
        ohci_host_shutdown(&usb_state.hosts[index]);
    mutex_destroy(&usb_state.lock);
    memset(&usb_state, 0, sizeof(usb_state));
}

bool usb_is_initialized(void) {
    return usb_state.initialized;
}

int usb_wait_scan(unsigned int timeout_ms) {
    uint64_t deadline = timer_ms_gettime64() + timeout_ms;

    if(!usb_state.initialized)
        return -1;
    while(!atomic_load_explicit(&usb_state.initial_scan_complete,
                                memory_order_acquire)) {
        if(timeout_ms && timer_ms_gettime64() >= deadline)
            return -1;
        thd_sleep(1U);
    }
    return 0;
}

size_t usb_device_count(void) {
    size_t count = 0;
    unsigned int index;

    if(!usb_state.initialized)
        return 0;
    mutex_lock(&usb_state.lock);
    for(index = 0; index < XBOX_USB_MAX_DEVICES; ++index)
        count += usb_state.devices[index].connected;
    mutex_unlock(&usb_state.lock);
    return count;
}

xbox_usb_device_t *usb_device_get(size_t requested) {
    xbox_usb_device_t *result = NULL;
    unsigned int index;

    if(!usb_state.initialized)
        return NULL;
    mutex_lock(&usb_state.lock);
    for(index = 0; index < XBOX_USB_MAX_DEVICES; ++index) {
        if(usb_state.devices[index].connected) {
            if(requested-- == 0U) {
                result = &usb_state.devices[index];
                break;
            }
        }
    }
    mutex_unlock(&usb_state.lock);
    return result;
}

static bool usb_device_handle_valid(const xbox_usb_device_t *device) {
    uintptr_t pointer = (uintptr_t)device;
    uintptr_t first = (uintptr_t)&usb_state.devices[0];
    uintptr_t last = (uintptr_t)&usb_state.devices[XBOX_USB_MAX_DEVICES];

    return pointer >= first && pointer < last &&
           (pointer - first) % sizeof(usb_state.devices[0]) == 0U;
}

bool usb_device_is_connected(const xbox_usb_device_t *device) {
    bool connected = false;

    if(!usb_state.initialized || !usb_device_handle_valid(device))
        return false;
    mutex_lock(&usb_state.lock);
    connected = device->allocated && device->connected;
    mutex_unlock(&usb_state.lock);
    return connected;
}

uint8_t usb_device_address(const xbox_usb_device_t *device) {
    uint8_t result = 0U;

    if(!usb_state.initialized || !usb_device_handle_valid(device))
        return result;
    mutex_lock(&usb_state.lock);
    if(device->allocated && device->connected)
        result = device->address;
    mutex_unlock(&usb_state.lock);
    return result;
}

xbox_usb_speed_t usb_device_speed(const xbox_usb_device_t *device) {
    xbox_usb_speed_t result = XBOX_USB_SPEED_FULL;

    if(!usb_state.initialized || !usb_device_handle_valid(device))
        return result;
    mutex_lock(&usb_state.lock);
    if(device->allocated && device->connected)
        result = device->speed;
    mutex_unlock(&usb_state.lock);
    return result;
}

uint16_t usb_device_vendor_id(const xbox_usb_device_t *device) {
    uint16_t result = 0U;

    if(!usb_state.initialized || !usb_device_handle_valid(device))
        return result;
    mutex_lock(&usb_state.lock);
    if(device->allocated && device->connected)
        result = device->vendor_id;
    mutex_unlock(&usb_state.lock);
    return result;
}

uint16_t usb_device_product_id(const xbox_usb_device_t *device) {
    uint16_t result = 0U;

    if(!usb_state.initialized || !usb_device_handle_valid(device))
        return result;
    mutex_lock(&usb_state.lock);
    if(device->allocated && device->connected)
        result = device->product_id;
    mutex_unlock(&usb_state.lock);
    return result;
}

xbox_usb_device_t *usb_device_parent(const xbox_usb_device_t *device) {
    xbox_usb_device_t *result = NULL;

    if(!usb_state.initialized || !usb_device_handle_valid(device))
        return result;
    mutex_lock(&usb_state.lock);
    if(device->allocated && device->connected)
        result = device->parent;
    mutex_unlock(&usb_state.lock);
    return result;
}

uint8_t usb_device_port(const xbox_usb_device_t *device) {
    uint8_t result = 0U;

    if(!usb_state.initialized || !usb_device_handle_valid(device))
        return result;
    mutex_lock(&usb_state.lock);
    if(device->allocated && device->connected)
        result = device->port;
    mutex_unlock(&usb_state.lock);
    return result;
}
