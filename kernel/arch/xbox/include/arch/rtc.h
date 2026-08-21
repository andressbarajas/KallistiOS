/* KallistiOS ##version##

   arch/xbox/include/arch/rtc.h
   Copyright (C) 2026 Cypress
*/

/** \file    arch/rtc.h
    \brief   Low-level real-time clock functionality.
    \ingroup rtc

    This file contains functions for interacting with the real-time clock in
    the original Xbox. Generally, you should prefer interacting with the higher
    level standard C functions, like time(), rather than these when simply
    needing to fetch the current system time.

    \author Cypress
*/

/* Keep this include above the macro guards */
#include <kos/rtc.h>

#ifndef __ARCH_RTC_H
#define __ARCH_RTC_H

#include <kos/cdefs.h>
__BEGIN_DECLS

#include <time.h>

/* Notes:
    The Xbox real-time clock is an MC146818-compatible part inside the MCPX
    southbridge, reached through the legacy PC index/data port pair 0x70/0x71.
    It is the same legacy block that provides the 8259 interrupt controllers
    and the 8254 timer, all of which this port already owns.

    The clock is kept as separate calendar registers rather than as a seconds
    counter. Their encoding is selected at runtime by control register B:
    BCD or binary, and 24- or 12-hour. Retail consoles and xemu were both
    observed in BCD 24-hour mode, but the driver honors whatever the machine
    reports rather than assuming.

    The year register holds only two digits and there is no century register:
    index 0x32 is ordinary CMOS RAM on this machine, not a century. The epoch
    is therefore a fixed base of 2000, and the last representable timestamp
    before rollover is December 31 2099 23:59:59.

    Two hardware behaviors drive the shape of this interface:

    - The part leaves its calendar registers undefined while an update cycle is
      running, which happens once per second for roughly two milliseconds. A
      retail console produced invalid readings at about 111 per second when
      read without synchronizing against register A's update-in-progress bit.
      Every read here is therefore synchronized and validated.

    - Register D's valid-RAM-and-time bit is not a usable validity signal on
      this machine. The Xbox backs the clock's standby domain with a capacitor
      rather than a battery, and consoles that have lost or had that capacitor
      removed report the bit clear while still holding a perfectly correct
      time. The bit is not consulted.

    \sa kos/rtc.h
*/

/** \brief  Cached wall-clock time from when KallistiOS started.

    Maintained by arch_rtc_init() and re-derived by arch_rtc_set_unix_secs()
    so that time() stays consistent across a clock change.
*/
extern time_t xbox_boot_time;

/** \brief  Read the current date/time from the hardware RTC. */
time_t arch_rtc_unix_secs(void);

/** \brief  Set the current date/time on the hardware RTC. */
int arch_rtc_set_unix_secs(time_t time);

static inline time_t arch_rtc_boot_time(void) {
    return xbox_boot_time;
}

/* \cond INTERNAL */
int arch_rtc_init(void);
void arch_rtc_shutdown(void);
/* \endcond */

__END_DECLS

#endif  /* __ARCH_RTC_H */
