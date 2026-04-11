/* KallistiOS ##version##

   kernel/gdb/gdb_break.c

   Copyright (C) Megan Potter
   Copyright (C) Richard Moats
   Copyright (C) 2026 Andy Barajas

*/

/*
   Implements GDB breakpoint and watchpoint packets for the Dreamcast target.

   Supported packet families:
     - Z0/z0: software breakpoints via TRAPA instruction patching
     - Z1-Z4/z1-z4: hardware breakpoints and watchpoints via the SH4 UBC

   Notes:
     - software breakpoints require 2-byte alignment
     - hardware breakpoints are limited to SH4 UBC-supported access sizes
     - the hardware breakpoint path uses the existing direct UBC register backend
*/

#include <arch/cache.h>

#include "gdb_internal.h"

/* Handle inserting/removing a hardware breakpoint.
   Using the SH4 User Break Controller (UBC) we can have
   two breakpoints, each set for either instruction and/or operand access.
   Break channel B can match a specific data being moved, but there is
   no GDB remote protocol spec for utilizing this functionality. */

#define LREG(r, o) (*((uint32_t*)((r)+(o))))
#define WREG(r, o) (*((uint16_t*)((r)+(o))))
#define BREG(r, o) (*((uint8_t*)((r)+(o))))

#define MAX_SW_BREAKPOINTS 32
#define GDB_SW_BREAK_OPCODE 0xc33f
#define GDB_BRK_SW 0
#define GDB_BRK_HW 1
#define GDB_WATCH_W 2
#define GDB_WATCH_R 3
#define GDB_WATCH_RW 4

typedef struct {
    uint32_t addr;
    uint16_t original;
    bool active;
} sw_breakpoint_t;

static sw_breakpoint_t sw_breakpoints[MAX_SW_BREAKPOINTS];

static bool encode_hw_break_length(size_t length, uint8_t *size_code) {
    switch(length) {
        case 1u:
            *size_code = 1u;
            return true;
        case 2u:
            *size_code = 2u;
            return true;
        case 4u:
            *size_code = 3u;
            return true;
        case 8u:
            *size_code = 4u;
            return true;
        default:
            return false;
    }
}

static void soft_breakpoint(bool set, uintptr_t addr, size_t length,
                            char *res_buffer) {
    if((addr & 1u) || length != 2u) {
        gdb_error_with_code_str(GDB_EINVAL,
                                "Z0: address must be 2-byte aligned");
        return;
    }

    for(int i = 0; i < MAX_SW_BREAKPOINTS; ++i) {
        if(sw_breakpoints[i].active && sw_breakpoints[i].addr == addr) {
            if(set) {
                strcpy(res_buffer, GDB_OK);
            }
            else {
                *((uint16_t *)addr) = sw_breakpoints[i].original;
                icache_flush_range(addr, 2);
                sw_breakpoints[i].active = false;
                strcpy(res_buffer, GDB_OK);
            }

            return;
        }
    }

    if(!set) {
        gdb_error_with_code_str(GDB_EBKPT_CLEAR_ADDR,
                                "z0: no breakpoint at requested address");
        return;
    }

    for(int i = 0; i < MAX_SW_BREAKPOINTS; ++i) {
        if(!sw_breakpoints[i].active) {
            sw_breakpoints[i].addr = addr;
            sw_breakpoints[i].original = *((uint16_t *)addr);
            sw_breakpoints[i].active = true;
            *((uint16_t *)addr) = GDB_SW_BREAK_OPCODE;
            icache_flush_range(addr, 2);
            strcpy(res_buffer, GDB_OK);
            return;
        }
    }

    gdb_error_with_code_str(GDB_EBKPT_SW_NORES, "Z0: no free breakpoint slots");
}

static void hard_breakpoint(bool set, int brk_type, uintptr_t addr, size_t length, char* res_buffer) {
    char* const ucb_base = (char*)0xff200000;
    static const int ucb_step = 0xc;
    static const char BAR = 0x0, BAMR = 0x4, BBR = 0x8, /*BASR = 0x14,*/ BRCR = 0x20;

    static const uint8_t bbrBrk[] = {
        0x0,  /* type 0, memory breakpoint -- unsupported */
        0x14, /* type 1, hardware breakpoint */
        0x28, /* type 2, write watchpoint */
        0x24, /* type 3, read watchpoint */
        0x2c  /* type 4, access watchpoint */
    };

    uint8_t bbr;
    char* ucb;
    int i;

    if(brk_type < GDB_BRK_HW || brk_type > GDB_WATCH_RW) {
        gdb_error_with_code_str(GDB_EINVAL, "Z/z: invalid hardware breakpoint type");
        return;
    }

    if(addr == 0) {  /* GDB tries to watch 0, wasting a UCB channel */
        strcpy(res_buffer, GDB_OK);
    }
    else if(!encode_hw_break_length(length, &bbr)) {
        gdb_error_with_code_str(GDB_EMEM_SIZE,
                                "Z/z: unsupported hardware breakpoint length");
    }
    else if(set) {
        bbr |= bbrBrk[brk_type];
        WREG(ucb_base, BRCR) = 0;

        /* find a free UCB channel */
        for(ucb = ucb_base, i = 2; i > 0; ucb += ucb_step, i--)
            if(WREG(ucb, BBR) == 0)
                break;

        if(i) {
            LREG(ucb, BAR) = addr;
            BREG(ucb, BAMR) = 0x4; /* no BASR bits used, all BAR bits used */
            WREG(ucb, BBR) = bbr;
            strcpy(res_buffer, GDB_OK);
        }
        else
            strcpy(res_buffer, "E12");
    }
    else {
        bbr |= bbrBrk[brk_type];
        /* find matching UCB channel */
        for(ucb = ucb_base, i = 2; i > 0; ucb += ucb_step, i--)
            if(LREG(ucb, BAR) == addr && WREG(ucb, BBR) == bbr)
                break;

        if(i) {
            WREG(ucb, BBR) = 0;
            strcpy(res_buffer, GDB_OK);
        }
        else
            strcpy(res_buffer, "E06");
    }
}

#undef LREG
#undef WREG

/*
 * Handle the 'Z' and 'z' commands.
 * Inserts or removes a breakpoint or watchpoint.
 * Format: Ztype,addr,kind or ztype,addr,kind
 */
void handle_breakpoint(char *ptr) {
    bool set = (ptr[-1] == 'Z');
    int brk_type = *ptr++ - '0';
    uint32_t addr;
    uint32_t length;

    if(*ptr++ == ',' && hex_to_int(&ptr, &addr) &&
       *ptr++ == ',' && hex_to_int(&ptr, &length)) {
        if(brk_type < GDB_BRK_SW || brk_type > GDB_WATCH_RW) {
            gdb_error_with_code_str(GDB_EINVAL, "Z/z: invalid breakpoint type");
        }
        else if(brk_type == GDB_BRK_SW)
            soft_breakpoint(set, addr, length, remcom_out_buffer);
        else
            hard_breakpoint(set, brk_type, addr, length, remcom_out_buffer);
    }
    else {
        gdb_error_with_code_str(GDB_EINVAL, "Z/z: invalid packet");
    }
}
