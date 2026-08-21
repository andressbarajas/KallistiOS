/* KallistiOS ##version##

   arch/xbox/kernel/init.c
   Copyright (C) 2026 Cypress
*/

#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>

#include <arch/arch.h>
#include <arch/irq.h>
#include <arch/kosload.h>
#include <arch/mmu.h>
#include <arch/stack.h>
#include <arch/timer.h>
#include <kos/cdefs.h>
#include <kos/dbgio.h>
#include <kos/fs.h>
#include <kos/fs_dev.h>
#include <kos/fs_kosload.h>
#include <kos/kosload.h>
#include <kos/fs_null.h>
#include <kos/fs_pty.h>
#include <kos/fs_ramdisk.h>
#include <kos/fs_random.h>
#include <kos/fs_romdisk.h>
#include <kos/init.h>
#include <kos/library.h>
#include <kos/linker.h>
#include <kos/mm.h>
#include <kos/nmmgr.h>
#include <kos/rtc.h>
#include <kos/thread.h>
#include <xbox/usb.h>
#include "x86.h"

/* Bounds supplied by utils/ldscripts/xbox.ld. */
extern uint8_t _bss_start[];
extern uint8_t _bss_end[];
extern uint8_t _heap_reserve_end__[];
extern uintptr_t arch_old_stack;

/* ctor/dtor stuff from libgcc. The linker builds _init() and _fini() from the
   .init/.fini fragments in crti.o, crtbegin.o, crtend.o and crtn.o, which
   utils/ldscripts/xbox.ld pulls in. _init() also registers the DWARF frame
   table, so no explicit __register_frame() call is needed here. */
extern void _init(void);
extern void _fini(void);

/* Provisional retail hosted-mode ceiling. Keeping this in writable data lets
   later platform discovery replace it before the allocator is initialized. */
uintptr_t _arch_mem_top = 64U * 1024U * 1024U;

/* Native XBE titles have no loader call frame to return through. The Xbox
   kernel patches this standard thunk entry before transferring control.
   Loader-hosted ELF images never call it. */
__attribute__((used, section(".xbethnk")))
static volatile uint32_t xbox_kernel_thunks[2] = {
    0x80000031U, /* HalReturnToFirmware, xboxkrnl export ordinal 49 */
    0,
};

typedef void (__attribute__((stdcall))
              *hal_return_to_firmware_t)(uint32_t routine);

#define XBOX_HAL_QUICK_REBOOT_ROUTINE 2U


_Static_assert(sizeof(x86_descriptor_register_t) == 6,
               "IA-32 descriptor register image must be 6 bytes");

/* Loader stack handoff supplied by startup.S. */
void arch_real_exit(int ret_code) __noreturn;

/* Optional callback installed by KOS_INIT_EARLY(). It must remain in .data:
   arch_main() consults it before clearing .bss. */
void (*__kos_init_early_fn)(void)
    __attribute__((weak, section(".data"))) = NULL;

int main(int argc, char **argv);
extern void __verify_newlib_patch(void);
extern void (*fs_dev_init_weak)(void);
extern void (*fs_dev_shutdown_weak)(void);
extern void (*fs_init_weak)(void);
extern void (*fs_kosload_init_console_weak)(void);
extern void (*fs_kosload_shutdown_weak)(void);
extern void (*fs_null_init_weak)(void);
extern void (*fs_null_shutdown_weak)(void);
extern void (*fs_pty_init_weak)(void);
extern void (*fs_pty_shutdown_weak)(void);
extern void (*fs_ramdisk_init_weak)(void);
extern void (*fs_ramdisk_shutdown_weak)(void);
extern void (*fs_rnd_init_weak)(void);
extern void (*fs_rnd_shutdown_weak)(void);
extern void (*fs_romdisk_init_weak)(void);
extern void (*fs_romdisk_shutdown_weak)(void);
extern void (*fs_shutdown_weak)(void);
extern void (*kosload_init_weak)(void);
extern void (*library_init_weak)(void);
extern void (*library_shutdown_weak)(void);
extern int (*usb_init_weak)(void) __weak_symbol;
extern void (*usb_shutdown_weak)(void) __weak_symbol;

/* Register the loader-backed /pc filesystem only after nmmgr is available. */
void kosload_init(void) {
    if(syscall_kosload_detected())
        fs_kosload_init();
}

/*
 * PE/COFF does not provide ELF-compatible weak function definitions. Supply
 * the default debug-I/O initializer strongly for Xbox instead of relying on
 * dbgio.c's weak override mechanism.
 */
int dbgio_init(void) {
    dbgio_dev_select_auto();
    dbgio_enable();
    return 0;
}

/* Stop safely if an exit backend unexpectedly returns. Interrupts have been
   masked since _start, so HLT cannot race with an uninitialized IRQ path. */
static __noreturn void arch_halt(void) {
    for(;;)
        __asm__ volatile("hlt");
}

static int xbox_auto_init(void) {
    /*
     * A loader-hosted guest has a deliberately bounded mapping, even on a
     * retail 64 MiB machine. This must be selected before mm_init(): both the
     * heap ceiling and newlib's entropy sampler consume _arch_mem_top.
     */
    if(syscall_kosload_detected()) {
        /* The loader has silenced the Xbox kernel, so the machine is ours to
           measure: walk the page tables for what is already mapped, claim what
           we can above it, and stop below the live framebuffer. Nothing here
           asks the loader anything, so the same call serves any host that has
           taken the kernel out of the picture. */
        _arch_mem_top = xbox_mmu_claim((uintptr_t)end);
        if(_arch_mem_top <=
           (uintptr_t)end + THD_KERNEL_STACK_SIZE)
            return -1;
    }
    else {
        uintptr_t native_mem_top = (uintptr_t)_heap_reserve_end__;

        /*
         * A native XBE can use only memory described by its section table.
         * The linker-supplied reservation is therefore authoritative; do
         * not let sbrk advance toward the machine's physical 64 MiB ceiling
         * through virtual pages the title loader never mapped.
         */
        if(native_mem_top <=
           (uintptr_t)end + THD_KERNEL_STACK_SIZE)
            return -1;

        _arch_mem_top = native_mem_top;
    }

    if(mm_init() != 0)
        return -1;

    /*
     * xbox-load-ip is a bare-metal development transport, not an Xbox-kernel
     * service. Initialize it before debug I/O so normal KOS diagnostics and
     * printf output use the same loader channel as the bring-up probes.
     */
    KOS_INIT_FLAG_CALL(fs_kosload_init_console);
    dbgio_add_handler(&dbgio_null);
    dbgio_add_handler(&dbgio_kosload);
    dbgio_init();
    if(__kos_init_flags & INIT_QUIET)
        dbgio_disable();

    if(irq_init() != 0)
        return -1;
    if(timer_init() != 0) {
        irq_shutdown();
        return -1;
    }
    if(rtc_init() != 0) {
        timer_shutdown();
        irq_shutdown();
        return -1;
    }
    if(thd_init() != 0) {
        rtc_shutdown();
        timer_shutdown();
        irq_shutdown();
        return -1;
    }

    /*
     * These are architecture-independent KOS services. In particular,
     * fs_pty_init() creates the console PTY and maps its slave onto file
     * descriptors 0, 1, and 2, making newlib stdio use the selected dbgio
     * backend.
     */
    nmmgr_init();
    KOS_INIT_FLAG_CALL(fs_init);
    KOS_INIT_FLAG_CALL(fs_dev_init);
    KOS_INIT_FLAG_CALL(fs_null_init);
    KOS_INIT_FLAG_CALL(fs_pty_init);
    KOS_INIT_FLAG_CALL(fs_ramdisk_init);
    KOS_INIT_FLAG_CALL(fs_romdisk_init);
    KOS_INIT_FLAG_CALL(fs_rnd_init);
    KOS_INIT_FLAG_CALL(kosload_init);
    KOS_INIT_FLAG_CALL(library_init);

    if(__kos_init_flags & INIT_IRQ)
        irq_enable();

    /*
     * USB enumeration performs timed control transfers and therefore starts
     * only after the scheduler and its timer interrupt are live. Failure is
     * non-fatal so a title can still run and report diagnostics.
     */
    if((__kos_init_flags & INIT_IRQ) && usb_init_weak &&
       (*usb_init_weak)() != 0)
        dbgio_write_str("KOS Xbox warning: USB initialization failed\n");

    return 0;
}

static void xbox_auto_shutdown(void) {
    KOS_INIT_FLAG_CALL(usb_shutdown);
    KOS_INIT_FLAG_CALL(library_shutdown);
    KOS_INIT_FLAG_CALL(fs_kosload_shutdown);
    KOS_INIT_FLAG_CALL(fs_rnd_shutdown);
    KOS_INIT_FLAG_CALL(fs_ramdisk_shutdown);
    KOS_INIT_FLAG_CALL(fs_romdisk_shutdown);
    KOS_INIT_FLAG_CALL(fs_null_shutdown);
    KOS_INIT_FLAG_CALL(fs_dev_shutdown);

    /*
     * Match the portable KOS shutdown workaround: invalidate the descriptor
     * table before dismantling the PTY objects referenced by descriptors 0-2.
     */
    KOS_INIT_FLAG_CALL(fs_shutdown);
    KOS_INIT_FLAG_CALL(fs_pty_shutdown);
    nmmgr_shutdown();

    /*
     * Thread teardown may join and wake internal threads, so keep interrupts
     * available until it has removed the scheduler's primary timer callback.
     */
    thd_shutdown();
    irq_disable();
    rtc_shutdown();
    timer_shutdown();
    irq_shutdown();
}

/* C-level kernel entry point.

   This initializes the portable KOS threading core after establishing the
   Xbox-owned descriptor, interrupt-controller, and timer state it requires. */
void arch_main(void) {
    uint8_t *ptr;
    int rv;

    /* KOS_INIT_EARLY() is specifically defined to run before BSS is cleared. */
    if(__kos_init_early_fn)
        __kos_init_early_fn();

    /* The bootstrap stack and saved loader stack are in .stack, outside these
       bounds, so clearing BSS cannot destroy the active call frames. */
    for(ptr = _bss_start; ptr < _bss_end; ++ptr)
        *ptr = 0;

    if(xbox_auto_init() != 0)
        arch_panic("Xbox KOS initialization failed");

    __verify_newlib_patch();

    /* Run ctors */
    _init();

    /* Call the user's main function */
    rv = main(0, NULL);

    exit(rv);
}

void arch_panic(const char *message) {
    arch_irq_disable();
    dbgio_write_str("KOS Xbox panic: ");
    dbgio_write_str(message);
    dbgio_write_str("\n");
    arch_halt();
}

void arch_set_exit_path(int path) {
    if(path != ARCH_EXIT_RETURN)
        arch_panic("Xbox exit path is not implemented");
}

void arch_exit(void) {
    exit(EXIT_SUCCESS);
}

void arch_return(int ret_code) {
    arch_real_exit(ret_code);
}

void arch_abort(void) {
    arch_panic("abort");
}

void arch_reboot(void) {
    arch_panic("Xbox reboot path is not implemented");
}

void arch_menu(void) {
    arch_panic("Xbox dashboard path is not implemented");
}

/* Entry used by newlib's _exit(). */
void arch_exit_handler(int ret_code) {
    bool loader_hosted = syscall_kosload_detected();

    /* Run dtors */
    _fini();

    xbox_auto_shutdown();

    /*
     * The loader draws its console through the write-combined alias and only
     * evicts stale lines when it first brings video up, not on the re-pin it
     * does each poll. Anything we touched through the cached mapping is still
     * sitting dirty in L1/L2 and would write back over its pixels after we are
     * gone, so flush before handing control back.
     */
    if(loader_hosted) {
        xbox_mmu_release();
        __asm__ __volatile__("wbinvd" ::: "memory");
        kosload_exit();
        arch_real_exit(ret_code);
    }

    ((hal_return_to_firmware_t)(uintptr_t)xbox_kernel_thunks[0])(
        XBOX_HAL_QUICK_REBOOT_ROUTINE);
    arch_halt();
}
