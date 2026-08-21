/* KallistiOS ##version##

   arch/xbox/include/arch/init_flags.h
   Copyright (C) 2026 Andress Barajas
   Copyright (C) 2026 Cypress

*/

/** \file    arch/init_flags.h
    \brief   Xbox-specific initialization-related flags and macros.
    \ingroup init_flags

    This file provides initialization-related flags that are specific to the
    Xbox architecture.

    \sa    kos/init.h
    \sa    kos/init_base.h

    \author Andress Barajas
*/

#ifndef __ARCH_INIT_FLAGS_H
#define __ARCH_INIT_FLAGS_H

#include <kos/cdefs.h>
#include <kos/init_base.h>
__BEGIN_DECLS

/** \brief   Xbox-specific KOS_INIT Exports
    \ingroup init_flags

    This macro contains a list of all of the possible Xbox-specific
    exported functions based on their associated initialization flags.

    \note
    This is not typically used directly and is instead included within
    the top-level architecture-independent KOS_INIT_FLAGS() macro.

    \param flags    Parts of KOS to initialize.

    \sa KOS_INIT_FLAGS()
*/
#define KOS_INIT_FLAGS_ARCH(flags) \
    KOS_INIT_FLAG_NONE(flags, INIT_NO_KOSLOAD, kosload_init); \
    KOS_INIT_FLAG_NONE(flags, INIT_NO_KOSLOAD, fs_kosload_init_console); \
    KOS_INIT_FLAG_NONE(flags, INIT_NO_KOSLOAD, fs_kosload_shutdown); \
    extern int usb_init(void); \
    int (*usb_init_weak)(void) = \
        ((flags) & INIT_NO_USB) ? (int (*)(void))0 : usb_init; \
    KOS_INIT_FLAG_NONE(flags, INIT_NO_USB, usb_shutdown)


/** \defgroup kos_init_flags_xbox Xbox-Specific Flags
    \brief    Xbox-specific initialization flags.
    \ingroup  init_flags

    These are the Xbox-specific flags that can be specified with
    KOS_INIT_FLAGS.

    \see    kos_initflags
    @{
*/

/** \brief Default init flags for the Xbox. */
#define INIT_DEFAULT_ARCH   0

#define INIT_NO_KOSLOAD     0x20000000  /**< \brief Disable kos-load */
#define INIT_NO_USB         0x10000000  /**< \brief Disable USB host support */

/** @} */

__END_DECLS

#endif /* !__ARCH_INIT_FLAGS_H */
