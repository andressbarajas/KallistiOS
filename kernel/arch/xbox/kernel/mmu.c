/* KallistiOS ##version##

   kernel/arch/xbox/kernel/mmu.c
   Copyright (C) 2026 Cypress

*/

#include <stdbool.h>
#include <stdint.h>

#include <arch/mmu.h>
#include <xbox/memory.h>

#include "x86.h"

/* Set by the linker; the end of our image and the floor for any claim. */
extern char end[];

/* Stack pointer the loader handed us, captured in startup.S. */
extern uintptr_t arch_old_stack;

/*
 * Paging is live and inherited from the Xbox kernel, so installed RAM is not
 * the limit that matters: an address is only usable if a page-table entry maps
 * it. The loader hands us a bounded window and NV_PFB_CSTATUS cheerfully
 * reports the full 64 MiB behind it, so asking the GPU how much RAM exists
 * gives an answer we cannot touch.
 *
 * Ask the page tables instead. They are self-mapped (the PTE for VA X lives at
 * 0xC0000000 + (X>>12)*4, so the directory itself is at 0xC0300000), which lets
 * us walk upward from the image and stop at the first absent page. That tracks
 * whatever window the loader actually established, with no constant shared
 * between the two projects to drift.
 */
#define XBOX_PAGE_SIZE   0x1000U
#define XBOX_LARGE_PAGE  0x400000U
#define XBOX_PTE_PRESENT 0x1U
#define XBOX_PDE_LARGE   0x80U

#define XBOX_PDE(va) \
    (*(volatile uint32_t *)(uintptr_t)(XBOX_MMU_PDE_BASE + (((va) >> 22) * 4U)))
#define XBOX_PDE_AT(idx) \
    (*(volatile uint32_t *)(uintptr_t)(XBOX_MMU_PDE_BASE + ((idx) * 4U)))
#define XBOX_PTE(va) \
    (*(volatile uint32_t *)(uintptr_t)(XBOX_MMU_PTE_BASE + (((va) >> 12) * 4U)))

/* Used only if the walk cannot get started. This runs before mm_init(), so
   refusing to boot would be worse than starting small. */
#define XBOX_MEM_TOP_FALLBACK ((uintptr_t)0x02000000)

/* The framebuffer is the hard stop: it is live RAM the loader keeps drawing
   into. Bound it by what the memory controller says is installed. */
#define XBOX_NV_PFB_CSTATUS 0x10020CU
#define XBOX_NV_PCRTC_START 0x600800U
#define XBOX_NV2A_R32(off) \
    (*(volatile uint32_t *)(uintptr_t)(XBOX_MEM_NV2A_BASE + (off)))

uintptr_t xbox_mmu_usable_ceiling(void) {
    uint32_t ram = XBOX_NV2A_R32(XBOX_NV_PFB_CSTATUS);
    uint32_t fb  = XBOX_NV2A_R32(XBOX_NV_PCRTC_START);

    if(ram < (32U << 20) || ram > (128U << 20) || (ram & 0xFFFFFU))
        return 0U;                      /* implausible - do not claim anything */

    if(fb != 0U && fb < ram)
        return (uintptr_t)fb;

    return (uintptr_t)ram;
}

uintptr_t xbox_mmu_mapped_top(uintptr_t from, uintptr_t fallback) {
    uintptr_t va = from & ~(uintptr_t)(XBOX_PAGE_SIZE - 1U);

    /* If our own image is not mapped by the tables we think we are reading,
       the self-map is not where we expect; do not trust the walk at all. */
    if(!(XBOX_PDE(va) & XBOX_PTE_PRESENT))
        return fallback;

    while(va < 0xC0000000U) {
        uint32_t pde = XBOX_PDE(va);

        if(!(pde & XBOX_PTE_PRESENT))
            break;

        /* A 4 MiB page needs no second level and is mapped in its entirety. */
        if(pde & XBOX_PDE_LARGE) {
            va = (va + XBOX_LARGE_PAGE) & ~(uintptr_t)(XBOX_LARGE_PAGE - 1U);
            continue;
        }

        if(!(XBOX_PTE(va) & XBOX_PTE_PRESENT))
            break;

        va += XBOX_PAGE_SIZE;
    }

    return (va > from) ? va : fallback;
}

/*
 * Claim the RAM the loader left unmapped.
 *
 * Paging is inherited from the Xbox kernel, so installed RAM is not usable
 * until a page-table entry covers it. Rather than live inside whatever window
 * the loader happened to map, identity-map the rest ourselves with 4 MiB pages
 * (CR4.PSE is set by the Xbox kernel, so no page tables need allocating) and
 * hand the directory back untouched on the way out.
 *
 * The directory is reachable only through the self-map: its physical home is
 * below the guest window and has no virtual address of its own.
 *
 * Off limits, per the loader's paging contract:
 *   PDE 0        the loader's identity window - the return target
 *   PDE 768      the self-map itself
 *   PDE 960..975 the write-combined alias the loader draws video through
 * Entries between the image and the framebuffer are ours.
 */
#define XBOX_PDE_IDENTITY_FLAGS 0x83U  /* present | writable | 4 MiB page */
#define XBOX_PDE_FIRST          1U     /* never touch PDE 0 */
#define XBOX_PDE_LIMIT          255U   /* stay far below the self-map */

static uint32_t xbox_saved_pde[XBOX_PDE_LIMIT + 1U];
static uint32_t xbox_saved_lo, xbox_saved_hi;   /* range actually rewritten */
static bool xbox_pdes_claimed;

static void xbox_flush_tlb(void) {
    uint32_t cr3;
    __asm__ __volatile__("mov %%cr3, %0" : "=r"(cr3));
    __asm__ __volatile__("mov %0, %%cr3" :: "r"(cr3) : "memory");
}

/* Confirm a freshly mapped 4 MiB region is RAM we can actually keep.

   Two ways this fails. The region may not be backed at all, in which case
   stores evaporate and the read-back mismatches. Or it may already be live
   under another name - the framebuffer and the page tables are both reachable
   only through apertures, so they leave no trace in the low directory - in
   which case writing a marker at the base would be corrupting someone. Guard
   against the second by checking the marker does not also appear at the start
   of the region below, which would mean we are aliasing rather than mapping
   new memory. */
static bool xbox_mmu_region_usable(uintptr_t base) {
    volatile uint32_t *probe = (volatile uint32_t *)base;
    volatile uint32_t *below = (volatile uint32_t *)(base - XBOX_LARGE_PAGE);
    uint32_t saved = *probe;
    uint32_t saved_below = *below;
    bool ok;

    *probe = 0x5A5AC0DEU;
    ok = (*probe == 0x5A5AC0DEU) && (*below != 0x5A5AC0DEU);

    *probe = saved;
    *below = saved_below;
    return ok;
}

static uintptr_t xbox_mmu_fill(uintptr_t from, uintptr_t ceiling) {
    uint32_t cr0, cr4;
    uint32_t idx, first, last;
    uintptr_t top = from;

    __asm__ __volatile__("mov %%cr4, %0" : "=r"(cr4));
    if(!(cr4 & 0x10U))            /* CR4.PSE - no 4 MiB pages, leave it alone */
        return from;

    first = (uint32_t)(from >> 22);
    if(first < XBOX_PDE_FIRST)
        first = XBOX_PDE_FIRST;

    /* Never map the directory entry the framebuffer lives in. */
    last = (uint32_t)(ceiling >> 22);
    if(last == 0U)
        return from;
    last--;
    if(last > XBOX_PDE_LIMIT)
        last = XBOX_PDE_LIMIT;
    if(first > last)
        return from;

    /* Page-table writes go through the self-map, which is writable; clearing
       WP is belt-and-braces, matching what the loader does. */
    __asm__ __volatile__("mov %%cr0, %0" : "=r"(cr0));
    if(cr0 & 0x00010000U)
        __asm__ __volatile__("mov %0, %%cr0" :: "r"(cr0 & ~0x00010000U) : "memory");

    xbox_saved_lo = first;
    xbox_saved_hi = first;

    for(idx = first; idx <= last; ++idx) {
        uint32_t pde = XBOX_PDE_AT(idx);

        xbox_saved_pde[idx] = pde;
        xbox_saved_hi = idx;

        if(!(pde & XBOX_PTE_PRESENT)) {
            XBOX_PDE_AT(idx) =
                ((uint32_t)idx << 22) | XBOX_PDE_IDENTITY_FLAGS;
            xbox_flush_tlb();

            /* An absent directory entry says nothing about the RAM behind it:
               without a firmware memory map we are inferring that it is free.
               Prove at least that it is real, writable memory that reads back
               what we store, and that we have not simply re-reached somewhere
               already in use. Give the entry back and stop if not. */
            if(!xbox_mmu_region_usable((uintptr_t)idx << 22)) {
                XBOX_PDE_AT(idx) = pde;
                xbox_flush_tlb();
                break;
            }
        }

        top = ((uintptr_t)idx + 1U) << 22;
    }

    xbox_pdes_claimed = true;

    if(cr0 & 0x00010000U)
        __asm__ __volatile__("mov %0, %%cr0" :: "r"(cr0) : "memory");

    xbox_flush_tlb();
    return top;
}

/* Put the directory back exactly as the loader left it. */
void xbox_mmu_release(void) {
    uint32_t cr0, idx;

    if(!xbox_pdes_claimed)
        return;

    __asm__ __volatile__("mov %%cr0, %0" : "=r"(cr0));
    if(cr0 & 0x00010000U)
        __asm__ __volatile__("mov %0, %%cr0" :: "r"(cr0 & ~0x00010000U) : "memory");

    for(idx = xbox_saved_lo; idx <= xbox_saved_hi; ++idx)
        XBOX_PDE_AT(idx) = xbox_saved_pde[idx];

    if(cr0 & 0x00010000U)
        __asm__ __volatile__("mov %0, %%cr0" :: "r"(cr0) : "memory");

    xbox_pdes_claimed = false;
    xbox_flush_tlb();
}


/*
 * A well-behaved loader keeps its own descriptor tables and return stack
 * outside the advertised guest arena. Preserve them defensively if a loader
 * (or a previously returned guest) leaves any of those live objects inside
 * the arena: KOS's contiguous sbrk heap must stop before the first one.
 *
 * This is especially important for repeated development uploads. The CPU
 * caches active descriptors, so overwriting the backing GDT may appear to
 * work until shutdown reloads it and faults while trying to return.
 */
static uintptr_t xbox_mmu_loader_ceiling(uintptr_t arena_top) {
    x86_descriptor_register_t gdtr;
    x86_descriptor_register_t idtr;
    uintptr_t image_end = (uintptr_t)end;
    uintptr_t ceiling = arena_top;
    uintptr_t candidates[3];
    unsigned int index;

    __asm__ volatile("sgdt %0" : "=m"(gdtr));
    __asm__ volatile("sidt %0" : "=m"(idtr));
    candidates[0] = gdtr.base;
    candidates[1] = idtr.base;
    candidates[2] = arch_old_stack;

    for(index = 0; index < 3U; ++index) {
        if(candidates[index] >= image_end && candidates[index] < ceiling)
            ceiling = candidates[index];
    }

    return ceiling;
}

/* Whatever the loader mapped, plus whatever we can claim above it. */
uintptr_t xbox_mmu_claim(uintptr_t from) {
    uintptr_t mapped = xbox_mmu_mapped_top(from, XBOX_MEM_TOP_FALLBACK);
    uintptr_t ceiling = xbox_mmu_usable_ceiling();

    if(ceiling > mapped)
        mapped = xbox_mmu_fill(mapped, ceiling);

    /* Whatever we ended up with, stop short of anything the loader left live
       inside it. */
    return xbox_mmu_loader_ceiling(mapped);
}
