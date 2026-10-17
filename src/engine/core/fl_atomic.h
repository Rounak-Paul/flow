#pragma once

#include <stdbool.h>
#include <stdint.h>

/*
 * Sequentially consistent atomics on naturally aligned 32/64-bit integers and
 * pointers. Every operation is lock-free and async-signal-safe.
 */

#if defined(_MSC_VER) && !defined(__clang__)

#include <intrin.h>

/* Atomically read *p. */
static inline uint32_t fl_atomic_load_u32(const volatile uint32_t *p)
{
    return (uint32_t)_InterlockedOr((volatile long *)p, 0);
}

/* Atomically write v to *p. */
static inline void fl_atomic_store_u32(volatile uint32_t *p, uint32_t v)
{
    _InterlockedExchange((volatile long *)p, (long)v);
}

/* Atomically add v to *p and return the previous value. */
static inline uint32_t fl_atomic_fetch_add_u32(volatile uint32_t *p, uint32_t v)
{
    return (uint32_t)_InterlockedExchangeAdd((volatile long *)p, (long)v);
}

/* Atomically replace *p with v and return the previous value. */
static inline uint32_t fl_atomic_exchange_u32(volatile uint32_t *p, uint32_t v)
{
    return (uint32_t)_InterlockedExchange((volatile long *)p, (long)v);
}

/* Replace *p with desired when it equals *expected; otherwise store the observed value in *expected. */
static inline bool fl_atomic_cas_u32(volatile uint32_t *p, uint32_t *expected, uint32_t desired)
{
    uint32_t observed = (uint32_t)_InterlockedCompareExchange((volatile long *)p, (long)desired,
                                                              (long)*expected);
    if (observed == *expected) return true;
    *expected = observed;
    return false;
}

/* Atomically read *p. */
static inline uint64_t fl_atomic_load_u64(const volatile uint64_t *p)
{
    return (uint64_t)_InterlockedOr64((volatile long long *)p, 0);
}

/* Atomically write v to *p. */
static inline void fl_atomic_store_u64(volatile uint64_t *p, uint64_t v)
{
    _InterlockedExchange64((volatile long long *)p, (long long)v);
}

/* Atomically add v to *p and return the previous value. */
static inline uint64_t fl_atomic_fetch_add_u64(volatile uint64_t *p, uint64_t v)
{
    return (uint64_t)_InterlockedExchangeAdd64((volatile long long *)p, (long long)v);
}

#else

/* Atomically read *p. */
static inline uint32_t fl_atomic_load_u32(const volatile uint32_t *p)
{
    return __atomic_load_n(p, __ATOMIC_SEQ_CST);
}

/* Atomically write v to *p. */
static inline void fl_atomic_store_u32(volatile uint32_t *p, uint32_t v)
{
    __atomic_store_n(p, v, __ATOMIC_SEQ_CST);
}

/* Atomically add v to *p and return the previous value. */
static inline uint32_t fl_atomic_fetch_add_u32(volatile uint32_t *p, uint32_t v)
{
    return __atomic_fetch_add(p, v, __ATOMIC_SEQ_CST);
}

/* Atomically replace *p with v and return the previous value. */
static inline uint32_t fl_atomic_exchange_u32(volatile uint32_t *p, uint32_t v)
{
    return __atomic_exchange_n(p, v, __ATOMIC_SEQ_CST);
}

/* Replace *p with desired when it equals *expected; otherwise store the observed value in *expected. */
static inline bool fl_atomic_cas_u32(volatile uint32_t *p, uint32_t *expected, uint32_t desired)
{
    return __atomic_compare_exchange_n(p, expected, desired, false, __ATOMIC_SEQ_CST,
                                       __ATOMIC_SEQ_CST);
}

/* Atomically read *p. */
static inline uint64_t fl_atomic_load_u64(const volatile uint64_t *p)
{
    return __atomic_load_n(p, __ATOMIC_SEQ_CST);
}

/* Atomically write v to *p. */
static inline void fl_atomic_store_u64(volatile uint64_t *p, uint64_t v)
{
    __atomic_store_n(p, v, __ATOMIC_SEQ_CST);
}

/* Atomically add v to *p and return the previous value. */
static inline uint64_t fl_atomic_fetch_add_u64(volatile uint64_t *p, uint64_t v)
{
    return __atomic_fetch_add(p, v, __ATOMIC_SEQ_CST);
}

#endif
