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
     - vCont;{c,s}[:thread-id][;...]
     - vMustReplyEmpty
     - vCtrlC
     - vKill
*/

#include <arch/arch.h>

#include "gdb_internal.h"

typedef struct {
    char type;
    int tid;
    bool has_thread;
} vcont_action_t;

static bool parse_vcont_thread_id(char *ptr, int *tid) {
    uint32_t parsed_tid = 0;

    if(ptr[0] == '-' && ptr[1] == '1' && ptr[2] == '\0') {
        *tid = GDB_THREAD_ALL;
        return true;
    }

    if(!hex_to_int(&ptr, &parsed_tid) || *ptr != '\0')
        return false;

    *tid = (int)parsed_tid;

    if(*tid > GDB_THREAD_ANY && !thd_by_tid((tid_t)*tid))
        return false;

    return true;
}

static bool parse_vcont_action(char *token, vcont_action_t *action) {
    memset(action, 0, sizeof(*action));
    action->tid = GDB_THREAD_ANY;

    if(*token == '\0') {
        gdb_error_with_code_str(GDB_EINVAL, "vCont: empty action");
        return false;
    }

    action->type = *token++;

    if(action->type != 'c' && action->type != 's') {
        gdb_error_with_code_str(GDB_EUNIMPL, "vCont: unsupported action");
        return false;
    }

    if(*token == '\0')
        return true;

    if(*token != ':') {
        gdb_error_with_code_str(GDB_EINVAL, "vCont: invalid action");
        return false;
    }

    if(!parse_vcont_thread_id(token + 1, &action->tid)) {
        gdb_error_with_code_str(GDB_EINVAL, "vCont: invalid thread-id");
        return false;
    }

    action->has_thread = true;
    return true;
}

static bool run_vcont_action(const vcont_action_t *action) {
    int saved_tid = get_ctrl_thread();
    char command[] = "c";
    bool resumed;

    if(action->has_thread)
        set_ctrl_thread(action->tid);

    command[0] = action->type;
    resumed = handle_continue_step(command + 1);

    if(action->has_thread)
        set_ctrl_thread(saved_tid);

    return resumed;
}

/*
   Processes GDB 'vCont' subcommands.

   This all-stop stub supports the common subset that GDB uses for threaded
   resume control: continue/step actions, optional thread qualifiers, and
   mixed packets such as 'vCont;s:tid;c'. More complex vCont actions still
   return an unsupported-feature error.
*/
static bool handle_vcont(char *ptr) {
    vcont_action_t selected = { 0 };
    bool have_selected = false;
    char *action;

    if(strcmp(ptr, "Cont?") == 0) {
        gdb_put_str("vCont;c;s");
        return false;
    }

    if(strncmp(ptr, "Cont;", 5) != 0) {
        gdb_error_with_code_str(GDB_EUNIMPL, "vCont: unsupported action");
        return false;
    }

    action = ptr + 5;

    while(action) {
        vcont_action_t parsed;
        char *next = strchr(action, ';');

        if(next)
            *next++ = '\0';

        if(!parse_vcont_action(action, &parsed))
            return false;

        if(parsed.type == 's') {
            if(have_selected && selected.type == 's') {
                gdb_error_with_code_str(GDB_EUNIMPL,
                                        "vCont: multiple step actions unsupported");
                return false;
            }

            selected = parsed;
            have_selected = true;
        }
        else if(!have_selected) {
            selected = parsed;
            have_selected = true;
        }

        action = next;
    }

    if(!have_selected) {
        gdb_error_with_code_str(GDB_EINVAL, "vCont: missing action");
        return false;
    }

    return run_vcont_action(&selected);
}

/*
   Handle GDB 'v' packets for extended operations.

   Supported subcommands:
     - vCont?          → Report supported actions ('c' and 's')
     - vCont;c         → Continue execution
     - vCont;s         → Step one instruction
     - vCont;{c,s}...  → Thread-qualified/multi-action continue or step
     - vMustReplyEmpty → Acknowledge empty reply support
     - vCtrlC         → Reboot the target after acknowledging the packet
     - vKill          → Abort execution
*/
bool handle_v_packet(char *ptr) {
    if(strcmp(ptr, "Cont?") == 0 || strncmp(ptr, "Cont;", 5) == 0)
        return handle_vcont(ptr);

    if(strcmp(ptr, "CtrlC") == 0) {
        put_packet(GDB_OK);
        arch_reboot();
    }

    if(strncmp(ptr, "Kill", 4) == 0 && (ptr[4] == '\0' || ptr[4] == ';')) {
        handle_kill();
        return true;
    }

    if(strcmp(ptr, "MustReplyEmpty") == 0) {
        gdb_clear_out_buffer();
        return false;
    }

    gdb_clear_out_buffer();
    return false;
}
