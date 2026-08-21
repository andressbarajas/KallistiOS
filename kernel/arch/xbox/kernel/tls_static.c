/* KallistiOS ##version##

   arch/xbox/kernel/tls_static.c
   Copyright (C) 2026 Cypress
*/

/* Functions to initialize and manage static TLS data. */

#include <assert.h>
#include <stdlib.h>
#include <string.h>

#include <arch/tls_static.h>
#include <kos/thread.h>

#include "gdt.h"

/* TLS section geometry - exported from utils/ldscripts/xbox.ld. Unlike PE,
   ELF adds no underscore to C identifiers, so these are spelled exactly as
   the linker script defines them. */
extern int _tdata_start, _tdata_size;
extern int _tbss_size;
extern long _tdata_align, _tbss_align;

/* Utility function for aligning an address or offset. */
static inline size_t align_to(size_t address, size_t alignment) {
    return (address + (alignment - 1)) & ~(alignment - 1);
}

/*  Thread Control Block Header

    i386 uses TLS variant II: the thread pointer points *at* the TCB, and the
    static block sits *below* it, so the linker's @ntpoff values are negative.
    This is inverted relative to the Dreamcast's variant I layout.

        low addr                                          high addr
        +----------------+--------------+--------+
        |     .tdata     |    .tbss     |  TCB   |
        +----------------+--------------+--------+
        ^                                ^
        allocation base                  TP == %gs base

    Only the self-pointer is meaningful here: the psABI requires that a
    thread be able to recover its own thread pointer with "movl %gs:0, %eax",
    which only works if the first word at TP holds TP itself.
*/
typedef struct tcbhead {
    void *tcb;   /* Self-pointer; MUST equal the thread pointer. */
    void *dtv;   /* Dynamic TLS vector (unused) */
    void *self;  /* Redundant self-pointer (unused) */
} tcbhead_t;

void arch_tls_init(void) {
    /* Install the already-allocated thread pointer of the running kernel
       thread. Everything before this point must avoid thread-locals; the
       call site in thd_init() runs before any other thread exists. */
    xbox_gdt_set_tls_base((uint32_t)(uintptr_t)thd_get_current()->context.tls_tp);
}

/*  Creates and initializes the static TLS block for a thread: .tdata
    followed by .tbss, followed by the TCB that the thread pointer names.

    The layout below must reproduce the PT_TLS image ld built, because the
    linker's @ntpoff values are authoritative. In particular .tbss begins at
    align_to(tdata_size, tbss_align) within the block, which is where ld
    placed it -- not necessarily immediately after .tdata.
*/
bool arch_tls_setup_data(kthread_t *thd) {
    size_t align, tdata_offset, tbss_offset, tbss_end, tlsoffset, block_size;

    uint8_t *block;
    tcbhead_t *tcbhead;

    /* Cached and typed local copies of the linker script's TLS geometry.

       SIZES MUST BE VOLATILE or the optimizer on non-debug builds will
       optimize the zero-check conditionals away, since why would the address
       of a variable be NULL? (Linker script magic, it can be.)
    */
    const volatile size_t   tdata_size  = (size_t)(&_tdata_size);
    const volatile size_t   tbss_size   = (size_t)(&_tbss_size);
    const          size_t   tdata_align = tdata_size ? (size_t)_tdata_align : 1;
    const          size_t   tbss_align  = tbss_size ? (size_t)_tbss_align : 1;
    const          uint8_t *tdata_start = (const uint8_t *)(&_tdata_start);

    /* The whole block is aligned by the largest requirement among the TCB
       and the two subsegments. Because the data sits *below* the thread
       pointer, over-alignment padding lands at the low end of the
       allocation, not the high end. */
    align = 8;               /* tcbhead_t has to be aligned by 8. */
    if(tdata_align > align)
        align = tdata_align; /* .TDATA segment's alignment */
    if(tbss_align > align)
        align = tbss_align;  /* .TBSS segment's alignment */

    /* Offsets of each subsegment from the allocation base, mirroring how ld
       laid out PT_TLS. */
    tdata_offset = 0;
    tbss_offset  = align_to(tdata_offset + tdata_size, tbss_align);
    tbss_end     = tbss_offset + tbss_size;

    /* Distance from the allocation base up to the thread pointer. Aligning
       it keeps the TCB -- and therefore TP itself -- correctly aligned. */
    tlsoffset  = align_to(tbss_end, align);
    block_size = tlsoffset + sizeof(tcbhead_t);

    block = aligned_alloc(align, align_to(block_size, align));

    if(!block)
        return false;

    assert(!((uintptr_t)block % align));

    /* Initialize .TDATA from the ELF image. */
    if(tdata_size) {
        assert(!((uintptr_t)(block + tdata_offset) % tdata_align));
        memcpy(block + tdata_offset, tdata_start, tdata_size);
    }

    /* Zero-initialize .TBSS. */
    if(tbss_size) {
        assert(!((uintptr_t)(block + tbss_offset) % tbss_align));
        memset(block + tbss_offset, 0, tbss_size);
    }

    tcbhead = (tcbhead_t *)(block + tlsoffset);
    memset(tcbhead, 0, sizeof(tcbhead_t));
    tcbhead->tcb = tcbhead;   /* "movl %gs:0, %eax" must yield TP. */

    /* The context switch reloads %gs from this; arch_tls_destroy_data()
       frees the allocation base, which is what tls_hnd keeps. */
    thd->context.tls_tp = (uint32_t)(uintptr_t)tcbhead;
    thd->tls_hnd = block;

    return true;
}

void arch_tls_destroy_data(kthread_t *thd) {
    free(thd->tls_hnd);
    thd->tls_hnd = NULL;
    thd->context.tls_tp = 0;
}

size_t arch_tls_data_offset(void) {
    /* tls_hnd is the allocation base, which is also where .tdata starts, so
       the offset is zero. Note this accessor is not meaningful on i386 the
       way it is on SH: variant II puts thread-locals at *negative* offsets
       from the thread pointer. There is no Xbox GDB stub, so nothing
       currently consumes it. */
    return 0;
}
