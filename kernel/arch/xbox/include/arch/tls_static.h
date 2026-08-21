/* KallistiOS ##version##

   arch/xbox/include/arch/tls_static.h
   Copyright (C) 2026 Cypress
*/

/** \file    arch/tls_static.h
    \brief   Xbox compiler thread-local storage integration.
    \ingroup kthreads

    The i686-pc-xbox GCC target lowers C/C++ thread-local variables to a
    single %gs-relative access, so each thread owns a real ELF static TLS
    block. i386 uses TLS variant II: the thread pointer names the Thread
    Control Block and the .tdata/.tbss image sits *below* it, which is why
    the compiler's @ntpoff displacements are negative.

    %gs is backed by a dedicated GDT descriptor whose base is rewritten to
    the incoming thread's thread pointer on every context switch. That
    descriptor must span a full flat 4 GB: a negative @ntpoff is a very large
    unsigned segment offset that only resolves because the effective address
    wraps modulo 2^32.
*/

#ifndef __ARCH_TLS_STATIC_H
#define __ARCH_TLS_STATIC_H

#include <kos/cdefs.h>
__BEGIN_DECLS

#include <stdbool.h>
#include <stddef.h>

#include <kos/thread.h>

void arch_tls_init(void);
bool arch_tls_setup_data(kthread_t *thread);
void arch_tls_destroy_data(kthread_t *thread);

/** Return the offset from a thread's tls_hnd to its .tdata image, which is
    zero because tls_hnd is the allocation base.

    \note
    Unlike SH, this accessor is not meaningful as a way to reach a thread's
    variables: variant II places them at *negative* offsets from the thread
    pointer, not positive offsets from the allocation. There is no Xbox GDB
    stub, so nothing currently consumes it. */
size_t arch_tls_data_offset(void);

__END_DECLS

#endif /* __ARCH_TLS_STATIC_H */
