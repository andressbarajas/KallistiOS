/* KallistiOS ##version##

   arch/xbox/hardware/rtc.c
   Copyright (C) 2026 Cypress
*/

/*
   Real-Time Clock (RTC) Support

   The functions in here return various info about the real-world time and
   date stored in the machine. The general process here is to retrieve the
   date/time value and then use the other functions to interpret it.

   The Xbox RTC is an MC146818-compatible part inside the MCPX southbridge,
   addressed through the legacy index/data port pair 0x70/0x71. Unlike the
   Dreamcast's single 32-bit seconds counter, it presents separate calendar
   registers whose encoding is chosen at runtime by control register B, so
   the driver reads that configuration instead of assuming one.

   Three hardware properties shape this code, all established by measurement
   on a retail console rather than inherited from PC convention:

   1. The calendar registers are undefined while an update cycle is running.
      Reading them without synchronizing against register A's update-in-progress
      bit produced roughly 111 invalid readings per second on hardware. Reads
      here wait out the update window, take two passes that must agree, and
      then validate encoding and range. Two agreeing passes alone are not
      sufficient, because both can land inside the same update window.

   2. Register D's valid-RAM-and-time bit is not a validity signal here. The
      clock's standby domain is backed by a capacitor rather than a battery,
      and consoles missing that capacitor report the bit clear while holding a
      correct time. It is deliberately never consulted.

   3. Index 0x70 bit 7 is the chipset NMI mask, so it is rewritten as a side
      effect of selecting any register. See the shadow discussion below.
 */

#include <arch/rtc.h>
#include <arch/irq.h>

#include <kos/mutex.h>
#include <kos/timer.h>

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>

/* Legacy CMOS index/data ports. */
#define CMOS_INDEX_PORT         0x70U
#define CMOS_DATA_PORT          0x71U

/* Bit 7 of the index port masks NMI rather than selecting a register. */
#define CMOS_NMI_DISABLE        0x80U
#define CMOS_INDEX_MASK         0x7fU

/* Clock and control register indexes. */
#define RTC_SECONDS             0x00U
#define RTC_MINUTES             0x02U
#define RTC_HOURS               0x04U
#define RTC_DAY_OF_WEEK         0x06U
#define RTC_DAY_OF_MONTH        0x07U
#define RTC_MONTH               0x08U
#define RTC_YEAR                0x09U
#define RTC_REG_A               0x0aU
#define RTC_REG_B               0x0bU
#define RTC_REG_D               0x0dU

#define RTC_A_UIP               0x80U
#define RTC_B_24HOUR            0x02U
#define RTC_B_BINARY            0x04U
#define RTC_B_SET               0x80U

/* In 12-hour mode the hours register carries a PM flag outside its value. */
#define RTC_HOUR_PM             0x80U

/*
   Epoch base.

   The year register holds two digits and this machine has no century
   register, so the representable range is fixed.
*/
#define RTC_BASE_YEAR           2000
#define RTC_LAST_YEAR           2099
#define RTC_UNIX_MIN            946684800LL   /* 2000-01-01 00:00:00 */
#define RTC_UNIX_MAX            4102444799LL  /* 2099-12-31 23:59:59 */

/*
    # of Read/Write Retry Attempts

    To ensure a coherent, race-free read/write operation.
*/
#define RTC_RETRY_COUNT         3

/* Coherent-read attempts, and the bound on one update-window wait. */
#define RTC_COHERENT_ATTEMPTS   16
#define RTC_UIP_TIMEOUT_US      20000U

/* The boot time; we'll save this in arch_rtc_init() */
time_t xbox_boot_time;

/*
   Software shadow of index-port bit 7.

   Selecting any register rewrites the NMI mask, so the driver must decide what
   that bit becomes. The inherited value cannot simply be read back: a retail
   console returns the last written index in bits 0-6 but always reports bit 7
   as zero, and xemu returns 0xff for the whole port. The shadow therefore
   defaults to "NMI enabled", which is both the ordinary x86 state and the
   choice that leaves the machine least disturbed, and KOS owns every IDT
   vector including NMI while it runs.
*/
static uint8_t rtc_nmi_shadow;

/* Inherited index, restored at shutdown on machines that read it back. */
static uint8_t rtc_saved_index;
static bool rtc_index_restorable;

/*
   Offset between this machine's day-of-week register and a Sunday-is-zero
   weekday, learned from the boot reading.

   The register's base is not a shared convention: a retail console and xemu
   reported different values for the same date. It is never used to tell the
   time, but it must be kept consistent when writing, because retail consoles
   were observed with the chip's daylight-saving enable set and that logic
   uses the weekday to find its changeover days.
*/
static uint8_t rtc_dow_base;
static bool rtc_dow_known;

static bool rtc_initialized;

/* Serialize complete calendar transactions. The low-level index/data helpers
   only protect a single register access, while coherent reads and RTC writes
   span several registers. arch_rtc_init() runs before the thread system and
   deliberately uses the unlocked helpers directly. */
static mutex_t rtc_lock = MUTEX_INITIALIZER;

static inline void rtc_out8(uint16_t port, uint8_t value) {
    __asm__ volatile("outb %0, %1" : : "a"(value), "Nd"(port) : "memory");
}

static inline uint8_t rtc_in8(uint16_t port) {
    uint8_t value;

    __asm__ volatile("inb %1, %0" : "=a"(value) : "Nd"(port) : "memory");
    return value;
}

/*
   Index selection and data access must not be separated, because the index is
   shared state. Interrupts are masked across the pair rather than around whole
   register sequences, so a caller waiting out an update window never holds
   them off for milliseconds.
*/
static uint8_t rtc_read(uint8_t index) {
    irq_mask_t mask = irq_disable();
    uint8_t value;

    rtc_out8(CMOS_INDEX_PORT, (uint8_t)(index | rtc_nmi_shadow));
    value = rtc_in8(CMOS_DATA_PORT);
    irq_restore(mask);

    return value;
}

static void rtc_write(uint8_t index, uint8_t value) {
    irq_mask_t mask = irq_disable();

    rtc_out8(CMOS_INDEX_PORT, (uint8_t)(index | rtc_nmi_shadow));
    rtc_out8(CMOS_DATA_PORT, value);
    irq_restore(mask);
}

static uint32_t bcd_to_binary(uint8_t value) {
    return (uint32_t)(value & 0x0fU) + ((uint32_t)(value >> 4) * 10U);
}

static uint8_t binary_to_bcd(uint32_t value) {
    return (uint8_t)(((value / 10U) << 4) | (value % 10U));
}

static bool bcd_valid(uint8_t value) {
    return (value & 0x0fU) <= 9U && ((value >> 4) & 0x0fU) <= 9U;
}

/* Wait out an update cycle. Returns false if one did not end in time. */
static bool rtc_wait_update_done(void) {
    uint64_t deadline = timer_us_gettime64() + RTC_UIP_TIMEOUT_US;

    while((rtc_read(RTC_REG_A) & RTC_A_UIP) != 0) {
        if(timer_us_gettime64() >= deadline)
            return false;
    }

    return true;
}

/* A decoded calendar reading, still in local time and without an epoch. */
typedef struct rtc_fields {
    uint32_t second;
    uint32_t minute;
    uint32_t hour;
    uint32_t day_of_month;
    uint32_t month;
    uint32_t year;
    uint8_t  day_of_week_raw;
} rtc_fields_t;

/* Raw register order used by every read and comparison below. */
static const uint8_t rtc_clock_registers[] = {
    RTC_SECONDS, RTC_MINUTES, RTC_HOURS,
    RTC_DAY_OF_WEEK, RTC_DAY_OF_MONTH, RTC_MONTH, RTC_YEAR
};

#define RTC_CLOCK_REGISTER_COUNT \
    (sizeof(rtc_clock_registers) / sizeof(rtc_clock_registers[0]))

static void rtc_read_raw(uint8_t *raw) {
    size_t i;

    for(i = 0; i < RTC_CLOCK_REGISTER_COUNT; i++)
        raw[i] = rtc_read(rtc_clock_registers[i]);
}

static bool rtc_raw_equal(const uint8_t *a, const uint8_t *b) {
    size_t i;

    for(i = 0; i < RTC_CLOCK_REGISTER_COUNT; i++)
        if(a[i] != b[i])
            return false;

    return true;
}

static void rtc_decode(const uint8_t *raw, uint8_t reg_b, rtc_fields_t *out) {
    bool binary = (reg_b & RTC_B_BINARY) != 0;
    uint8_t hour_raw = raw[2];
    bool pm = false;

    if(!(reg_b & RTC_B_24HOUR)) {
        pm = (hour_raw & RTC_HOUR_PM) != 0;
        hour_raw = (uint8_t)(hour_raw & ~RTC_HOUR_PM);
    }

    out->second = binary ? raw[0] : bcd_to_binary(raw[0]);
    out->minute = binary ? raw[1] : bcd_to_binary(raw[1]);
    out->hour = binary ? hour_raw : bcd_to_binary(hour_raw);
    out->day_of_week_raw = raw[3];
    out->day_of_month = binary ? raw[4] : bcd_to_binary(raw[4]);
    out->month = binary ? raw[5] : bcd_to_binary(raw[5]);
    out->year = (binary ? raw[6] : bcd_to_binary(raw[6])) + RTC_BASE_YEAR;

    if(!(reg_b & RTC_B_24HOUR)) {
        /* 12 AM is midnight and 12 PM is noon. */
        if(out->hour == 12U)
            out->hour = 0U;
        if(pm)
            out->hour += 12U;
    }
}

static bool rtc_fields_valid(const rtc_fields_t *f) {
    return f->second <= 59U &&
           f->minute <= 59U &&
           f->hour <= 23U &&
           f->day_of_month >= 1U && f->day_of_month <= 31U &&
           f->month >= 1U && f->month <= 12U &&
           f->year >= RTC_BASE_YEAR && f->year <= RTC_LAST_YEAR;
}

/*
   Is a raw reading self-consistent?

   This is the test that separates a real reading from update-window garbage,
   which is why it checks the encoding as well as the decoded ranges.
*/
static bool rtc_raw_plausible(const uint8_t *raw, uint8_t reg_b,
                              rtc_fields_t *out) {
    size_t i;

    if(!(reg_b & RTC_B_BINARY)) {
        for(i = 0; i < RTC_CLOCK_REGISTER_COUNT; i++) {
            uint8_t value = raw[i];

            if(rtc_clock_registers[i] == RTC_HOURS && !(reg_b & RTC_B_24HOUR))
                value = (uint8_t)(value & ~RTC_HOUR_PM);

            if(!bcd_valid(value))
                return false;
        }
    }

    rtc_decode(raw, reg_b, out);
    return rtc_fields_valid(out);
}

/* Days since 1970-01-01 for a Gregorian date. Era-based, so there are no
   month tables and no leap-year special cases beyond the era arithmetic. */
static int64_t rtc_days_from_civil(int64_t year, uint32_t month,
                                   uint32_t day) {
    int64_t era;
    uint32_t year_of_era;
    uint32_t day_of_year;
    uint32_t day_of_era;

    year -= (month <= 2);
    era = (year >= 0 ? year : year - 399) / 400;
    year_of_era = (uint32_t)(year - era * 400);
    day_of_year = (153U * (month + (month > 2 ? (uint32_t)-3 : 9U)) + 2U) / 5U +
                  day - 1U;
    day_of_era = year_of_era * 365U + year_of_era / 4U - year_of_era / 100U +
                 day_of_year;

    return era * 146097 + (int64_t)day_of_era - 719468;
}

/* Inverse of rtc_days_from_civil(). */
static void rtc_civil_from_days(int64_t days, int64_t *year, uint32_t *month,
                                uint32_t *day) {
    int64_t era;
    uint32_t day_of_era;
    uint32_t year_of_era;
    int64_t y;
    uint32_t day_of_year;
    uint32_t mp;

    days += 719468;
    era = (days >= 0 ? days : days - 146096) / 146097;
    day_of_era = (uint32_t)(days - era * 146097);
    year_of_era = (day_of_era - day_of_era / 1460U + day_of_era / 36524U -
                   day_of_era / 146096U) / 365U;
    y = (int64_t)year_of_era + era * 400;
    day_of_year = day_of_era - (365U * year_of_era + year_of_era / 4U -
                                year_of_era / 100U);
    mp = (5U * day_of_year + 2U) / 153U;

    *day = day_of_year - (153U * mp + 2U) / 5U + 1U;
    *month = mp + (mp < 10U ? 3U : (uint32_t)-9);
    *year = y + (*month <= 2U);
}

/* Sunday is zero. 1970-01-01 was a Thursday. */
static uint32_t rtc_weekday_from_days(int64_t days) {
    int64_t weekday = (days + 4) % 7;

    if(weekday < 0)
        weekday += 7;

    return (uint32_t)weekday;
}

static time_t rtc_fields_to_unix(const rtc_fields_t *f) {
    int64_t days = rtc_days_from_civil((int64_t)f->year, f->month,
                                       f->day_of_month);

    return (time_t)(days * 86400 +
                    (int64_t)f->hour * 3600 +
                    (int64_t)f->minute * 60 +
                    (int64_t)f->second);
}

/*
   Bounded, update-synchronized calendar read.

   Returns false only when no self-consistent reading could be obtained within
   the attempt budget.
*/
static bool rtc_read_fields(rtc_fields_t *out) {
    uint8_t first[RTC_CLOCK_REGISTER_COUNT];
    uint8_t second[RTC_CLOCK_REGISTER_COUNT];
    uint8_t reg_b;
    int attempt;

    for(attempt = 0; attempt < RTC_COHERENT_ATTEMPTS; attempt++) {
        if(!rtc_wait_update_done())
            continue;

        reg_b = rtc_read(RTC_REG_B);
        rtc_read_raw(first);
        rtc_read_raw(second);

        if(!rtc_raw_equal(first, second))
            continue;

        /* An update must not have begun while the passes were running. */
        if((rtc_read(RTC_REG_A) & RTC_A_UIP) != 0)
            continue;

        if(!rtc_raw_plausible(first, reg_b, out))
            continue;

        return true;
    }

    return false;
}

static time_t rtc_unix_secs_unlocked(void) {
    rtc_fields_t fields;

    if(!rtc_initialized || !rtc_read_fields(&fields)) {
        /*
           The portable interface has no way to report a failed read, and
           returning a wild value would be worse than returning a consistent
           one. Fall back to the cached boot time advanced by uptime, which
           keeps the clock monotonic and plausible.
        */
        uint32_t secs;
        uint32_t msecs;

        timer_ms_gettime(&secs, &msecs);
        return xbox_boot_time + (time_t)secs;
    }

    return rtc_fields_to_unix(&fields);
}

/* Returns the date/time value as a UNIX epoch time stamp */
time_t arch_rtc_unix_secs(void) {
    time_t result;

    mutex_lock(&rtc_lock);
    result = rtc_unix_secs_unlocked();
    mutex_unlock(&rtc_lock);

    return result;
}

/* Sets the date/time value from a UNIX epoch time stamp,
   returning 0 for success or -1 for failure. */
int arch_rtc_set_unix_secs(time_t secs) {
    rtc_fields_t verify;
    int64_t days;
    int64_t year;
    uint32_t month;
    uint32_t day;
    uint32_t remainder;
    uint32_t hour;
    uint32_t minute;
    uint32_t second;
    uint8_t original_reg_b;
    bool binary;
    uint32_t stored_hour;
    bool pm = false;
    uint32_t secs32;
    uint32_t msecs32;
    int attempt;
    int result = 0;

    if(!rtc_initialized) {
        errno = ENODEV;
        return -1;
    }

    /* Protect against a timestamp the two-digit year cannot represent. */
    if((int64_t)secs < RTC_UNIX_MIN || (int64_t)secs > RTC_UNIX_MAX) {
        errno = EINVAL;
        return -1;
    }

    mutex_lock(&rtc_lock);

    days = (int64_t)secs / 86400;
    remainder = (uint32_t)((int64_t)secs - days * 86400);
    rtc_civil_from_days(days, &year, &month, &day);

    hour = remainder / 3600U;
    minute = (remainder / 60U) % 60U;
    second = remainder % 60U;

    original_reg_b = rtc_read(RTC_REG_B);
    binary = (original_reg_b & RTC_B_BINARY) != 0;

    stored_hour = hour;
    if(!(original_reg_b & RTC_B_24HOUR)) {
        pm = hour >= 12U;
        stored_hour = hour % 12U;
        if(stored_hour == 0U)
            stored_hour = 12U;
    }

    for(attempt = 0; attempt < RTC_RETRY_COUNT; attempt++) {
        uint8_t hour_value;

        /*
           Halt the update cycle while the calendar is inconsistent. The rest
           of register B, in particular the daylight-saving enable that retail
           consoles ship with set, is preserved exactly.
        */
        rtc_write(RTC_REG_B, (uint8_t)(original_reg_b | RTC_B_SET));

        hour_value = binary ? (uint8_t)stored_hour : binary_to_bcd(stored_hour);
        if(pm)
            hour_value = (uint8_t)(hour_value | RTC_HOUR_PM);

        rtc_write(RTC_SECONDS, binary ? (uint8_t)second : binary_to_bcd(second));
        rtc_write(RTC_MINUTES, binary ? (uint8_t)minute : binary_to_bcd(minute));
        rtc_write(RTC_HOURS, hour_value);
        rtc_write(RTC_DAY_OF_MONTH,
                  binary ? (uint8_t)day : binary_to_bcd(day));
        rtc_write(RTC_MONTH, binary ? (uint8_t)month : binary_to_bcd(month));
        rtc_write(RTC_YEAR,
                  binary ? (uint8_t)(year - RTC_BASE_YEAR)
                         : binary_to_bcd((uint32_t)(year - RTC_BASE_YEAR)));

        /*
           Keep the weekday register consistent with the date. It is never
           used to tell the time, but the chip's daylight-saving logic uses it
           to locate its changeover days, so leaving it stale after a set would
           be a real hazard on a console that ships with that enable set. The
           register's base is learned from the machine rather than assumed.
        */
        if(rtc_dow_known) {
            uint32_t weekday = rtc_weekday_from_days(days) + rtc_dow_base;

            rtc_write(RTC_DAY_OF_WEEK,
                      binary ? (uint8_t)weekday : binary_to_bcd(weekday));
        }

        rtc_write(RTC_REG_B, original_reg_b);

        /* Read the time back again, to ensure it was written properly. */
        if(rtc_read_fields(&verify) &&
           rtc_fields_to_unix(&verify) == secs)
            break;
    }

    /* Signify failure if the fetched time never matched the
       time we attempted to set. */
    if(attempt == RTC_RETRY_COUNT) {
        errno = EPERM;
        result = -1;
    }

    /*
       We have to update the boot time now as well, subtracting the amount of
       time that has elapsed since boot from the new time we've just set.
    */
    timer_ms_gettime(&secs32, &msecs32);
    xbox_boot_time = rtc_unix_secs_unlocked() - (time_t)secs32;

    mutex_unlock(&rtc_lock);

    return result;
}

int arch_rtc_init(void) {
    rtc_fields_t fields;
    uint8_t probe_index;
    uint8_t readback;
    uint8_t reg_b;
    uint32_t stored_weekday;
    uint32_t true_weekday;

    rtc_nmi_shadow = 0;
    rtc_index_restorable = false;
    rtc_dow_known = false;

    /*
       Capture the inherited register selection so shutdown can put it back.
       Whether the index port reads back at all is machine-specific: a retail
       console returns the last written index, xemu returns 0xff. Prove the
       port is readable before trusting the value that was sampled from it.
    */
    probe_index = rtc_in8(CMOS_INDEX_PORT);
    rtc_out8(CMOS_INDEX_PORT, RTC_REG_D);
    readback = rtc_in8(CMOS_INDEX_PORT);

    if((readback & CMOS_INDEX_MASK) == RTC_REG_D) {
        rtc_saved_index = (uint8_t)(probe_index & CMOS_INDEX_MASK);
        rtc_index_restorable = true;
    }

    rtc_initialized = true;

    /*
       Cache the boot time. A machine whose clock cannot be read coherently is
       not a fatal condition: KOS still runs, time() still advances, and the
       epoch simply falls back to zero the way the pre-driver port behaved.
    */
    if(rtc_read_fields(&fields)) {
        xbox_boot_time = rtc_fields_to_unix(&fields);

        /*
           Learn the weekday register's base from this reading. Only the two
           bases seen in practice are accepted; anything else means the
           register is not trustworthy and it will not be written.
        */
        reg_b = rtc_read(RTC_REG_B);
        stored_weekday = (reg_b & RTC_B_BINARY)
                         ? fields.day_of_week_raw
                         : bcd_to_binary(fields.day_of_week_raw);
        true_weekday = rtc_weekday_from_days(
            rtc_days_from_civil((int64_t)fields.year, fields.month,
                                fields.day_of_month));

        if(stored_weekday == true_weekday ||
           stored_weekday == true_weekday + 1U) {
            rtc_dow_base = (uint8_t)(stored_weekday - true_weekday);
            rtc_dow_known = true;
        }
    }
    else {
        xbox_boot_time = 0;
    }

    return 0;
}

void arch_rtc_shutdown(void) {
    if(!rtc_initialized)
        return;

    /*
       Put the register selection back the way the Xbox kernel left it. The
       NMI mask cannot be restored because bit 7 does not read back on this
       hardware; the shadow keeps NMI enabled throughout, which is the state
       the machine is overwhelmingly likely to have been in.
    */
    if(rtc_index_restorable)
        rtc_out8(CMOS_INDEX_PORT, (uint8_t)(rtc_saved_index | rtc_nmi_shadow));

    rtc_initialized = false;
}
