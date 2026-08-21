/* KallistiOS ##version##

   arch/xbox/include/arch/kosload.h
   Copyright (C) 2026 Andress Barajas
   Copyright (C) 2026 Cypress

*/

/** \file    arch/kosload.h
    \brief   Xbox-specific kos-load memory area.
    \ingroup vfs_kosload

    Describes where kos-load (kos-tool's xbox-load-ip) lives in RAM on the
    original Xbox, which is all the portable driver needs in order to detect
    the connection.  The native syscall transport itself is in
    arch/xbox/hardware/kosload_syscalls.c.

    The Xbox loader publishes a small guest-facing header at a fixed low-memory
    address: the magic value at +0 and the syscall function pointer at +4.  The
    Dreamcast instead keeps those two words at unrelated BIOS-RAM addresses, so
    only the magic address is described here and the vector is derived from the
    same base in kosload_syscalls.c.

    Included automatically by kernel/kosload.c via the arch-specific include
    path; do not include directly.
*/

#ifndef __ARCH_KOSLOAD_H
#define __ARCH_KOSLOAD_H

#include <stdint.h>

/** \brief  Base address of the kos-load header in RAM */
#define KOSLOAD_BASE_ADDR     0x00011000

/** \brief  Address of the kos-load magic value in RAM */
#define KOSLOAD_MAGIC_ADDR    (KOSLOAD_BASE_ADDR + 0x0)

#endif  /* __ARCH_KOSLOAD_H */
