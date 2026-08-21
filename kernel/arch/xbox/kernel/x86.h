/* KallistiOS ##version##

   kernel/arch/xbox/kernel/x86.h
   Copyright (C) 2026 Cypress

*/

/* Shared x86 CPU structures, private to the Xbox arch implementation. */

#ifndef __XBOX_KERNEL_X86_H
#define __XBOX_KERNEL_X86_H

#include <stdint.h>

/* Operand of LGDT/SGDT and LIDT/SIDT. */
typedef struct __attribute__((packed)) x86_descriptor_register {
    uint16_t limit;
    uint32_t base;
} x86_descriptor_register_t;

#endif  /* __XBOX_KERNEL_X86_H */
