/* KallistiOS ##version##

   arch/dreamcast/kernel/gdb/gdb_remote.c

   Copyright (C) 2026 Andy Barajas

*/

/*
   Implements remote stop and control replies for the GDB stub.

   This file is responsible for:
     - translating SH4 exceptions into GDB-visible stop signals
     - building T stop replies with signal, register, thread, and reason fields
     - handling D detach and k kill control packets
*/

#include <arch/arch.h>

#include "gdb_internal.h"

#define SIGILL  4   /* Illegal Instruction */
#define SIGTRAP 5   /* Breakpoint Trap */
#define SIGBUS  7   /* Bus Error */
#define SIGSEGV 11  /* Segmentation Fault */

/*
   Maps SH-4 exception vectors to the signal numbers reported in stop replies.
*/
static int compute_signal(int exception_vector) {
    switch(exception_vector) {
        case EXC_ILLEGAL_INSTR:
        case EXC_SLOT_ILLEGAL_INSTR:
            return SIGILL;
        case EXC_USER_BREAK_PRE:
        case EXC_TRAPA:
            return SIGTRAP;
        case EXC_DATA_ADDRESS_READ:
        case EXC_DATA_ADDRESS_WRITE:
            return SIGSEGV;
        default:
            return SIGBUS;
    }
}

/*
   Returns the reason name used in the T stop reply's reason: field.
*/
static const char *stop_reason_name(int exception_vector) {
    switch(exception_vector) {
        case EXC_TRAPA:
            return "swbreak";
        case EXC_USER_BREAK_PRE:
            return "hwbreak";
        case EXC_DATA_ADDRESS_WRITE:
            return "watch";
        case EXC_DATA_ADDRESS_READ:
            return "rwatch";
        default:
            return "signal";
    }
}

/*
   Handle the 'D' (detach) command.
   Instructs the stub to detach from the target.
   Format: D

   The stub replies with "OK", marks the debugger as disconnected, and returns
   to the interrupted program without staying in the debug loop.
*/
void handle_detach(void) {
    put_packet(GDB_OK);
    gdb_set_connected(false);
}

/*
   Handle the 'k' (kill) command.
   Instructs the stub to terminate the program being debugged.
   Format: k

   The stub replies with "OK" and then aborts execution via arch_abort().
*/
void handle_kill(void) {
    put_packet(GDB_OK);
    arch_abort();
}

/*
   Constructs a `T` stop reply packet to notify GDB that the target has halted.
   Includes the signal, the mapped base register fields, the current thread ID,
   and a stop reason string.

   Format:
     Tssnn:vvvvvvvv;...thread:tid;reason:name;

   The register fields appended here are the stop-reply register subset, not
   the full raw g/G register block.
*/
void handle_t_stop_reply(int exception_vector) {
    const irq_context_t *context = gdb_get_irq_context();
    kthread_t *thd = thd_get_current();
    char *out = remcom_out_buffer;
    int sigval = compute_signal(exception_vector);

    *out++ = 'T';
    *out++ = highhex(sigval);
    *out++ = lowhex(sigval);
    out = append_regs(out, context);

    memcpy(out, "thread:", 7);
    out += 7;
    out += format_thread_id_hex(out, (uint32_t)thd->tid);
    *out++ = ';';

    memcpy(out, "reason:", 7);
    out += 7;
    out += strlen(strcpy(out, stop_reason_name(exception_vector)));
    *out++ = ';';
    *out = '\0';
}
