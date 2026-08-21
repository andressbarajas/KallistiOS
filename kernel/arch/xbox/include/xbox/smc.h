/* KallistiOS ##version##

   xbox/smc.h
   Copyright (C) 2026 Cypress
*/

/** \file    xbox/smc.h
    \brief   Original Xbox System Management Controller status interface.
    \ingroup xbox_smc
*/

#ifndef __XBOX_SMC_H
#define __XBOX_SMC_H

#include <kos/cdefs.h>
__BEGIN_DECLS

#include <stdint.h>

/** \defgroup xbox_smc Xbox SMC
    \brief             Original Xbox System Management Controller
    \ingroup           system

    The SMC is the always-on microcontroller that owns the Xbox's power
    sequencing, front panel, cooling and tray mechanism. It is reached over the
    MCPX SMBus at address 0x10.

    This interface exposes status and offers no state-changing commands.
    Powering off, resetting, ejecting the tray, changing fan speed, changing
    the LED, acknowledging events and writing the persistent scratch byte are
    all SMC capabilities that are intentionally absent here. Each of them
    changes machine state in a way that a caller reading a temperature has no
    business triggering by accident, and each needs its own design decisions
    about safety and restoration before it is offered.

    \note
    On revision 1.6 consoles the SMC is part of the integrated Xyclops device
    rather than the earlier discrete PIC. The interface is the same.

    \note
    These functions serialize access with blocking mutexes and must be called
    from thread context, not an interrupt handler.

    @{
*/

/** Number of characters in the SMC version string. */
#define XBOX_SMC_VERSION_LENGTH 3

/** Disc tray position and media presence, as reported by the SMC. */
typedef enum xbox_smc_tray_state {
    XBOX_SMC_TRAY_CLOSED,         /**< Closed. */
    XBOX_SMC_TRAY_OPEN,           /**< Open. */
    XBOX_SMC_TRAY_NO_MEDIA,       /**< Closed, no media detected. */
    XBOX_SMC_TRAY_MEDIA_DETECTED, /**< Closed, media detected. */
    XBOX_SMC_TRAY_UNKNOWN         /**< Reported value was not recognized. */
} xbox_smc_tray_state_t;

/** Which AV pack is attached, as reported by the SMC. */
typedef enum xbox_smc_av_pack {
    XBOX_SMC_AV_SCART,     /**< SCART. */
    XBOX_SMC_AV_HDTV,      /**< Component HDTV. */
    XBOX_SMC_AV_VGA,       /**< VGA. */
    XBOX_SMC_AV_RFU,       /**< Reserved for future use. */
    XBOX_SMC_AV_SVIDEO,    /**< S-Video. */
    XBOX_SMC_AV_COMPOSITE, /**< Composite. */
    XBOX_SMC_AV_NONE,      /**< No AV pack attached. */
    XBOX_SMC_AV_UNKNOWN    /**< Reported value was not recognized. */
} xbox_smc_av_pack_t;

/** \brief   Read the current tray state.

    \param  state           Where to store the tray state.

    \return                 0 on success, or -1 on failure with errno set.

    \exception EINVAL       \p state was NULL.
    \exception ENODEV       No SMBus controller was found.
    \exception EBUSY        The SMBus controller never became idle.
    \exception ETIMEDOUT    The transaction did not complete in time.
    \exception ECANCELED    The SMBus host aborted the transaction.
    \exception EIO          The SMBus reported a bus or protocol error.
*/
int xbox_smc_tray_state(xbox_smc_tray_state_t *state);

/** \brief   Read which AV pack is attached.

    \param  pack            Where to store the AV pack identity.

    \return                 0 on success, or -1 on failure with errno set.

    \sa xbox_smc_tray_state()
*/
int xbox_smc_av_pack(xbox_smc_av_pack_t *pack);

/** \brief   Read the CPU temperature in degrees Celsius.

    \param  celsius         Where to store the temperature.

    \return                 0 on success, or -1 on failure with errno set.

    \note
    On a revision 1.6 console this and xbox_smc_board_temperature() reported
    identical values across 50 samples spanning six distinct temperatures, so
    the two registers there are almost certainly fed by one sensor. Earlier
    revisions carry a separate temperature device. Either way, do not treat a
    difference between the two as meaningful.

    \sa xbox_smc_board_temperature()
*/
int xbox_smc_cpu_temperature(uint8_t *celsius);

/** \brief   Read the motherboard temperature in degrees Celsius.

    \param  celsius         Where to store the temperature.

    \return                 0 on success, or -1 on failure with errno set.

    \sa xbox_smc_cpu_temperature()
*/
int xbox_smc_board_temperature(uint8_t *celsius);

/** \brief   Read the SMC's last recorded error code.

    \param  code            Where to store the error code.

    \return                 0 on success, or -1 on failure with errno set.
*/
int xbox_smc_error_code(uint8_t *code);

/** \brief   Read the SMC firmware version string.

    The version identifies the SMC firmware, and in practice the motherboard
    revision with it: for example "P2L" corresponds to a revision 1.6 console.

    \warning
    Unlike every other function here, this one is not purely a read. The SMC
    returns the version one character at a time from an internal pointer, and
    that pointer can only be rewound by writing to the version register. The
    write changes no machine state: it selects which of three characters comes
    back next, and any other reader rewinds it before reading. Callers who
    require that KOS never write to the SMC at all should not call this.

    \param  version         Buffer of at least #XBOX_SMC_VERSION_LENGTH bytes.
                            Not NUL-terminated.

    \return                 0 on success, or -1 on failure with errno set.
*/
int xbox_smc_version(char *version);

/** @} */

__END_DECLS

#endif /* __XBOX_SMC_H */
