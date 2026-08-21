/* Xbox memory probe.
 *
 * The NV2A is an NV20-family part with no dedicated video memory: it scans out
 * of main RAM. So "how much RAM is installed" and "how much may a guest touch"
 * are separate questions, and both are answerable from its registers.
 */

#include <stdio.h>
#include <stdint.h>
#include <arch/arch.h>

#define NV2A_BASE        0xFD000000u
#define NV_PFB_CSTATUS   0x10020Cu   /* installed RAM, in bytes   */
#define NV_PCRTC_START   0x600800u   /* live scanout base, phys   */
#define NV_CRTC_IDX      0x6013D4u   /* VGA CRTC index (PRMCIO)   */
#define NV_CRTC_DAT      0x6013D5u   /* VGA CRTC data             */

#define NV2A_R32(off)    (*(volatile uint32_t *)(uintptr_t)(NV2A_BASE + (off)))
#define NV2A_R8P(off)    ((volatile uint8_t  *)(uintptr_t)(NV2A_BASE + (off)))

int main(int argc, char *argv[]) {
    uint32_t ram, fb, c13, c19, c25, pitch;
    (void)argc; (void)argv;

    printf("Xbox memory probe\n\n");

    ram = NV2A_R32(NV_PFB_CSTATUS);
    printf("NV_PFB_CSTATUS  = 0x%08lx  -> %lu MB installed\n",
           (unsigned long)ram, (unsigned long)(ram / (1024U * 1024U)));

    fb = NV2A_R32(NV_PCRTC_START);
    *NV2A_R8P(NV_CRTC_IDX) = 0x13u; c13 = *NV2A_R8P(NV_CRTC_DAT);
    *NV2A_R8P(NV_CRTC_IDX) = 0x19u; c19 = *NV2A_R8P(NV_CRTC_DAT);
    *NV2A_R8P(NV_CRTC_IDX) = 0x25u; c25 = *NV2A_R8P(NV_CRTC_DAT);
    pitch = ((c13 & 0xFFu) | ((c19 & 0xE0u) << 3) | ((c25 & 0x20u) << 6)) << 3;

    printf("NV_PCRTC_START  = 0x%08lx  -> framebuffer at %lu.%02lu MB\n",
           (unsigned long)fb,
           (unsigned long)(fb / (1024U * 1024U)),
           (unsigned long)(((fb % (1024U * 1024U)) * 100U) / (1024U * 1024U)));
    printf("CRTC pitch      = %lu bytes/line (%lu px wide)\n",
           (unsigned long)pitch, (unsigned long)(pitch / 4U));

    printf("\n_arch_mem_top   = 0x%08lx  -> %lu.%02lu MB usable\n",
           (unsigned long)_arch_mem_top,
           (unsigned long)(_arch_mem_top / (1024U * 1024U)),
           (unsigned long)(((_arch_mem_top % (1024U * 1024U)) * 100U)
                           / (1024U * 1024U)));

    if(fb != 0U && _arch_mem_top < fb)
        printf("  (stops %lu KB below the framebuffer, as intended)\n",
               (unsigned long)((fb - _arch_mem_top) / 1024U));

    /* Reconnaissance for letting KOS map its own memory: can we use 4 MiB
       pages (no page-table allocation needed), and where does the mapping
       actually stop? */
    {
        uint32_t cr0, cr3, cr4;
        uint32_t va;
        volatile uint32_t *pd = (volatile uint32_t *)(uintptr_t)0xC0300000u;
        unsigned shown = 0;

        __asm__ volatile("mov %%cr0, %0" : "=r"(cr0));
        __asm__ volatile("mov %%cr3, %0" : "=r"(cr3));
        __asm__ volatile("mov %%cr4, %0" : "=r"(cr4));

        printf("\nPaging state:\n");
        printf("  CR0 = 0x%08lx  PG=%lu\n",
               (unsigned long)cr0, (unsigned long)((cr0 >> 31) & 1U));
        printf("  CR3 = 0x%08lx  (page directory physical base)\n",
               (unsigned long)cr3);
        printf("  CR4 = 0x%08lx  PSE=%lu (4 MiB pages %s)\n",
               (unsigned long)cr4, (unsigned long)((cr4 >> 4) & 1U),
               ((cr4 >> 4) & 1U) ? "AVAILABLE" : "NOT available");

        printf("\nPage directory, first 4 MiB entries covering low memory:\n");
        for(va = 0; va < 0x04000000u && shown < 18u; va += 0x400000u) {
            uint32_t pde = pd[va >> 22];
            printf("  VA 0x%08lx  PDE=0x%08lx  %s%s\n",
                   (unsigned long)va, (unsigned long)pde,
                   (pde & 1U) ? "present" : "ABSENT ",
                   (pde & 1U) ? ((pde & 0x80U) ? " 4MiB-page" : " 4KiB-table") : "");
            shown++;
        }
    }

    printf("\nDone.\n");
    return 0;
}
