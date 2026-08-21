/* KallistiOS ##version##

   kernel/arch/xbox/include/xbox/memory.h
   Copyright (C) 2026 Cypress

*/

/** \file    xbox/memory.h
    \brief   Constants for areas of the Xbox system memory map.

    The NV2A shares main RAM with the CPU: there is no separate video memory,
    so the framebuffer is an ordinary allocation the display engine scans out
    of. Physical RAM is 64 MiB on retail consoles and 128 MiB on debug kits and
    modded units, and is aliased into several apertures.

    \author Cypress
*/

#ifndef __XBOX_MEMORY_H
#define __XBOX_MEMORY_H

#include <sys/cdefs.h>
__BEGIN_DECLS

#include <stdint.h>

/** \defgroup xbox_memory_map  Memory Map
    \brief    Physical layout and apertures of the Xbox address space
    @{
*/

/** \brief Base of physical RAM. */
#define XBOX_MEM_PHYS_BASE      0x00000000U

/** \brief Physical RAM top on a retail console (64 MiB). */
#define XBOX_MEM_PHYS_TOP_RETAIL 0x04000000U

/** \brief Physical RAM top on a debug kit or modded unit (128 MiB). */
#define XBOX_MEM_PHYS_TOP_DEBUG  0x08000000U

/** \brief Base of the cached alias of physical RAM.

    Sparsely populated under the loader; do not assume it maps all of RAM.
*/
#define XBOX_MEM_CACHED_BASE    0x80000000U

/** \brief Base of the write-combined alias of physical RAM.

    Sized to installed RAM: ends at 0xF3FFFFFF with 64 MiB, 0xF7FFFFFF with
    128 MiB. The loader draws its console through this aperture.
*/
#define XBOX_MEM_WC_BASE        0xF0000000U

/** \brief Convert a physical address to its write-combined alias. */
#define XBOX_MEM_TO_WC(phys)    (XBOX_MEM_WC_BASE | (uint32_t)(phys))

/** \brief Base of NV2A (GPU) MMIO registers. */
#define XBOX_MEM_NV2A_BASE      0xFD000000U

/** \brief Base of APU MMIO registers. */
#define XBOX_MEM_APU_BASE       0xFE800000U

/** \brief Base of flash ROM. */
#define XBOX_MEM_FLASH_BASE     0xFF000000U

/** @} */

__END_DECLS
#endif  /* __XBOX_MEMORY_H */
