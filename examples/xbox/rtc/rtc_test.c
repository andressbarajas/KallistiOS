/* KallistiOS ##version##

   rtc_test.c
   Copyright (C) 2026 Cypress

*/

/* This example demonstrates the portable KOS real-time clock interface on the
   original Xbox. It deliberately uses only the public API in <kos/rtc.h> and
   the standard C time functions; nothing here touches a CMOS register.

   Three ways of asking the time are compared:

     time()           standard C, served from the cached boot time plus the
                      timer subsystem's delta, so it is cheap
     rtc_unix_secs()  a real synchronized read of the hardware clock
     rtc_boot_time()  the wall clock captured when KOS started

   The example never calls rtc_set_unix_secs(). Setting the clock would change
   the console's own date and time, which is not something a demo should do. */

#include <kos/init.h>
#include <kos/rtc.h>
#include <kos/thread.h>

#include <stdio.h>
#include <time.h>

#define SAMPLE_COUNT    5
#define SAMPLE_DELAY_MS 1000

static void print_stamp(const char *label, time_t stamp) {
    char text[32];
    struct tm broken_down;

    /* The RTC carries no time-zone information, so its value is local time by
       definition. gmtime_r() is used to format it verbatim rather than
       applying a second conversion on top of that. */
    if(gmtime_r(&stamp, &broken_down) == NULL ||
       strftime(text, sizeof(text), "%Y-%m-%d %H:%M:%S", &broken_down) == 0) {
        printf("%-16s %lld (unformattable)\n", label, (long long)stamp);
        return;
    }

    printf("%-16s %lld  %s\n", label, (long long)stamp, text);
}

int main(int argc, char *argv[]) {
    time_t boot;
    time_t previous;
    time_t hardware;
    time_t standard;
    int advanced = 0;
    int regressed = 0;
    int i;

    (void)argc;
    (void)argv;

    printf("KOS Xbox RTC example\n\n");

    boot = rtc_boot_time();
    print_stamp("boot time", boot);

    if(boot == 0) {
        /* arch_rtc_init() reports a zero boot time when the clock could not be
           read coherently. The rest of the example still runs, but the epoch
           is meaningless, so say so rather than printing 1970 dates as if they
           were real. */
        printf("\nThe RTC could not be read at startup, so the epoch below is\n"
               "system boot rather than a real date.\n");
    }

    hardware = rtc_unix_secs();
    standard = time(NULL);
    print_stamp("rtc_unix_secs", hardware);
    print_stamp("time()", standard);

    /* The two paths read the same clock by different routes, so they should
       agree to within the granularity of a one-second clock. */
    printf("\ncached vs hardware difference: %lld second(s)\n",
           (long long)(standard - hardware));

    printf("\nSampling the hardware clock once per second:\n");
    previous = hardware;

    for(i = 0; i < SAMPLE_COUNT; i++) {
        thd_sleep(SAMPLE_DELAY_MS);

        hardware = rtc_unix_secs();
        print_stamp("  sample", hardware);

        if(hardware > previous)
            advanced++;
        else if(hardware < previous)
            regressed++;

        previous = hardware;
    }

    printf("\nadvanced %d time(s), went backwards %d time(s)\n",
           advanced, regressed);
    printf("uptime by boot-time delta: %lld second(s)\n",
           (long long)(rtc_unix_secs() - rtc_boot_time()));

    if(regressed != 0) {
        printf("\nFAIL: the wall clock is not monotonic\n");
        return 1;
    }

    printf("\nPASS\n");
    return 0;
}
