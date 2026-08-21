/* KallistiOS ##version##

   arch/xbox/hardware/smc.c
   Copyright (C) 2026 Cypress
*/

/*
   Original Xbox System Management Controller status interface.

   The SMC answers on the MCPX SMBus at address 0x10. This file owns the policy
   of what may be touched; smbus.c owns only the transport.

   The policy is a short list, and everything not on it is excluded on purpose:

     0x01 version       readable, but see smc_version_index_reset()
     0x03 tray state    plain read
     0x04 AV pack       plain read
     0x09 CPU temp      plain read
     0x0a board temp    plain read
     0x0f error code    plain read

   Deliberately absent, with reasons, so a future reader does not have to guess
   whether they were forgotten:

     0x02 power         reset, power-cycle and shutdown commands
     0x05 fan mode      thermal policy
     0x06 fan speed     thermal policy; a wrong value can cook the console
     0x07 LED mode      visible state that would need saving and restoring
     0x08 LED sequence  same
     0x0c tray eject    mechanical action
     0x0d interrupt ack mutates pending event state
     0x11 int status    clear-on-read; reading it would steal a pending tray,
                        eject or AV-pack event from the dashboard or kernel
     0x19 reset-on-eject, 0x1a interrupt enable   policy changes
     0x1b scratch       persists across reboot and is used by the dashboard
     0x1c-0x21          the boot challenge/response handshake

   The one write this file performs is the version index reset, and it is
   confined to a single function whose register index is a compile-time
   constant.
*/

#include "smbus_private.h"

#include <xbox/smc.h>

#include <kos/mutex.h>

#include <errno.h>
#include <stddef.h>
#include <stdint.h>

#define SMC_ADDRESS             0x10U

#define SMC_REG_VERSION         0x01U
#define SMC_REG_TRAY_STATE      0x03U
#define SMC_REG_AV_PACK         0x04U
#define SMC_REG_CPU_TEMP        0x09U
#define SMC_REG_BOARD_TEMP      0x0aU
#define SMC_REG_ERROR_CODE      0x0fU

/* Tray state is reported in the upper nibble. */
#define SMC_TRAY_MASK           0x70U
#define SMC_TRAY_CLOSED         0x00U
#define SMC_TRAY_OPEN           0x10U
#define SMC_TRAY_NO_MEDIA       0x40U
#define SMC_TRAY_MEDIA          0x60U

/* AV pack is reported in the low three bits. */
#define SMC_AV_MASK             0x07U
#define SMC_AV_SCART            0x00U
#define SMC_AV_HDTV             0x01U
#define SMC_AV_VGA              0x02U
#define SMC_AV_RFU              0x03U
#define SMC_AV_SVIDEO           0x04U
#define SMC_AV_COMPOSITE        0x06U
#define SMC_AV_NONE             0x07U

/*
   The version register is a stateful three-byte stream. The transport mutex
   serializes each bus cycle, but the reset/read/read/read sequence must also
   be atomic with respect to another version caller or the two streams can
   interleave and corrupt both results.
*/
static mutex_t smc_version_lock = MUTEX_INITIALIZER;

static int smc_read(uint8_t reg, uint8_t *value) {
    if(value == NULL) {
        errno = EINVAL;
        return -1;
    }

    return xbox_smbus_read_byte(SMC_ADDRESS, reg, value);
}

int xbox_smc_tray_state(xbox_smc_tray_state_t *state) {
    uint8_t raw;

    if(state == NULL) {
        errno = EINVAL;
        return -1;
    }

    if(smc_read(SMC_REG_TRAY_STATE, &raw) != 0)
        return -1;

    switch(raw & SMC_TRAY_MASK) {
        case SMC_TRAY_CLOSED:
            *state = XBOX_SMC_TRAY_CLOSED;
            break;
        case SMC_TRAY_OPEN:
            *state = XBOX_SMC_TRAY_OPEN;
            break;
        case SMC_TRAY_NO_MEDIA:
            *state = XBOX_SMC_TRAY_NO_MEDIA;
            break;
        case SMC_TRAY_MEDIA:
            *state = XBOX_SMC_TRAY_MEDIA_DETECTED;
            break;
        default:
            /* An unrecognized encoding is reported rather than guessed at, so
               a console revision this was not tested against stays visible
               instead of silently masquerading as a known state. */
            *state = XBOX_SMC_TRAY_UNKNOWN;
            break;
    }

    return 0;
}

int xbox_smc_av_pack(xbox_smc_av_pack_t *pack) {
    uint8_t raw;

    if(pack == NULL) {
        errno = EINVAL;
        return -1;
    }

    if(smc_read(SMC_REG_AV_PACK, &raw) != 0)
        return -1;

    switch(raw & SMC_AV_MASK) {
        case SMC_AV_SCART:
            *pack = XBOX_SMC_AV_SCART;
            break;
        case SMC_AV_HDTV:
            *pack = XBOX_SMC_AV_HDTV;
            break;
        case SMC_AV_VGA:
            *pack = XBOX_SMC_AV_VGA;
            break;
        case SMC_AV_RFU:
            *pack = XBOX_SMC_AV_RFU;
            break;
        case SMC_AV_SVIDEO:
            *pack = XBOX_SMC_AV_SVIDEO;
            break;
        case SMC_AV_COMPOSITE:
            *pack = XBOX_SMC_AV_COMPOSITE;
            break;
        case SMC_AV_NONE:
            *pack = XBOX_SMC_AV_NONE;
            break;
        default:
            *pack = XBOX_SMC_AV_UNKNOWN;
            break;
    }

    return 0;
}

int xbox_smc_cpu_temperature(uint8_t *celsius) {
    return smc_read(SMC_REG_CPU_TEMP, celsius);
}

int xbox_smc_board_temperature(uint8_t *celsius) {
    return smc_read(SMC_REG_BOARD_TEMP, celsius);
}

int xbox_smc_error_code(uint8_t *code) {
    return smc_read(SMC_REG_ERROR_CODE, code);
}

/*
   Rewind the SMC's version-string read pointer.

   This is the only write in the Xbox SMBus/SMC code. The register index is a
   constant here rather than a parameter precisely so this function cannot be
   repurposed into a general SMC write by a later edit that only changes a call
   site.
*/
static int smc_version_index_reset(void) {
    return xbox_smbus_write_byte(SMC_ADDRESS, SMC_REG_VERSION, 0x00U);
}

int xbox_smc_version(char *version) {
    uint8_t raw;
    int rv = -1;
    int i;

    if(version == NULL) {
        errno = EINVAL;
        return -1;
    }

    if(mutex_lock(&smc_version_lock) != 0)
        return -1;

    if(smc_version_index_reset() != 0)
        goto out;

    for(i = 0; i < XBOX_SMC_VERSION_LENGTH; i++) {
        if(smc_read(SMC_REG_VERSION, &raw) != 0)
            goto out;

        version[i] = (char)raw;
    }

    rv = 0;

out:
    mutex_unlock(&smc_version_lock);
    return rv;
}
