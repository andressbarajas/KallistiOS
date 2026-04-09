/* KallistiOS ##version##

   kernel/gdb/gdb_mem.c

   Copyright (C) Megan Potter
   Copyright (C) Richard Moats
   Copyright (C) 2026 Andy Barajas

*/

#include <arch/arch.h>
#include <arch/cache.h>

#include <dc/memory.h>

#include "gdb_internal.h"

static bool is_valid_memory_address(uintptr_t addr, bool is_write) {
    uintptr_t normalized = (addr & MEM_AREA_CACHE_MASK) | MEM_AREA_P1_BASE;

    if(arch_valid_address(normalized) || arch_valid_text_address(normalized))
        return true;

    if(!is_write && addr >= MEM_AREA_P4_BASE)
        return true;

    return false;
}

static bool is_valid_memory_range(uint32_t addr, uint32_t len, bool is_write) {
    uintptr_t end_addr;

    if(len == 0)
        return true;

    end_addr = (uintptr_t)addr + (uintptr_t)len - 1u;
    if(end_addr < addr)
        return false;

    return is_valid_memory_address(addr, is_write) &&
           is_valid_memory_address(end_addr, is_write);
}

static char *append_escaped_binary_byte(char *out, unsigned char value) {
    if(value == '\0' || value == '$' || value == '#' ||
       value == '}' || value == '*') {
        *out++ = '}';
        *out++ = (char)(value ^ 0x20);
    }
    else {
        *out++ = (char)value;
    }

    return out;
}

static bool parse_binary_memory_header(char **ptr, uint32_t *addr, uint32_t *len) {
    return hex_to_int(ptr, addr) && *(*ptr)++ == ',' &&
           hex_to_int(ptr, len);
}

/*
 * Handle the 'm' command.
 * Reads memory from the target at a given address and length.
 * Format: mADDR,LEN
 */
void handle_read_mem(char *ptr) {
    uint32_t addr = 0;
    uint32_t len = 0;

    if(!parse_binary_memory_header(&ptr, &addr, &len) || *ptr != '\0') {
        gdb_error_with_code_str(GDB_EINVAL, "m: invalid packet");
        return;
    }

    if(!is_valid_memory_range(addr, len, false)) {
        gdb_error_with_code_str(GDB_EMEM_PROT, "m: invalid read range");
        return;
    }

    if(len > (BUFMAX - 1u) / 2u) {
        gdb_error_with_code_str(GDB_EMEM_SIZE, "m: read length too large");
        return;
    }

    mem_to_hex((const char *)addr, remcom_out_buffer, len);
}

/*
 * Handle the 'M' command.
 * Writes memory to the target at a given address.
 * Format: MADDR,LEN:DATA
 */
void handle_write_mem(char *ptr) {
    uint32_t addr = 0;
    uint32_t len = 0;

    if(parse_binary_memory_header(&ptr, &addr, &len) && *ptr++ == ':' &&
       (len == 0 || *ptr != '\0')) {
        if(!is_valid_memory_range(addr, len, true)) {
            gdb_error_with_code_str(GDB_EMEM_PROT, "M: invalid write range");
            return;
        }

        if(len > strlen(ptr) / 2u) {
            gdb_error_with_code_str(GDB_EMEM_SIZE, "M: short write payload");
            return;
        }

        hex_to_mem(ptr, (char *)addr, len);
        icache_flush_range(addr, len);
        gdb_put_ok();
    }
    else {
        gdb_error_with_code_str(GDB_EINVAL, "M: invalid packet");
    }
}

void handle_read_mem_binary(char *ptr) {
    uint32_t addr = 0;
    uint32_t len = 0;
    const unsigned char *src;
    char *out = remcom_out_buffer;

    if(!parse_binary_memory_header(&ptr, &addr, &len) || *ptr != '\0') {
        gdb_error_with_code_str(GDB_EINVAL, "x: invalid packet");
        return;
    }

    if(!is_valid_memory_range(addr, len, false)) {
        gdb_error_with_code_str(GDB_EMEM_PROT, "x: invalid read range");
        return;
    }

    src = (const unsigned char *)addr;
    for(uint32_t i = 0; i < len; ++i) {
        if((size_t)(out - remcom_out_buffer) > BUFMAX - 3u) {
            gdb_error_with_code_str(GDB_EMEM_SIZE, "x: response too large");
            return;
        }

        out = append_escaped_binary_byte(out, src[i]);
    }

    *out = '\0';
}

static void unescape_binary_data(const unsigned char *src, char *dest,
                                 uint32_t output_len) {
    for(uint32_t i = 0; i < output_len; ++i) {
        unsigned char value = *src++;

        if(value == '}')
            value = (*src++) ^ 0x20;

        dest[i] = (char)value;
    }
}

void handle_write_mem_binary(char *ptr) {
    uint32_t addr = 0;
    uint32_t len = 0;
    char tmp[BUFMAX];

    if(!parse_binary_memory_header(&ptr, &addr, &len) || *ptr++ != ':') {
        gdb_error_with_code_str(GDB_EINVAL, "X: invalid packet");
        return;
    }

    if(!is_valid_memory_range(addr, len, true)) {
        gdb_error_with_code_str(GDB_EMEM_PROT, "X: invalid write range");
        return;
    }

    if(len > sizeof(tmp)) {
        gdb_error_with_code_str(GDB_EMEM_SIZE, "X: write length too large");
        return;
    }

    unescape_binary_data((const unsigned char *)ptr, tmp, len);
    memcpy((void *)addr, tmp, len);
    icache_flush_range(addr, len);
    gdb_put_ok();
}
