/* KallistiOS ##version##

   arch/dreamcast/kernel/gdb/gdb_vpacket.c

   Copyright (C) 2026 Andy Barajas

*/

/*
   Implements extended 'v' packet handling for the GDB remote stub.

   Supported subcommands:
     - vCont?      : advertise supported vCont actions
     - vCont;c     : continue execution
     - vCont;s     : single-step execution
     - vMustReplyEmpty
     - vCtrlC
     - vKill
*/

#include <arch/arch.h>

#include "gdb_internal.h"

/*
   Processes GDB 'vCont' subcommands.
   Currently supports:
     - 'vCont?'  -> advertise supported actions
     - 'vCont;c' → continue
     - 'vCont;s' → single-step
*/
static bool handle_vcont(char *ptr) {
    if(strncmp(ptr, "Cont?", 5) == 0) {
        gdb_put_str("vCont;c;s");
        return false;
    }

    if(strcmp(ptr, "Cont;c") == 0) {
        char command[] = "c";
        handle_continue_step(command + 1);
        return true;
    }

    if(strcmp(ptr, "Cont;s") == 0) {
        char command[] = "s";
        handle_continue_step(command + 1);
        return true;
    }

    gdb_error_with_code_str(GDB_EUNIMPL, "vCont: unsupported action");
    return false;
}

/*
   Handle GDB 'v' packets for extended operations.

   Supported subcommands:
     - vCont?         → Report supported actions ('c' and 's')
     - vCont;c        → Continue execution
     - vCont;s        → Step one instruction
     - vMustReplyEmpty → Acknowledge empty reply support
     - vCtrlC         → Reboot the target after acknowledging the packet
     - vKill          → Abort execution
*/
bool handle_v_packet(char *ptr) {
    if(strncmp(ptr, "Cont", 4) == 0)
        return handle_vcont(ptr);

    if(strcmp(ptr, "CtrlC") == 0) {
        put_packet(GDB_OK);
        arch_reboot();
    }

    if(strcmp(ptr, "Kill") == 0)
        handle_kill();

    if(strncmp(ptr, "MustReplyEmpty", 14) == 0) {
        gdb_clear_out_buffer();
        return false;
    }

    gdb_clear_out_buffer();
    return false;
}
