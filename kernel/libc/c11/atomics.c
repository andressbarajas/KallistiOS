/* KallistiOS ##version##

   atomics.c
   Copyright (C) 2023 Falco Girgis
*/

/* This file provides the C11 atomic support symbols GCC expects from
   libatomic, which the bare-metal toolchains do not ship. The compiler
   emits calls to them whenever it cannot expand an atomic operation
   inline, either because the target has no suitable instruction or
   because the operation is wider than one it can perform atomically.
*/

#include <kos/cache.h>
#include <kos/irq.h>
#include <kos/mutex.h>
#include <arch/arch.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>

/* Create a set of macros to codegen atomic symbols for primitive types.
   For these types, we simply disable interrupts then re-enable them
   around accesses to our atomics to ensure their atomicity.
*/
#define ATOMIC_LOAD_N_(type, n) \
    __weak_symbol type \
    __atomic_load_##n(const volatile void *ptr, int model) { \
        (void)model; \
        irq_disable_scoped(); \
        return *(type *)ptr; \
    }

#define ATOMIC_STORE_N_(type, n) \
    __weak_symbol void \
    __atomic_store_##n(volatile void *ptr, type val, int model) { \
        (void)model; \
        irq_disable_scoped(); \
        *(type *)ptr = val; \
    }

#define ATOMIC_EXCHANGE_N_(type, n) \
    __weak_symbol type \
    __atomic_exchange_##n(volatile void* ptr, type val, int model) { \
        irq_disable_scoped(); \
        const type ret = *(type *)ptr; \
        (void)model; \
        *(type*)ptr = val; \
        return ret; \
    }

#define ATOMIC_COMPARE_EXCHANGE_N_(type, n) \
    __weak_symbol bool \
    __atomic_compare_exchange_##n(volatile void *ptr, \
                                  void *expected, \
                                  type desired, \
                                  bool weak, \
                                  int success_memorder, \
                                  int failure_memorder) { \
        (void)weak; \
        (void)success_memorder; \
        (void)failure_memorder; \
        irq_disable_scoped(); \
        if(*(type *)ptr == *(type *)expected) { \
            *(type *)ptr = desired; \
            return true; \
        } else { \
            *(type *)expected = *(type *)ptr; \
            return false; \
        } \
    }

#define ATOMIC_FETCH_N_(type, n, opname, op) \
    __weak_symbol type \
    __atomic_fetch_##opname##_##n(volatile void* ptr, \
                                  type val, \
                                  int memorder) { \
        irq_disable_scoped(); \
        type ret = *(type *)ptr; \
        (void)memorder; \
        *(type *)ptr op val; \
        return ret;  \
    }

#define ATOMIC_FETCH_NAND_N_(type, n) \
    __weak_symbol type \
    __atomic_fetch_nand_##n(volatile void* ptr, \
                            type val, \
                            int memorder) { \
        irq_disable_scoped(); \
        type ret = *(type *)ptr; \
        (void)memorder; \
        *(type *)ptr = ~(*(type *)ptr & val); \
        return ret;  \
    }

#define ATOMIC_OPS_N_(type, n) \
    ATOMIC_LOAD_N_(type, n) \
    ATOMIC_STORE_N_(type, n) \
    ATOMIC_EXCHANGE_N_(type, n) \
    ATOMIC_COMPARE_EXCHANGE_N_(type, n) \
    ATOMIC_FETCH_N_(type, n, add, +=) \
    ATOMIC_FETCH_N_(type, n, sub, -=) \
    ATOMIC_FETCH_N_(type, n, and, &=) \
    ATOMIC_FETCH_N_(type, n, or, |=) \
    ATOMIC_FETCH_N_(type, n, xor, ^=) \
    ATOMIC_FETCH_NAND_N_(type, n)

ATOMIC_OPS_N_(unsigned char, 1)
ATOMIC_OPS_N_(unsigned short, 2)
ATOMIC_OPS_N_(unsigned int, 4)
ATOMIC_OPS_N_(unsigned long long, 8)

/* Provide GCC with symbols and logic required to implement
   generically sized atomics. Rather than disabling an enabling
   interrupts around primitive assignments, we use mutexes
   around memcpy() calls. */

/* Size of each memory region covered by an individual lock. */
#define GENERIC_LOCK_BLOCK_SIZE     (CACHE_L1_DCACHE_LINESIZE * 4)

/* Locks have to be shared for each page with the MMU enabled,
   otherwise we can fail when aliasing an address range to multiple
   pages. */
#define GENERIC_LOCK_COUNT          (PAGESIZE / GENERIC_LOCK_BLOCK_SIZE)

/* Create a hash table mapping a region of memory to a lock. */
static mutex_t locks[GENERIC_LOCK_COUNT] = {
    [0 ... (GENERIC_LOCK_COUNT - 1)] = MUTEX_INITIALIZER
};

/* Our hash function which maps an address to its corresponding lock index. */
inline static uintptr_t
address_to_mutex(const volatile void *ptr) {
    return ((uintptr_t)ptr / GENERIC_LOCK_BLOCK_SIZE) % GENERIC_LOCK_COUNT;
}

void __atomic_load(size_t size,
                   const volatile void *ptr,
                   void *ret,
                   int memorder) {
    const uintptr_t lock = address_to_mutex(ptr);

    (void)memorder;

    mutex_lock_scoped(&locks[lock]);
    memcpy(ret, (const void *)ptr, size);
}

void __atomic_store(size_t size,
                    volatile void *ptr,
                    void *val,
                    int memorder) {
    const uintptr_t lock = address_to_mutex(ptr);

    (void)memorder;

    mutex_lock_scoped(&locks[lock]);
    memcpy((void *)ptr, val, size);
}

void __atomic_exchange(size_t size,
                       volatile void *ptr,
                       void *val,
                       void *ret,
                       int memorder) {
    const uintptr_t lock = address_to_mutex(ptr);

    (void)memorder;

    mutex_lock_scoped(&locks[lock]);
    memcpy(ret, (const void *)ptr, size);
    memcpy((void *)ptr, val, size);
}

bool __atomic_compare_exchange(size_t size,
                               volatile void* ptr,
                               void* expected,
                               void* desired,
                               int success_memorder,
                               int fail_memorder) {
    const uintptr_t lock = address_to_mutex(ptr);

    (void)success_memorder;
    (void)fail_memorder;

    mutex_lock_scoped(&locks[lock]);

    if(memcmp((const void *)ptr, expected, size) == 0) {
        memcpy((void *)ptr, desired, size);
        return true;
    }
    else {
        memcpy(expected, (const void *)ptr, size);
        return false;
    }
}

/* atomic_flag support: targets without an inline test-and-set sequence
   call these instead. */
__weak_symbol bool __atomic_test_and_set(volatile void *ptr, int model) {
    (void)model;
    irq_disable_scoped();
    const bool ret = *(volatile uint8_t *)ptr != 0;
    *(volatile uint8_t *)ptr = __GCC_ATOMIC_TEST_AND_SET_TRUEVAL;
    return ret;
}

__weak_symbol void __atomic_clear(volatile void *ptr, int model) {
    (void)model;
    irq_disable_scoped();
    *(volatile uint8_t *)ptr = 0;
}

/* All atomics for builtin types are lock-free, while our
   generic atomics back-end utilizes mutexes. */
bool __atomic_is_lock_free(size_t size, const volatile void *ptr) {
    (void)ptr;
    (void)size;
    return (size <= sizeof(long long)) ? true : false;
}
