/* KallistiOS ##version##

   arch/dreamcast/include/arch/dcload.h
   Copyright (C) 2025 Donald Haase
   Copyright (C) 2026 Andy Barajas

*/

/** \file    arch/dcload.h
    \brief   Dreamcast-specific dc-load memory area.
    \ingroup vfs_dcload

    Describes where dc-load lives in RAM on the Dreamcast, which is all that is
    needed in order to detect the connection.  The syscall transport itself is
    in arch/dreamcast/hardware/dcload_syscalls.c.
*/

#ifndef __ARCH_DCLOAD_H
#define __ARCH_DCLOAD_H

#include <stdint.h>

/** \brief  Size of the dc-load area reserved in bytes (45 KB) */
#define DCLOAD_SIZE          (45 * 1024)

/** \brief  Base address of the dc-load area in RAM */
#define DCLOAD_BASE_ADDR     0x8c004000

/** \brief  Address of the dc-load magic value in RAM */
#define DCLOAD_MAGIC_ADDR    (DCLOAD_BASE_ADDR + 0x4)

#endif  /* __ARCH_DCLOAD_H */
