/* KallistiOS ##version##

   arch/dreamcast/kernel/gdb/gdb_ctrl.c

   Copyright (C) Megan Potter
   Copyright (C) Richard Moats
   Copyright (C) 2026 Andy Barajas

*/

/*
   Implements continue and single-step control for the GDB remote stub.

   Supported control packets:
     - c / s         : continue and single-step
     - Cxx / Sxx     : continue and single-step with signal
     - Hg / Hc       : select threads for register or execution control

   Single-step is implemented by decoding the next SH4 instruction and
   patching the computed stop location with an internal TRAPA trap.
*/

#include <arch/irq.h>
#include <arch/cache.h>

#include "gdb_internal.h"


/* Hitachi SH architecture instruction encoding masks */
#define COND_BR_MASK   0xff00
#define UCOND_DBR_MASK 0xe000
#define UCOND_RBR_MASK 0xf0df
#define TRAPA_MASK     0xff00

#define COND_DISP      0x00ff
#define UCOND_DISP     0x0fff
#define UCOND_REG      0x0f00

/* Hitachi SH instruction opcodes */
#define BF_INSTR       0x8b00
#define BFS_INSTR      0x8f00
#define BT_INSTR       0x8900
#define BTS_INSTR      0x8d00
#define BRA_INSTR      0xa000
#define BSR_INSTR      0xb000
#define JMP_INSTR      0x402b
#define JSR_INSTR      0x400b
#define RTS_INSTR      0x000b
#define RTE_INSTR      0x002b
#define TRAPA_INSTR    0xc300
#define SSTEP_INSTR    0xc320

/* Hitachi SH processor register masks */
#define T_BIT_MASK     0x0001

typedef struct {
    short *mem_addr;
    short old_instr;
} step_data_t;

static bool stepped;
static step_data_t instr_buffer;
static int32_t gdb_thread_for_ctrl = GDB_THREAD_ANY;
static irq_context_t *ctrl_irq_ctx;

static void do_single_step(void);

static void resume_target(bool stepping, bool set_pc, uint32_t pc) {
    setup_ctrl_context();

    if(set_pc)
        ctrl_irq_ctx->pc = pc;

    if(stepping)
        do_single_step();
}

/* Sets the target thread for control operations (continue/step). */
void set_ctrl_thread(int tid) {
    gdb_thread_for_ctrl = tid;
}

/*
   Sets the IRQ context used for control operations like continue/step.
   If the selected thread is "any" or "all", or the requested thread no longer
   exists, it falls back to the interrupted thread's current context.
*/
void setup_ctrl_context(void) {
    irq_context_t *default_context = gdb_get_irq_context();

    if(gdb_thread_for_ctrl == GDB_THREAD_ALL ||
       gdb_thread_for_ctrl == GDB_THREAD_ANY) {
        ctrl_irq_ctx = default_context;
        return;
    }

    kthread_t *target = thd_by_tid((tid_t)gdb_thread_for_ctrl);
    ctrl_irq_ctx = target ? &target->context : default_context;
}

/*
   Prepares for single-step execution by patching the computed next stop
   address with a TRAPA. Branches, delay slots, returns, and TRAPA opcodes are
   decoded so the trap lands where execution would naturally continue.
*/
static void do_single_step(void) {
    short *instr_mem;
    int displacement;
    int reg;
    unsigned short opcode, br_opcode;

    stepped = true;
    setup_ctrl_context();
    instr_mem = (short *)ctrl_irq_ctx->pc;
    opcode = *instr_mem;
    br_opcode = opcode & COND_BR_MASK;

    if(br_opcode == BT_INSTR || br_opcode == BTS_INSTR) {
        if(ctrl_irq_ctx->sr & T_BIT_MASK) {
            displacement = (opcode & COND_DISP) << 1;

            if(displacement & 0x80)
                displacement |= 0xffffff00;

            /*
               Remember PC points to second instr.
               after PC of branch ... so add 4
            */
            instr_mem = (short *)(ctrl_irq_ctx->pc + displacement + 4);
        }
        else {
            /* Can't safely place trapa in BT/S delay slot */
            instr_mem += (br_opcode == BTS_INSTR) ? 2 : 1;
        }
    }
    else if(br_opcode == BF_INSTR || br_opcode == BFS_INSTR) {
        if(ctrl_irq_ctx->sr & T_BIT_MASK) {
            /* Can't put a trapa in the delay slot of a bf/s instruction */
            instr_mem += (br_opcode == BFS_INSTR) ? 2 : 1;
        }
        else {
            displacement = (opcode & COND_DISP) << 1;

            if(displacement & 0x80)
                displacement |= 0xffffff00;

            /*
               Remember PC points to second instr.
               after PC of branch ... so add 4
            */
            instr_mem = (short *)(ctrl_irq_ctx->pc + displacement + 4);
        }
    }
    else if((opcode & UCOND_DBR_MASK) == BRA_INSTR) {
        displacement = (opcode & UCOND_DISP) << 1;

        if(displacement & 0x0800)
            displacement |= 0xfffff000;

        /*
          Remember PC points to second instr.
          after PC of branch ... so add 4
        */
        instr_mem = (short *)(ctrl_irq_ctx->pc + displacement + 4);
    }
    else if((opcode & UCOND_RBR_MASK) == JSR_INSTR) {
        reg = (char)((opcode & UCOND_REG) >> 8);

        instr_mem = (short *)ctrl_irq_ctx->r[reg];
    }
    else if(opcode == RTS_INSTR)
        instr_mem = (short *)ctrl_irq_ctx->pr;
    else if(opcode == RTE_INSTR)
        instr_mem = (short *)ctrl_irq_ctx->r[15];
    else if((opcode & TRAPA_MASK) == TRAPA_INSTR)
        instr_mem = (short *)((opcode & ~TRAPA_MASK) << 2);
    else
        instr_mem += 1;

    instr_buffer.mem_addr = instr_mem;
    instr_buffer.old_instr = *instr_mem;
    *instr_mem = SSTEP_INSTR;
    icache_flush_range((uint32_t)instr_mem, 2);
}

/*
   Undo the effect of a previous do_single_step.  If we single stepped,
   restore the old instruction.
*/
void undo_single_step(void) {
    if(stepped) {
        short *instr_mem;
        instr_mem = instr_buffer.mem_addr;
        *instr_mem = instr_buffer.old_instr;
        icache_flush_range((uint32_t)instr_mem, 2);
    }

    stepped = false;
}

/*
   Handle the 'c' (continue) and 's' (single-step) GDB commands.

   These commands resume execution of the program, optionally from a new PC.
   - 'c' continues execution normally.
   - 's' performs a single instruction step.

   Format:
     - 'c'           → continue from current PC
     - 'cXXXX'       → continue from address XXXX
     - 's'           → single-step from current PC
     - 'sXXXX'       → single-step from address XXXX

   This function parses the optional address (if present), updates the selected
   control thread's PC, and resumes that thread. If single-stepping, it
   prepares the computed next stop location for trapping.
*/
void handle_continue_step(char *ptr) {
    bool stepping = (ptr[-1] == 's');
    uint32_t addr = 0;
    bool set_pc = hex_to_int(&ptr, &addr) != 0;

    resume_target(stepping, set_pc, addr);
}

/*
   Handles the 'C' and 'S' packets for continue or step with signal.

   Format:
     - 'Cxx'         → continue with signal xx
     - 'Sxx'         → step one instruction with signal xx
     - 'Cxx;ADDR'    → continue from address ADDR with signal xx
     - 'Sxx;ADDR'    → step from address ADDR with signal xx

   Signals are ignored on SH4; this just resumes or steps the selected control
   thread as needed. If 'S' is used, single-step mode is enabled before
   continuing.

   ptr points to the character after 'C' or 'S'.
*/
bool handle_continue_step_signal(char *ptr) {
    bool stepping = (ptr[-1] == 'S');
    uint32_t signal = 0;
    uint32_t addr = 0;
    bool set_pc = false;

    /* Parse signal (always two hex digits) */
    if(hex_to_int(&ptr, &signal) != 2) {
        gdb_error_with_code_str(GDB_EINVAL, "C/S: invalid signal packet");
        return false;
    }

    /* Optional: skip semicolon and parse new PC if present */
    if(*ptr == ';') {
        ++ptr;

        if(!hex_to_int(&ptr, &addr) || *ptr != '\0') {
            gdb_error_with_code_str(GDB_EINVAL, "C/S: invalid address packet");
            return false;
        }

        set_pc = true;
    }
    else if(*ptr != '\0') {
        gdb_error_with_code_str(GDB_EINVAL, "C/S: invalid packet");
        return false;
    }

    /* SH4 does not use the supplied signal value here. */
    (void)signal;
    resume_target(stepping, set_pc, addr);
    return true;
}

/*
   Handle the 'H' packet to select the active thread for GDB operations.

   Format:
     - HgXX → Set thread for register ops (g, G, p, P)
     - HcXX → Set thread for control ops (c, s, C, S, and vCont continue/step)

   XX is a thread ID in hex. Special values:
     - 0        → Any thread (default)
     - 0xFFFFFFFF (or -1) → All threads

   Unsupported selectors and unknown thread IDs return invalid-argument errors.
*/
void handle_thread_select(char *ptr) {
    int tid = GDB_THREAD_ANY;
    char type = *ptr++;
    uint32_t parsed_tid = 0;

    if(*ptr == '-' && ptr[1] == '1' && ptr[2] == '\0')
        tid = GDB_THREAD_ALL;
    else if(hex_to_int(&ptr, &parsed_tid) && *ptr == '\0')
        tid = (int)parsed_tid;
    else {
        gdb_error_with_code_str(GDB_EINVAL, "H: invalid thread selector");
        return;
    }

    if(tid > GDB_THREAD_ANY && !thd_by_tid((tid_t)tid)) {
        gdb_error_with_code_str(GDB_EINVAL, "H: unknown thread");
        return;
    }

    if(type == 'g')
        set_regs_thread(tid);
    else if(type == 'c')
        set_ctrl_thread(tid);
    else {
        gdb_error_with_code_str(GDB_EINVAL, "H: unsupported selector");
        return;
    }

    gdb_put_ok();
}
