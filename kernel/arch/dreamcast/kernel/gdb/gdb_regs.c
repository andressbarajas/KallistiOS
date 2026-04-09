/* KallistiOS ##version##

   kernel/gdb/gdb_regs.c

   Copyright (C) Megan Potter
   Copyright (C) Richard Moats
   Copyright (C) 2026 Andy Barajas

*/

#include "gdb_internal.h"

/* map from KOS register context order to GDB sh4 order */
#define KOS_REG(r)      offsetof(irq_context_t, r)

uint32_t kos_reg_map[] = {
    KOS_REG(r[0]), KOS_REG(r[1]), KOS_REG(r[2]), KOS_REG(r[3]),
    KOS_REG(r[4]), KOS_REG(r[5]), KOS_REG(r[6]), KOS_REG(r[7]),
    KOS_REG(r[8]), KOS_REG(r[9]), KOS_REG(r[10]), KOS_REG(r[11]),
    KOS_REG(r[12]), KOS_REG(r[13]), KOS_REG(r[14]), KOS_REG(r[15]),

    KOS_REG(pc), KOS_REG(pr), KOS_REG(gbr), KOS_REG(vbr),
    KOS_REG(mach), KOS_REG(macl), KOS_REG(sr),
    KOS_REG(fpul), KOS_REG(fpscr),

    KOS_REG(fr[0]), KOS_REG(fr[1]), KOS_REG(fr[2]), KOS_REG(fr[3]),
    KOS_REG(fr[4]), KOS_REG(fr[5]), KOS_REG(fr[6]), KOS_REG(fr[7]),
    KOS_REG(fr[8]), KOS_REG(fr[9]), KOS_REG(fr[10]), KOS_REG(fr[11]),
    KOS_REG(fr[12]), KOS_REG(fr[13]), KOS_REG(fr[14]), KOS_REG(fr[15])
};

#undef KOS_REG

/*
 * GDB's raw SH4 remote register block is 67 32-bit registers:
 *  - 41 directly readable registers we map from irq_context_t
 *  - 18 unavailable raw slots for ssr/spc and banked r0-r7 values
 *  - 8 reserved blank raw slots
 *
 * Pseudo registers like dr0-dr14 and fv0-fv12 are not part of the g/G block.
 */
#define BASE_REG_COUNT         ((int)(sizeof(kos_reg_map) / sizeof(kos_reg_map[0])))
#define UNAVAILABLE_REG_COUNT  18
#define RESERVED_REG_COUNT     8
#define RAW_REG_COUNT          (BASE_REG_COUNT + UNAVAILABLE_REG_COUNT + RESERVED_REG_COUNT)
#define PSEUDO_ZERO_REGNUM     RAW_REG_COUNT
#define DOUBLE_REG_BASE        (PSEUDO_ZERO_REGNUM + 1)
#define DOUBLE_REG_COUNT       8
#define VECTOR_REG_BASE        (DOUBLE_REG_BASE + DOUBLE_REG_COUNT)
#define VECTOR_REG_COUNT       4

static int32_t gdb_thread_for_regs = GDB_THREAD_ANY;
static irq_context_t *regs_irq_ctx;

void set_regs_thread(int tid) {
    gdb_thread_for_regs = tid;
}

void setup_regs_context(void) {
    irq_context_t *default_context = gdb_get_irq_context();

    if(gdb_thread_for_regs == GDB_THREAD_ALL ||
       gdb_thread_for_regs == GDB_THREAD_ANY) {
        regs_irq_ctx = default_context;
        return;
    }

    {
        kthread_t *target = thd_by_tid((tid_t)gdb_thread_for_regs);
        regs_irq_ctx = target ? &target->context : default_context;
    }
}

static uint64_t build_dr(uint32_t fr_low, uint32_t fr_high) {
    return ((uint64_t)fr_high << 32) | fr_low;
}

static void build_fv(uint32_t fv[4], const irq_context_t *context, int base) {
    fv[0] = context->fr[base + 0];
    fv[1] = context->fr[base + 1];
    fv[2] = context->fr[base + 2];
    fv[3] = context->fr[base + 3];
}

static char *append_zero_reg_hex(char *out) {
    static const uint32_t zero;
    return mem_to_hex((const char *)&zero, out, sizeof(zero));
}

static bool read_register_hex(char *out, int regnum) {
    irq_context_t *context;

    if(regnum < 0)
        return false;

    setup_regs_context();
    context = regs_irq_ctx;

    if(regnum < BASE_REG_COUNT) {
        uint32_t *reg_ptr =
            (uint32_t *)((uintptr_t)context + kos_reg_map[regnum]);
        mem_to_hex((const char *)reg_ptr, out, sizeof(*reg_ptr));
        return true;
    }

    regnum -= BASE_REG_COUNT;

    if(regnum < UNAVAILABLE_REG_COUNT + RESERVED_REG_COUNT) {
        append_zero_reg_hex(out);
        return true;
    }

    regnum -= UNAVAILABLE_REG_COUNT + RESERVED_REG_COUNT;

    if(regnum == 0) {
        append_zero_reg_hex(out);
        return true;
    }

    --regnum;

    if(regnum < DOUBLE_REG_COUNT) {
        int base = regnum * 2;
        uint64_t dr = build_dr(context->fr[base], context->fr[base + 1]);
        mem_to_hex((const char *)&dr, out, sizeof(dr));
        return true;
    }

    regnum -= DOUBLE_REG_COUNT;

    if(regnum < VECTOR_REG_COUNT) {
        int base = regnum * 4;
        uint32_t fv[4];
        build_fv(fv, context, base);
        mem_to_hex((const char *)fv, out, sizeof(fv));
        return true;
    }

    return false;
}

static bool write_register_hex(int regnum, const char *in) {
    irq_context_t *context;

    if(regnum < 0)
        return false;

    setup_regs_context();
    context = regs_irq_ctx;

    if(regnum < BASE_REG_COUNT) {
        hex_to_mem(in, (char *)((uintptr_t)context + kos_reg_map[regnum]), 4);
        return true;
    }

    regnum -= BASE_REG_COUNT;

    if(regnum < UNAVAILABLE_REG_COUNT + RESERVED_REG_COUNT)
        return true;

    regnum -= UNAVAILABLE_REG_COUNT + RESERVED_REG_COUNT;

    if(regnum == 0)
        return true;

    --regnum;

    if(regnum < DOUBLE_REG_COUNT) {
        int base = regnum * 2;
        uint64_t dr = 0;

        hex_to_mem(in, (char *)&dr, sizeof(dr));
        context->fr[base] = (uint32_t)(dr & 0xffffffffu);
        context->fr[base + 1] = (uint32_t)(dr >> 32);
        return true;
    }

    regnum -= DOUBLE_REG_COUNT;

    if(regnum < VECTOR_REG_COUNT) {
        int base = regnum * 4;
        uint32_t fv[4];

        hex_to_mem(in, (char *)fv, sizeof(fv));
        for(int i = 0; i < 4; i++)
            context->fr[base + i] = fv[i];
        return true;
    }

    return false;
}

/*
 * Handle the 'p' command.
 * Returns the value of a single register.
 */
void handle_read_reg(char *ptr) {
    uint32_t regnum = 0;

    if(!hex_to_int(&ptr, &regnum) || *ptr != '\0' ||
       !read_register_hex(remcom_out_buffer, (int)regnum)) {
        strcpy(remcom_out_buffer, "E01");
    }
}

/*
 * Handle the 'P' command.
 * Writes a single register in the form Pn...=r...
 */
void handle_write_reg(char *ptr) {
    uint32_t regnum = 0;

    if(!hex_to_int(&ptr, &regnum) || *ptr++ != '=' ||
       !write_register_hex((int)regnum, ptr)) {
        strcpy(remcom_out_buffer, "E01");
        return;
    }

    strcpy(remcom_out_buffer, GDB_OK);
}

/*
 * Handle the 'g' command.
 * Returns the full set of general-purpose registers.
 */
void handle_read_regs(char *ptr) {
    (void)ptr;

    char *out = remcom_out_buffer;
    irq_context_t *context;

    setup_regs_context();
    context = regs_irq_ctx;

    for(int i = 0; i < BASE_REG_COUNT; i++)
        out = mem_to_hex((char *)((uint32_t)context + kos_reg_map[i]), out, 4);

    for(int i = 0; i < UNAVAILABLE_REG_COUNT + RESERVED_REG_COUNT; i++)
        out = append_zero_reg_hex(out);

    *out = 0;
}

/*
 * Handle the 'G' command.
 * Writes to all general-purpose registers.
 * Format: Gxxxxxxxx.... (entire register state in hex).
 */
void handle_write_regs(char *ptr) {
    char *in = ptr;
    size_t remaining = strlen(in);
    irq_context_t *context;

    if(remaining < (size_t)RAW_REG_COUNT * 8) {
        strcpy(remcom_out_buffer, "E01");
        return;
    }

    setup_regs_context();
    context = regs_irq_ctx;

    for(int i = 0; i < BASE_REG_COUNT; i++, in += 8)
        hex_to_mem(in, (char *)((uint32_t)context + kos_reg_map[i]), 4);

    in += (UNAVAILABLE_REG_COUNT + RESERVED_REG_COUNT) * 8;

    strcpy(remcom_out_buffer, GDB_OK);
}
