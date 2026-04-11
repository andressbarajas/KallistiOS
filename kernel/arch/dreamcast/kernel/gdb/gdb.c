/* KallistiOS ##version##

   arch/dreamcast/kernel/gdb/gdb.c

   Copyright (C) Megan Potter
   Copyright (C) Richard Moats
   Copyright (C) 2026 Andy Barajas

*/

/*
   Core Dreamcast GDB stub entry points and exception dispatch.

   This file owns:
     - stub initialization and shutdown
     - trap and exception registration
     - the main remote-protocol dispatch loop
     - connection lifecycle state for a live GDB session
*/

#include <arch/arch.h>
#include <arch/gdb.h>

#include <dc/dcload.h>
#include <dc/scif.h>

#include <stdio.h>

#include "gdb_internal.h"

/* TRAPA #0x20: internal single-step or resume */
#define TRAPA_GDB_SINGLESTEP 32

/* TRAPA #0x3F: GDB-inserted software breakpoints (Z0) */
#define TRAPA_GDB_BREAKPOINT 63

/* TRAPA #0xFF: manually inserted via gdb_breakpoint() */
#define TRAPA_USER_BREAKPOINT 255

irq_context_t *irq_ctx;
static bool initialized;
static bool connected;

/*
   Main GDB exception handler.

   Builds the initial stop reply for the current exception, then services
   remote packets until execution is resumed or the debugger detaches/kills
   the target.
*/
static void gdb_handle_exception(int exception_vector) {
    char *ptr;

    handle_t_stop_reply(exception_vector);
    put_packet(remcom_out_buffer);

    undo_single_step();

    while(true) {
        remcom_out_buffer[0] = 0;
        ptr = (char *)get_packet();
        connected = true;

        switch(*ptr++) {
            case '?': handle_t_stop_reply(exception_vector); break;
            case 'p': handle_read_reg(ptr); break;
            case 'P': handle_write_reg(ptr); break;
            case 'q': handle_query(ptr); break;
            case 'Q': handle_set_query(ptr); break;
            case 'T': handle_thread_alive(ptr); break;
            case 'H': handle_thread_select(ptr); break;
            case 'g': handle_read_regs(ptr); break;
            case 'G': handle_write_regs(ptr); break;
            case 'm': handle_read_mem(ptr); break;
            case 'M': handle_write_mem(ptr); break;
            case 'x': handle_read_mem_binary(ptr); break;
            case 'X': handle_write_mem_binary(ptr); break;
            case 'c':
            case 's':
                handle_continue_step(ptr); return;
            case 'C':
            case 'S':
                if(handle_continue_step_signal(ptr))
                    return;
                break;
            case 'Z':
            case 'z':
                handle_breakpoint(ptr); break;
            case 'v':
                if(handle_v_packet(ptr))
                    return;
                break;
            case 'D': handle_detach(); return;
            case 'k': handle_kill(); return;
            default:
                break;
        }

        put_packet(remcom_out_buffer);
    }
}

/* Returns the current IRQ context captured during a GDB exception. */
irq_context_t *gdb_get_irq_context(void) {
    return irq_ctx;
}

/* Updates whether a live debugger session is currently considered connected. */
void gdb_set_connected(bool is_connected) {
    connected = is_connected;
}


/* Generic SH4 exception entry point for debugger-visible faults. */
static void handle_exception(irq_t code, irq_context_t *context, void *data) {
    (void)data;

    irq_ctx = context;
    gdb_handle_exception(code);
}

static void handle_user_trapa(irq_t code, irq_context_t *context, void *data) {
    (void)code;
    (void)data;

    irq_ctx = context;
    gdb_handle_exception(EXC_TRAPA);
}

/*
   Handle TRAPA exceptions used internally by the stub for single-step traps
   and patched Z0 software breakpoints.

   Because the trap instruction replaces the original instruction in memory,
   the saved PC is rewound by one instruction before reporting EXC_TRAPA.
*/
static void handle_gdb_trapa(irq_t code, irq_context_t *context, void *data) {
    (void)code;
    (void)data;

    irq_ctx = context;
    irq_ctx->pc -= 2;
    gdb_handle_exception(EXC_TRAPA);
}

/*
   Triggers a software breakpoint using TRAPA #0xFF.

   Typically called at the start of a program to establish a connection
   with the debugger, or used to halt execution and enter the debugger manually.
*/
void gdb_breakpoint(void) {
    __asm__("trapa	#0xff"::);
}

/*
   Send a Wxx exit packet when a debugger is connected.

   This is only emitted after gdb_init() has run and the stub has seen at
   least one live debugger session.
*/
void gdb_shutdown(int status) {
    char *out;

    if(!initialized || !connected)
        return;

    out = gdb_get_out_buffer();
    snprintf(out, BUFMAX, "W%02x", status & 0xff);
    put_packet(out);
    connected = false;
}

/*
   Initialize the Dreamcast GDB stub and register its exception handlers.

   The transport backend is chosen at runtime by probing for dc-load GDB
   support and otherwise falling back to SCIF. Initialization ends by raising
   an initial breakpoint so the debugger can attach immediately.
*/
void gdb_init(void) {
    if(initialized)
        return;

    initialized = true;
    connected = false;

    if(dcload_gdbpacket(NULL, 0, NULL, 0) == 0)
        using_dcl = 1;
    else {
        using_dcl = 0;
        scif_set_parameters(57600, 1);
    }

    irq_set_handler(EXC_ILLEGAL_INSTR, handle_exception, NULL);
    irq_set_handler(EXC_SLOT_ILLEGAL_INSTR, handle_exception, NULL);
    irq_set_handler(EXC_DATA_ADDRESS_READ, handle_exception, NULL);
    irq_set_handler(EXC_DATA_ADDRESS_WRITE, handle_exception, NULL);
    irq_set_handler(EXC_USER_BREAK_PRE, handle_exception, NULL);
    irq_set_handler(EXC_USER_BREAK_POST, handle_exception, NULL);

    irq_set_handler(IRQ_TRAP_CODE(TRAPA_GDB_SINGLESTEP), handle_gdb_trapa, NULL);
    irq_set_handler(IRQ_TRAP_CODE(TRAPA_GDB_BREAKPOINT), handle_gdb_trapa, NULL);
    irq_set_handler(IRQ_TRAP_CODE(TRAPA_USER_BREAKPOINT), handle_user_trapa, NULL);

    gdb_breakpoint();
}
