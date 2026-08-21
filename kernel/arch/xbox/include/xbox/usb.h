/* KallistiOS ##version##

   xbox/usb.h
   Copyright (C) 2026 Cypress
*/

/** \file    xbox/usb.h
    \brief   Xbox USB host and device interface.
    \ingroup xbox_usb
*/

#ifndef __XBOX_USB_H
#define __XBOX_USB_H

#include <kos/cdefs.h>
__BEGIN_DECLS

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/** \defgroup xbox_usb Xbox USB
    \brief                 USB host support for the original Xbox
    \ingroup               peripherals
*/

/** Maximum number of devices represented by the Xbox USB host stack. */
#define XBOX_USB_MAX_DEVICES 32U

/** USB device link speed. */
typedef enum xbox_usb_speed {
    XBOX_USB_SPEED_LOW,
    XBOX_USB_SPEED_FULL
} xbox_usb_speed_t;

/** Opaque USB device handle. */
typedef struct xbox_usb_device xbox_usb_device_t;

/** Initialize both MCPX OHCI host controllers and enumerate the USB tree.

    This function is normally called by KOS initialization.

    \retval 0              Success.
    \retval -1             No usable OHCI controller or initialization error.
*/
int usb_init(void);

/** Shut down USB and restore the host-controller state inherited at startup. */
void usb_shutdown(void);

/** Return whether USB initialization completed successfully. */
bool usb_is_initialized(void);

/** Wait until the initial root-hub scan has completed.

    \param timeout_ms      Maximum wait in milliseconds, or zero for no timeout.
    \retval 0              Initial scan completed.
    \retval -1             USB is unavailable or the timeout expired.
*/
int usb_wait_scan(unsigned int timeout_ms);

/** Return the number of currently attached USB devices. */
size_t usb_device_count(void);

/** Return the Nth currently attached USB device.

    The returned pointer remains valid only while the device stays attached.
*/
xbox_usb_device_t *usb_device_get(size_t index);

/** Return whether a device handle still represents an attached device. */
bool usb_device_is_connected(const xbox_usb_device_t *device);

/** Return a device's assigned USB address. */
uint8_t usb_device_address(const xbox_usb_device_t *device);

/** Return a device's negotiated link speed. */
xbox_usb_speed_t usb_device_speed(const xbox_usb_device_t *device);

/** Return a device's USB vendor identifier. */
uint16_t usb_device_vendor_id(const xbox_usb_device_t *device);

/** Return a device's USB product identifier. */
uint16_t usb_device_product_id(const xbox_usb_device_t *device);

/** Return the upstream device, or NULL for a root-hub child. */
xbox_usb_device_t *usb_device_parent(const xbox_usb_device_t *device);

/** Return the one-based upstream hub port used by a device. */
uint8_t usb_device_port(const xbox_usb_device_t *device);

__END_DECLS

#endif /* __XBOX_USB_H */
