/* KallistiOS ##version##

   xbox/usb/controller.h
   Copyright (C) 2026 Cypress
*/

/** \file    xbox/usb/controller.h
    \brief   Original Xbox USB controller interface.
    \ingroup xbox_controller
*/

#ifndef __XBOX_USB_CONTROLLER_H
#define __XBOX_USB_CONTROLLER_H

#include <kos/cdefs.h>
__BEGIN_DECLS

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/** \defgroup xbox_controller Xbox controllers
    \brief                        Original Xbox XID game-controller support
    \ingroup                      xbox_usb
*/

#define XBOX_CONTROLLER_MAX_COUNT 4U

/** Digital button masks in xbox_controller_state_t::buttons. */
enum xbox_controller_button {
    XBOX_CONTROLLER_DPAD_UP     = 1U << 0,
    XBOX_CONTROLLER_DPAD_DOWN   = 1U << 1,
    XBOX_CONTROLLER_DPAD_LEFT   = 1U << 2,
    XBOX_CONTROLLER_DPAD_RIGHT  = 1U << 3,
    XBOX_CONTROLLER_START       = 1U << 4,
    XBOX_CONTROLLER_BACK        = 1U << 5,
    XBOX_CONTROLLER_LEFT_STICK  = 1U << 6,
    XBOX_CONTROLLER_RIGHT_STICK = 1U << 7
};

/** Normalized state of an original Xbox game controller. */
typedef struct xbox_controller_state {
    uint32_t buttons;       /**< Digital buttons from xbox_controller_button. */
    uint8_t a;              /**< Analog A pressure. */
    uint8_t b;              /**< Analog B pressure. */
    uint8_t x;              /**< Analog X pressure. */
    uint8_t y;              /**< Analog Y pressure. */
    uint8_t black;          /**< Analog black-button pressure. */
    uint8_t white;          /**< Analog white-button pressure. */
    uint8_t left_trigger;   /**< Left-trigger pressure. */
    uint8_t right_trigger;  /**< Right-trigger pressure. */
    int16_t left_x;         /**< Left-stick horizontal axis. */
    int16_t left_y;         /**< Left-stick vertical axis. */
    int16_t right_x;        /**< Right-stick horizontal axis. */
    int16_t right_y;        /**< Right-stick vertical axis. */
    uint32_t sequence;      /**< Increments after each valid input report. */
} xbox_controller_state_t;

/** Opaque controller handle. */
typedef struct xbox_controller xbox_controller_t;

/** Return the number of currently attached XID game controllers. */
size_t xbox_controller_count(void);

/** Return the Nth currently attached XID game controller.

    The returned pointer remains valid only while the controller stays
    attached. Discard it as soon as xbox_controller_is_connected() returns
    false and enumerate again to obtain a newly attached controller.
*/
xbox_controller_t *xbox_controller_get(size_t index);

/** Return whether a controller handle is currently connected. */
bool xbox_controller_is_connected(const xbox_controller_t *controller);

/** Copy the latest complete input report.

    \retval 0              State copied.
    \retval -1             Invalid argument or disconnected controller.
*/
int xbox_controller_get_state(const xbox_controller_t *controller,
                              xbox_controller_state_t *state);

/** Set the two controller rumble motors.

    \param controller      Attached controller.
    \param low_frequency   Low-frequency motor strength.
    \param high_frequency  High-frequency motor strength.
    \retval 0              Output report completed.
    \retval -1             Invalid argument, disconnect, or transfer error.
*/
int xbox_controller_set_rumble(xbox_controller_t *controller,
                               uint16_t low_frequency,
                               uint16_t high_frequency);

__END_DECLS

#endif /* __XBOX_USB_CONTROLLER_H */
