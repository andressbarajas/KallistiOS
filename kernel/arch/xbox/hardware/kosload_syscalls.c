/* KallistiOS ##version##

   kosload_syscalls.c

   Copyright (C) 2026 Andress Barajas
   Copyright (C) 2026 Cypress

*/

#include <kos/kosload.h>

#include <arch/kosload.h>

#include <kos/irq.h>

/* This is the address where the function pointer for the kosload syscall is
   fetched from. The Xbox loader stores it directly after the magic value. */
#define VEC_KOSLOAD       (KOSLOAD_BASE_ADDR + 0x4)

/*
    This is the single syscall kosload provides. It is then multiplexed out based on the `cmd`
    parameter.
*/

int kosload_syscall_native(kosload_cmd_t cmd, void *param1, void *param2, void *param3) {
    uintptr_t *syscall_ptr = (uintptr_t *)VEC_KOSLOAD;
    int (*syscall)() = (int (*)())(*syscall_ptr);

    /* Disable IRQs until the syscall returns */
    irq_disable_scoped();

    /* x86 has no serial FIFO to drain before the call. */

    /* Make the call */
    return syscall(cmd, param1, param2, param3);
}
