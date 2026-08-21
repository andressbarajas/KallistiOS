/* Internal Xbox GDT ownership interface. */

#ifndef __KOS_XBOX_KERNEL_GDT_H
#define __KOS_XBOX_KERNEL_GDT_H

#include <stdint.h>

/* Selector of the per-thread static TLS data descriptor. Loading it into %gs
   is what makes "movl %gs:x@ntpoff, %eax" reach the running thread's block. */
#define X86_SELECTOR_TLS    0x20U

/* GDT entry whose base holds the running thread's TLS thread pointer. The
   CPU caches a descriptor in the hidden part of a segment register, so %gs
   must be reloaded *after* this base is rewritten. */
extern uint64_t *const xbox_gdt_tls_descriptor;

/* Point the TLS descriptor at a thread pointer and reload %gs from it. */
void xbox_gdt_set_tls_base(uint32_t base);

int xbox_gdt_init(void);
void xbox_gdt_shutdown(void);

#endif
