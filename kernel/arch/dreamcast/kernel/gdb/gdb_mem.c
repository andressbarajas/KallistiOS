/* KallistiOS ##version##

   arch/dreamcast/kernel/gdb/gdb_mem.c

   Copyright (C) Megan Potter
   Copyright (C) Richard Moats
   Copyright (C) 2026 Andy Barajas

*/

/*
   Implements GDB memory access packets for Dreamcast targets.

   Supported packet families:
     - m / M : hexadecimal memory reads and writes
     - x / X : binary memory reads and writes

   The handlers reject P4 SH-internal space, heuristically validate other
   ranges using KOS address helpers, escape outbound binary replies and
   unescape inbound binary payloads as required by RSP, and flush the
   instruction cache after writes that may affect code.
*/

#include <arch/arch.h>
#include <arch/cache.h>

#include <dc/memory.h>

#include "gdb_internal.h"

/*
   Heuristically validates a single address for GDB memory access.

   P4 SH-internal space is rejected outright; all other addresses are
   normalized to the cached P1 alias before consulting KOS's generic address
   helpers.
*/
static bool is_valid_memory_address(uintptr_t addr, bool is_write) {
    if(addr >= MEM_AREA_P4_BASE)
        return false;

    uintptr_t normalized = (addr & MEM_AREA_CACHE_MASK) | MEM_AREA_P1_BASE;

    if(arch_valid_address(normalized) || arch_valid_text_address(normalized))
        return true;

    return false;
}

/*
   Validates an address range by checking the first and last accessed bytes.

   Zero-length accesses are allowed. Overflow in the computed end address is
   treated as invalid.
*/
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

static bool hex_to_mem_checked(const char *src, void *dest, uint32_t count) {
    if(!src || !dest)
        return false;

    for(uint32_t i = 0; i < count; ++i) {
        int high = hex(src[i * 2u]);
        int low = hex(src[(i * 2u) + 1u]);

        if(high < 0 || low < 0)
            return false;
    }

    hex_to_mem(src, (char *)dest, count);
    return true;
}

/*
   Handle the 'm' command.
   Reads memory from the target at a given address and length.
   Format: mADDR,LEN
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
   Handle the 'M' command.
   Writes memory to the target at a given address.
   Format: MADDR,LEN:DATA

   DATA is hex-encoded.
*/
void handle_write_mem(char *ptr) {
    uint32_t addr = 0;
    uint32_t len = 0;
    size_t payload_len;

    if(!parse_binary_memory_header(&ptr, &addr, &len) || *ptr++ != ':') {
        gdb_error_with_code_str(GDB_EINVAL, "M: invalid packet");
        return;
    }

    if(len == 0 && *ptr != '\0') {
        gdb_error_with_code_str(GDB_EINVAL, "M: invalid packet");
        return;
    }

    if(!is_valid_memory_range(addr, len, true)) {
        gdb_error_with_code_str(GDB_EMEM_PROT, "M: invalid write range");
        return;
    }

    payload_len = strlen(ptr);
    if(payload_len < (size_t)len * 2u) {
        gdb_error_with_code_str(GDB_EMEM_SIZE, "M: short write payload");
        return;
    }

    if(payload_len != (size_t)len * 2u) {
        gdb_error_with_code_str(GDB_EINVAL, "M: invalid packet");
        return;
    }

    if(!hex_to_mem_checked(ptr, (char *)addr, len)) {
        gdb_error_with_code_str(GDB_EINVAL, "M: invalid hex payload");
        return;
    }

    icache_flush_range(addr, len);
    gdb_put_ok();
}

/*
   Handle the 'x' command.
   Reads memory from the target and returns it as RSP-escaped binary data.
   Format: xADDR,LEN

   - ADDR: Target address in hex
   - LEN:  Number of bytes to read

   Special characters are escaped according to the GDB remote protocol.
*/
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

/*
   Decodes an escaped RSP binary payload into exactly output_len bytes.

   Returns false if the escaped source data is truncated or if it does not
   decode to exactly the requested output length.
*/
static bool unescape_binary_data(const unsigned char *src, size_t src_len,
                                 char *dest, uint32_t output_len) {
    size_t src_pos = 0;

    for(uint32_t i = 0; i < output_len; ++i) {
        unsigned char value;

        if(src_pos >= src_len)
            return false;

        value = src[src_pos++];

        if(value == '}') {
            if(src_pos >= src_len)
                return false;

            value = src[src_pos++] ^ 0x20;
        }

        dest[i] = (char)value;
    }

    return src_pos == src_len;
}

/*
   Handle the 'X' command.
   Writes binary data to target memory at a given address.
   Format: XADDR,LEN:DATA

   - ADDR: Target address in hex
   - LEN:  Number of bytes to write
   - DATA: Raw binary data (not hex-encoded), may include RSP escape sequences
*/
void handle_write_mem_binary(char *ptr) {
    char *packet_start = ptr;
    uint32_t addr = 0;
    uint32_t len = 0;
    char tmp[BUFMAX];
    size_t packet_len = gdb_get_in_packet_length();
    size_t args_len;
    size_t escaped_len;

    if(packet_len == 0) {
        gdb_error_with_code_str(GDB_EINVAL, "X: invalid packet");
        return;
    }

    args_len = packet_len - 1u;

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

    if((size_t)(ptr - packet_start) > args_len) {
        gdb_error_with_code_str(GDB_EINVAL, "X: invalid packet");
        return;
    }

    escaped_len = args_len - (size_t)(ptr - packet_start);
    if(!unescape_binary_data((const unsigned char *)ptr, escaped_len, tmp, len)) {
        gdb_error_with_code_str(GDB_EINVAL, "X: invalid binary payload");
        return;
    }

    memcpy((void *)addr, tmp, len);
    icache_flush_range(addr, len);
    gdb_put_ok();
}
