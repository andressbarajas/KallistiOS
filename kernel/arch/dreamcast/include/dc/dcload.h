/* KallistiOS ##version##

   kernel/arch/dreamcast/include/dc/dcload.h
   Copyright (C) 2025 Donald Haase
   Copyright (C) 2026 Andy Barajas

*/

/** \file    dc/dcload.h
    \brief   Deprecated compatibility shim for the old dcload driver API.
    \ingroup kosload_syscalls

    \deprecated
    The dc-load syscall driver has been made platform-agnostic and renamed to
    kos-load.  This header only provides backwards-compatible aliases for the
    old dc-load names.

    \author Donald Haase
    \see    kos/kosload.h
*/

#ifndef __DC_DCLOAD_H
#define __DC_DCLOAD_H

#include <kos/cdefs.h>
__BEGIN_DECLS

#include <kos/kosload.h>

/** \addtogroup kosload_syscalls
    @{
*/

/* \cond */

/** \brief  \deprecated Use \ref kosload_cmd_t. */
#define dcload_cmd_t        kosload_cmd_t
/** \brief  \deprecated Use \ref kosload_syscall_t. */
#define dcload_syscall_t    kosload_syscall_t
/** \brief  \deprecated Use \ref kosload_stat_t. */
#define dcload_stat_t       kosload_stat_t
/** \brief  \deprecated Use struct \ref kosload_stat. */
#define dcload_stat         kosload_stat

#define DCLOAD_READ         KOSLOAD_READ
#define DCLOAD_WRITE        KOSLOAD_WRITE
#define DCLOAD_OPEN         KOSLOAD_OPEN
#define DCLOAD_CLOSE        KOSLOAD_CLOSE
#define DCLOAD_CREAT        KOSLOAD_CREAT
#define DCLOAD_LINK         KOSLOAD_LINK
#define DCLOAD_UNLINK       KOSLOAD_UNLINK
#define DCLOAD_CHDIR        KOSLOAD_CHDIR
#define DCLOAD_CHMOD        KOSLOAD_CHMOD
#define DCLOAD_LSEEK        KOSLOAD_LSEEK
#define DCLOAD_FSTAT        KOSLOAD_FSTAT
#define DCLOAD_TIME         KOSLOAD_TIME
#define DCLOAD_STAT         KOSLOAD_STAT
#define DCLOAD_UTIME        KOSLOAD_UTIME
#define DCLOAD_ASSIGNWRKMEM KOSLOAD_ASSIGNWRKMEM
#define DCLOAD_EXIT         KOSLOAD_EXIT
#define DCLOAD_OPENDIR      KOSLOAD_OPENDIR
#define DCLOAD_CLOSEDIR     KOSLOAD_CLOSEDIR
#define DCLOAD_READDIR      KOSLOAD_READDIR
#define DCLOAD_GETHOSTINFO  KOSLOAD_GETHOSTINFO
#define DCLOAD_GDBPACKET    KOSLOAD_GDBPACKET
#define DCLOAD_REWINDDIR    KOSLOAD_REWINDDIR

#define dcload_syscall_set          kosload_syscall_set
#define dcload_syscall_net_init     kosload_syscall_net_init
#define dcload_syscall_net_shutdown kosload_syscall_net_shutdown

#define dcload_read         kosload_read
#define dcload_write        kosload_write
#define dcload_open         kosload_open
#define dcload_close        kosload_close
#define dcload_creat        kosload_creat
#define dcload_link         kosload_link
#define dcload_unlink       kosload_unlink
#define dcload_chdir        kosload_chdir
#define dcload_chmod        kosload_chmod
#define dcload_lseek        kosload_lseek
#define dcload_fstat        kosload_fstat
#define dcload_time         kosload_time
#define dcload_assignwrkmem kosload_assignwrkmem
#define dcload_exit         kosload_exit
#define dcload_opendir      kosload_opendir
#define dcload_closedir     kosload_closedir
#define dcload_readdir      kosload_readdir
#define dcload_gethostinfo  kosload_gethostinfo
#define dcload_gdbpacket    kosload_gdbpacket
#define dcload_rewinddir    kosload_rewinddir

/* \endcond */

/** @} */

__END_DECLS

#endif  /* __DC_DCLOAD_H */
