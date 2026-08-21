/* KallistiOS ##version##

   arch/xbox/hardware/smbus_private.h
   Copyright (C) 2026 Cypress
*/

/* Private MCPX SMBus transport.

   This header is deliberately not installed and not public. The Xbox SMBus
   host is an AMD756-compatible controller, not the Intel PIIX4/ICH part that
   most x86 SMBus code assumes, so nothing about this layout is portable and
   no generic SMBus abstraction is offered. A public abstraction waits until a
   second in-tree consumer proves one is needed; today the only consumer is the
   status-focused SMC interface in smc.c.

   Measured properties this transport relies on, from a retail v1.6 console and
   xemu 0.8.136. Full evidence is in the external research notes:

   - The host is PCI 00:01.1, NVIDIA nForce, class 0x0c0500.
   - The register block lives at BAR 1 + 0, not at the 0xe0 sub-offset some
     AMD756 documentation implies. Reads at +0xe0 return 0xff.
   - BAR 1 is programmed by the Xbox kernel. BAR 0 is present but left with a
     zero base, so the controller must be found by BAR index rather than by
     scanning for the first I/O BAR.
   - Retail exposes a second, independent bus segment at BAR 2 that xemu does
     not model. This transport drives BAR 1 only.
   - The Xbox kernel leaves the controller idle with no stale status bits.
*/

#ifndef __ARCH_XBOX_SMBUS_PRIVATE_H
#define __ARCH_XBOX_SMBUS_PRIVATE_H

#include <kos/cdefs.h>
__BEGIN_DECLS

#include <stdint.h>

/** Read one byte from a device register using an SMBus byte-data cycle.

    Serialized against other transactions and bounded at every wait. Returns 0
    on success, or -1 with errno set to ENODEV when no controller was found,
    EBUSY when the controller never went idle, ETIMEDOUT when the cycle did not
    complete, or EIO for a decoded bus error. */
int xbox_smbus_read_byte(uint8_t address, uint8_t command, uint8_t *value);

/** Write one byte to a device register using an SMBus byte-data cycle.

    \warning
    This increment sanctions exactly one caller and exactly one target
    register: the SMC version-string index reset in smc.c, which rewinds a read
    pointer and changes no machine state. Every other SMC register reachable by
    a write is a control register: power, reset, eject, fan speed, LED state or
    the persistent scratch byte. Adding a second caller means revisiting the
    documented safety boundary first, not just calling this function.

    It is declared here rather than kept static because the policy of what may
    be written belongs to the SMC layer, not to the transport. */
int xbox_smbus_write_byte(uint8_t address, uint8_t command, uint8_t value);

__END_DECLS

#endif /* __ARCH_XBOX_SMBUS_PRIVATE_H */
