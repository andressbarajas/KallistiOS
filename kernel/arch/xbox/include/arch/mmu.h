/* KallistiOS ##version##

   kernel/arch/xbox/include/arch/mmu.h
   Copyright (C) 2026 Cypress

*/

/** \file    arch/mmu.h
    \brief   Paging support for the original Xbox.

    Paging is already enabled when KOS gains control: the page tables belong to
    the Xbox kernel and are inherited through whichever loader launched us. RAM
    is therefore only usable where a page-table entry maps it, which is why the
    amount of installed memory is not by itself the limit that matters.

    The directory is reachable only through its self-map. Its physical home
    lies below the guest window and has no virtual address of its own.

    \author Cypress
*/

#ifndef __ARCH_MMU_H
#define __ARCH_MMU_H

#include <sys/cdefs.h>
__BEGIN_DECLS

#include <stdint.h>

/** \defgroup xbox_mmu  MMU
    \brief    Page directory inspection and identity mapping
    @{
*/

/** \brief Virtual base of the page-table self-map.

    The page-table entry describing virtual address X lives at
    XBOX_MMU_PTE_BASE + ((X >> 12) * 4).
*/
#define XBOX_MMU_PTE_BASE   0xC0000000U

/** \brief Virtual address of the page directory itself.

    Falls out of the self-map: XBOX_MMU_PTE_BASE + ((XBOX_MMU_PTE_BASE >> 12) * 4).
*/
#define XBOX_MMU_PDE_BASE   0xC0300000U

/** \brief Highest address KOS will find or claim usable memory below. */
#define XBOX_MMU_HEAP_TOP   0x03FFFFFFU

/** \brief  Find the end of the contiguous mapping above an address.

    Walks the page directory upward from \p from, stopping at the first page
    that is not present. This measures the window the loader actually
    established rather than assuming an agreed size.

    \param  from      Address to begin walking from, typically the image end.
    \param  fallback  Returned if the self-map is not where it is expected.
    \return           First unmapped address at or above \p from.
*/
uintptr_t xbox_mmu_mapped_top(uintptr_t from, uintptr_t fallback);

/** \brief  Identity-map usable RAM the loader left unmapped.

    Fills absent page-directory entries between \p from and the framebuffer
    with 4 MiB identity pages, leaving present entries untouched. Entries that
    are overwritten are recorded so xbox_mmu_release() can restore them.

    Requires CR4.PSE, which the Xbox kernel sets; without it nothing is claimed.

    \param  from      Address to begin claiming from.
    \return           New top of usable memory.
*/
uintptr_t xbox_mmu_claim(uintptr_t from);

/** \brief  Restore every page-directory entry xbox_mmu_claim() overwrote.

    Must run before handing control back to a loader, which expects the
    directory as it left it.
*/
void xbox_mmu_release(void);

/** \brief  Highest usable address, bounded by the framebuffer.

    The live scanout base is the hard ceiling: it is RAM the loader keeps
    drawing into. Bounded by the memory controller's report of installed RAM.

    \return  Ceiling address, or 0 if the hardware reports implausibly.
*/
uintptr_t xbox_mmu_usable_ceiling(void);

/** @} */

__END_DECLS
#endif  /* __ARCH_MMU_H */
