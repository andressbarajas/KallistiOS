/* KallistiOS ##version##

   include/kos/fs_kosload.h
   (c)2002 Andrew Kieschnick

*/

/** \file    kos/fs_kosload.h
    \brief   Implementation of kosload "filesystem".
    \ingroup vfs_kosload

    This file contains declarations related to using kosload, both in its -ip and
    -serial forms. This is only used for dcload-ip support if the internal
    network stack is not initialized at start via KOS_INIT_FLAGS().

    \author Andrew Kieschnick
*/

#ifndef __KOS_FS_KOSLOAD_H
#define __KOS_FS_KOSLOAD_H

/* Definitions for the "kosload" file system */

#include <kos/cdefs.h>
__BEGIN_DECLS

#include <stdbool.h>
#include <kos/fs.h>
#include <kos/dbgio.h>

#include <arch/kosload.h>

/** \defgroup vfs_kosload    PC
    \brief                  VFS driver for accessing a remote PC via
                            DC-Load/Tool
    \ingroup                vfs

    @{
*/

/* \cond */
extern dbgio_handler_t dbgio_kosload;
/* \endcond */

/* kosload magic value */
/** \brief  The kosload magic value! */
#define KOSLOADMAGICVALUE 0xdeadbeef

/** \brief  The address of the kosload magic value */
#define KOSLOADMAGICADDR (unsigned int *)KOSLOAD_MAGIC_ADDR

/* Are we using dc-load-serial or dc-load-ip? */
#define KOSLOAD_TYPE_NONE    -1      /**< \brief No kosload connection */
#define KOSLOAD_TYPE_SER     0       /**< \brief dcload-serial connection */
#define KOSLOAD_TYPE_IP      1       /**< \brief dcload-ip connection */

/** \brief  What type of kosload connection do we have? */
extern int kosload_type;

/* \cond */

/* Tests for the kosload syscall being present. */
int syscall_kosload_detected(void);

/* Init func */
void fs_kosload_init_console(void);
void fs_kosload_init(void);
void fs_kosload_shutdown(void);

/* \endcond */

/** @} */

__END_DECLS

#endif  /* __KOS_FS_KOSLOAD_H */
