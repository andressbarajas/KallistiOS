/* KallistiOS ##version##

   smc_test.c
   Copyright (C) 2026 Cypress

*/

/* This example demonstrates the Xbox System Management Controller status
   interface in <xbox/smc.h>.

   It reports what the SMC can tell you about the machine: which AV pack is
   attached, whether there is a disc in the tray, how warm the console is, and
   which SMC firmware it is running.

   Nothing here commands the SMC. There is no way to do so through this API,
   deliberately: powering off, resetting, ejecting the tray, changing the fan
   or the LED are all absent. */

#include <kos/init.h>
#include <kos/thread.h>
#include <xbox/smc.h>

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define TEMPERATURE_SAMPLES 5
#define SAMPLE_DELAY_MS     500
#define VERSION_THREADS     4
#define VERSION_ITERATIONS  32

static int failures;
static char expected_version[XBOX_SMC_VERSION_LENGTH];

static const char *tray_name(xbox_smc_tray_state_t state) {
    switch(state) {
        case XBOX_SMC_TRAY_CLOSED:         return "closed";
        case XBOX_SMC_TRAY_OPEN:           return "open";
        case XBOX_SMC_TRAY_NO_MEDIA:       return "closed, no media";
        case XBOX_SMC_TRAY_MEDIA_DETECTED: return "closed, media detected";
        case XBOX_SMC_TRAY_UNKNOWN:        return "unrecognized";
        default:                           return "invalid";
    }
}

static const char *av_pack_name(xbox_smc_av_pack_t pack) {
    switch(pack) {
        case XBOX_SMC_AV_SCART:     return "SCART";
        case XBOX_SMC_AV_HDTV:      return "component HDTV";
        case XBOX_SMC_AV_VGA:       return "VGA";
        case XBOX_SMC_AV_RFU:       return "reserved";
        case XBOX_SMC_AV_SVIDEO:    return "S-Video";
        case XBOX_SMC_AV_COMPOSITE: return "composite";
        case XBOX_SMC_AV_NONE:      return "none attached";
        case XBOX_SMC_AV_UNKNOWN:   return "unrecognized";
        default:                    return "invalid";
    }
}

static void report_failure(const char *what) {
    printf("  %-22s FAILED (errno=%d)\n", what, errno);
    failures++;
}

static void *version_reader(void *arg) {
    char version[XBOX_SMC_VERSION_LENGTH];
    int i;

    (void)arg;

    for(i = 0; i < VERSION_ITERATIONS; i++) {
        if(xbox_smc_version(version) != 0 ||
           memcmp(version, expected_version, sizeof(version)) != 0)
            return (void *)(uintptr_t)1;
    }

    return NULL;
}

int main(int argc, char *argv[]) {
    kthread_t *version_threads[VERSION_THREADS] = { NULL };
    xbox_smc_tray_state_t tray;
    xbox_smc_av_pack_t pack;
    char version[XBOX_SMC_VERSION_LENGTH + 1];
    uint8_t cpu;
    uint8_t board;
    uint8_t error_code;
    void *thread_result;
    int i;

    (void)argc;
    (void)argv;

    printf("KOS Xbox SMC example\n\n");

    if(xbox_smc_version(version) == 0) {
        memcpy(expected_version, version, sizeof(expected_version));
        version[XBOX_SMC_VERSION_LENGTH] = '\0';
        printf("  %-22s %s\n", "SMC version", version);

        for(i = 0; i < VERSION_THREADS; i++) {
            version_threads[i] = thd_create(false, version_reader, NULL);
            if(version_threads[i] == NULL) {
                report_failure("version thread create");
                break;
            }
        }

        for(i = 0; i < VERSION_THREADS; i++) {
            if(version_threads[i] == NULL)
                break;

            thread_result = NULL;
            if(thd_join(version_threads[i], &thread_result) != 0 ||
               thread_result != NULL)
                report_failure("concurrent version read");
        }
    }
    else {
        report_failure("SMC version");
    }

    if(xbox_smc_tray_state(&tray) == 0)
        printf("  %-22s %s\n", "tray", tray_name(tray));
    else
        report_failure("tray");

    if(xbox_smc_av_pack(&pack) == 0)
        printf("  %-22s %s\n", "AV pack", av_pack_name(pack));
    else
        report_failure("AV pack");

    if(xbox_smc_error_code(&error_code) == 0)
        printf("  %-22s 0x%02x\n", "SMC error code", error_code);
    else
        report_failure("SMC error code");

    printf("\nTemperatures:\n");
    for(i = 0; i < TEMPERATURE_SAMPLES; i++) {
        int cpu_ok = xbox_smc_cpu_temperature(&cpu) == 0;
        int board_ok = xbox_smc_board_temperature(&board) == 0;

        if(!cpu_ok || !board_ok) {
            report_failure("temperature sample");
            break;
        }

        printf("  sample %d: cpu %u C, board %u C\n",
               i, (unsigned)cpu, (unsigned)board);

        thd_sleep(SAMPLE_DELAY_MS);
    }

    /* A NULL argument must be rejected rather than dereferenced. */
    errno = 0;
    if(xbox_smc_tray_state(NULL) != -1 || errno != EINVAL) {
        printf("\n  NULL argument was not rejected with EINVAL\n");
        failures++;
    }

    if(failures != 0) {
        printf("\nXBOX_SMC_TEST_FAIL failures=%d\n", failures);
        return 1;
    }

    printf("\nXBOX_SMC_TEST_PASS\n");
    return 0;
}
