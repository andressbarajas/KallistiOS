/* KallistiOS ##version##

   arch/xbox/hardware/smbus.c
   Copyright (C) 2026 Cypress
*/

/*
   MCPX SMBus host transport.

   The Xbox SMBus controller is AMD756-compatible. It is reached through an
   I/O register block whose base comes from BAR 1 of PCI function 00:01.1, and
   a transaction is driven by programming the target address, the command
   index and a cycle type, then polling global status until the cycle
   completes or a decoded failure appears.

   Everything here is bounded: the controller is shared with the Xbox kernel's
   own boot-time users, so this code waits for idle rather than forcing a
   transaction, gives every poll a deadline, and always leaves the controller
   idle with status cleared.

   The one thing worth knowing before changing this file: the busy path is
   written but has never been observed on hardware. Across the discovery probe
   and the driver's own testing the controller has always completed fast enough
   that GS_HST was never seen set. Treat that path as unexercised.
*/

#include "smbus_private.h"

#include <arch/irq.h>

#include <kos/mutex.h>
#include <kos/timer.h>

#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* PCI configuration mechanism 1. */
#define PCI_CONFIG_ADDRESS_PORT 0x0cf8U
#define PCI_CONFIG_DATA_PORT    0x0cfcU
#define PCI_ENABLE              0x80000000U

/* The SMBus host function. */
#define SMBUS_PCI_BUS           0U
#define SMBUS_PCI_DEVICE        1U
#define SMBUS_PCI_FUNCTION      1U
#define SMBUS_PCI_VENDOR        0x10deU

#define PCI_IDENTITY_OFFSET     0x00U
#define PCI_COMMAND_OFFSET      0x04U
#define PCI_COMMAND_IO_SPACE    0x0001U

/*
   BAR 1 specifically, not "the first I/O BAR". Retail leaves BAR 0 present
   with a zero base, so an index-agnostic search finds a useless base first.
*/
#define SMBUS_BAR_OFFSET        0x14U
#define PCI_BAR_IO_FLAG         0x1U
#define PCI_BAR_IO_MASK         0xfffcU

/* Register offsets within the block. */
#define SMB_GLOBAL_STATUS       0x00U
#define SMB_GLOBAL_ENABLE       0x02U
#define SMB_HOST_ADDRESS        0x04U
#define SMB_HOST_DATA0          0x06U
#define SMB_HOST_COMMAND        0x08U

/* Global status bits. */
#define GS_ABRT                 (1U << 0)
#define GS_COL                  (1U << 1)
#define GS_PRERR                (1U << 2)
#define GS_HST                  (1U << 3)
#define GS_HCYC                 (1U << 4)
#define GS_TO                   (1U << 5)
#define GS_CLEARABLE            (GS_ABRT | GS_COL | GS_PRERR | GS_HST | \
                                 GS_HCYC | GS_TO)
#define GS_ERRORS               (GS_ABRT | GS_COL | GS_PRERR | GS_TO)

/* Global enable bits. */
#define GE_HOST_START           (1U << 3)

/* Cycle types. Only byte-data is ever used. */
#define CYCLE_BYTE_DATA         0x02U

/* Direction bit in the host address register. */
#define ADDRESS_READ            0x01U

/*
   Bounds.

   A byte-data cycle at 100 kHz takes well under a millisecond, so these are
   generous. They exist to guarantee termination, not to be tight.
*/
#define SMBUS_IDLE_TIMEOUT_US   100000U
#define SMBUS_CYCLE_TIMEOUT_US  50000U

static mutex_t smbus_lock = MUTEX_INITIALIZER;
static uint16_t smbus_base;
static bool smbus_probed;
static bool smbus_present;

static inline void smbus_out8(uint16_t port, uint8_t value) {
    __asm__ volatile("outb %0, %1" : : "a"(value), "Nd"(port) : "memory");
}

static inline uint8_t smbus_in8(uint16_t port) {
    uint8_t value;

    __asm__ volatile("inb %1, %0" : "=a"(value) : "Nd"(port) : "memory");
    return value;
}

static inline void smbus_out32(uint16_t port, uint32_t value) {
    __asm__ volatile("outl %0, %1" : : "a"(value), "Nd"(port) : "memory");
}

static inline uint32_t smbus_in32(uint16_t port) {
    uint32_t value;

    __asm__ volatile("inl %1, %0" : "=a"(value) : "Nd"(port) : "memory");
    return value;
}

/*
   PCI configuration reads are two accesses to a shared address/data port pair,
   so they cannot be interleaved with another agent's configuration cycle.
   Interrupts are masked across the pair only, never across a transaction.
*/
static uint32_t smbus_pci_read32(uint8_t offset) {
    irq_mask_t mask;
    uint32_t address;
    uint32_t value;

    address = PCI_ENABLE |
              (SMBUS_PCI_BUS << 16) |
              (SMBUS_PCI_DEVICE << 11) |
              (SMBUS_PCI_FUNCTION << 8) |
              (uint32_t)(offset & 0xfcU);

    mask = irq_disable();
    smbus_out32(PCI_CONFIG_ADDRESS_PORT, address);
    value = smbus_in32(PCI_CONFIG_DATA_PORT);
    irq_restore(mask);

    return value;
}

/* Locate the controller once. Called with the transport lock held. */
static bool smbus_probe(void) {
    uint32_t identity;
    uint32_t command;
    uint32_t bar;

    if(smbus_probed)
        return smbus_present;

    smbus_probed = true;
    smbus_present = false;

    identity = smbus_pci_read32(PCI_IDENTITY_OFFSET);
    if((identity & 0xffffU) != SMBUS_PCI_VENDOR)
        return false;

    /* The Xbox kernel enables I/O decoding during boot. If it is off, the
       block is not reachable and this driver does not turn it on: enabling
       decode is a change to inherited configuration, not a read. */
    command = smbus_pci_read32(PCI_COMMAND_OFFSET);
    if((command & PCI_COMMAND_IO_SPACE) == 0)
        return false;

    bar = smbus_pci_read32(SMBUS_BAR_OFFSET);
    if((bar & PCI_BAR_IO_FLAG) == 0)
        return false;

    smbus_base = (uint16_t)(bar & PCI_BAR_IO_MASK);
    if(smbus_base == 0)
        return false;

    smbus_present = true;
    return true;
}

static uint8_t smbus_status(void) {
    return smbus_in8((uint16_t)(smbus_base + SMB_GLOBAL_STATUS));
}

static void smbus_clear_status(void) {
    smbus_out8((uint16_t)(smbus_base + SMB_GLOBAL_STATUS),
               (uint8_t)GS_CLEARABLE);
}

/* Wait for the controller to go idle. Returns false on timeout. */
static bool smbus_wait_idle(void) {
    uint64_t deadline = timer_us_gettime64() + SMBUS_IDLE_TIMEOUT_US;

    while((smbus_status() & GS_HST) != 0) {
        if(timer_us_gettime64() >= deadline)
            return false;
    }

    return true;
}

/*
   Poll a started cycle to completion.

   Each hardware failure is reported distinctly rather than collapsed into one
   generic error, because collision and protocol error mean different things
   about the bus and a caller debugging a new device needs to tell them apart.
*/
static int smbus_wait_cycle(void) {
    uint64_t deadline = timer_us_gettime64() + SMBUS_CYCLE_TIMEOUT_US;
    uint8_t status;

    for(;;) {
        status = smbus_status();

        if((status & GS_ERRORS) != 0) {
            smbus_clear_status();

            if(status & GS_COL)
                errno = EIO;        /* bus collision */
            else if(status & GS_ABRT)
                errno = ECANCELED;  /* host aborted the cycle */
            else if(status & GS_TO)
                errno = ETIMEDOUT;  /* target stretched past the bus timeout */
            else
                errno = EIO;        /* protocol error */

            return -1;
        }

        if((status & GS_HCYC) != 0 && (status & GS_HST) == 0)
            return 0;

        if(timer_us_gettime64() >= deadline) {
            smbus_clear_status();
            errno = ETIMEDOUT;
            return -1;
        }
    }
}

/* Common prologue: acquire, locate, wait for idle, clear stale status. */
static int smbus_begin(void) {
    mutex_lock(&smbus_lock);

    if(!smbus_probe()) {
        mutex_unlock(&smbus_lock);
        errno = ENODEV;
        return -1;
    }

    if(!smbus_wait_idle()) {
        mutex_unlock(&smbus_lock);
        errno = EBUSY;
        return -1;
    }

    smbus_clear_status();
    return 0;
}

static void smbus_end(void) {
    /* Leave the controller idle for the next user, including the Xbox kernel
       once KOS returns to the loader. */
    smbus_clear_status();
    mutex_unlock(&smbus_lock);
}

int xbox_smbus_read_byte(uint8_t address, uint8_t command, uint8_t *value) {
    int rv;

    if(value == NULL) {
        errno = EINVAL;
        return -1;
    }

    *value = 0;

    if(smbus_begin() != 0)
        return -1;

    smbus_out8((uint16_t)(smbus_base + SMB_HOST_ADDRESS),
               (uint8_t)((address << 1) | ADDRESS_READ));
    smbus_out8((uint16_t)(smbus_base + SMB_HOST_COMMAND), command);
    smbus_out8((uint16_t)(smbus_base + SMB_GLOBAL_ENABLE),
               (uint8_t)(CYCLE_BYTE_DATA | GE_HOST_START));

    rv = smbus_wait_cycle();
    if(rv == 0)
        *value = smbus_in8((uint16_t)(smbus_base + SMB_HOST_DATA0));

    smbus_end();
    return rv;
}

int xbox_smbus_write_byte(uint8_t address, uint8_t command, uint8_t value) {
    int rv;

    if(smbus_begin() != 0)
        return -1;

    smbus_out8((uint16_t)(smbus_base + SMB_HOST_ADDRESS),
               (uint8_t)((address << 1) & ~ADDRESS_READ));
    smbus_out8((uint16_t)(smbus_base + SMB_HOST_COMMAND), command);
    smbus_out8((uint16_t)(smbus_base + SMB_HOST_DATA0), value);
    smbus_out8((uint16_t)(smbus_base + SMB_GLOBAL_ENABLE),
               (uint8_t)(CYCLE_BYTE_DATA | GE_HOST_START));

    rv = smbus_wait_cycle();

    smbus_end();
    return rv;
}
