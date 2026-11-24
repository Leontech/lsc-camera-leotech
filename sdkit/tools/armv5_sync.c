/* __sync_* builtins for ARMv5 (ARM926EJ-S has no ldrex/strex) using the Linux
 * kernel user helper __kuser_cmpxchg (0xffff0fc0) and __kuser_memory_barrier. */
#include <stdint.h>
typedef int (kuser_cmpxchg_t)(int oldval, int newval, volatile int *ptr);
#define kuser_cmpxchg (*(kuser_cmpxchg_t *)0xffff0fc0)
typedef void (kuser_mb_t)(void);
#define kuser_mb (*(kuser_mb_t *)0xffff0fa0)

void leo_sync_synchronize(void) __asm__("__sync_synchronize");
void leo_sync_synchronize(void) { kuser_mb(); }

static unsigned cas_word(volatile unsigned *p, unsigned oldv, unsigned newv)
{
    for (;;) {
        unsigned cur = *p;
        if (cur != oldv) return cur;
        if (kuser_cmpxchg((int)oldv, (int)newv, (volatile int *)p) == 0) return oldv;
    }
}

/* sub-word CAS on the containing aligned word (little endian) */
static unsigned cas_sub(volatile void *ptr, unsigned oldv, unsigned newv, int size)
{
    uintptr_t a = (uintptr_t)ptr;
    volatile unsigned *w = (volatile unsigned *)(a & ~3u);
    int shift = (int)(a & 3u) * 8;
    unsigned mask = (size == 1 ? 0xffu : 0xffffu) << shift;
    for (;;) {
        unsigned cur = *w;
        unsigned curv = (cur & mask) >> shift;
        if (curv != (oldv & (mask >> shift))) return curv;
        unsigned nw = (cur & ~mask) | ((newv << shift) & mask);
        if (kuser_cmpxchg((int)cur, (int)nw, (volatile int *)w) == 0) return curv;
    }
}

#define CAS4(p, o, n) cas_word((volatile unsigned *)(p), (unsigned)(o), (unsigned)(n))
#define CASN(p, o, n, s) ((s) == 4 ? CAS4(p, o, n) : cas_sub(p, (unsigned)(o), (unsigned)(n), s))

#define FN(R, name, args) R leo_##name args __asm__("__" #name); R leo_##name args
#define DEF(T, N) \
FN(T, sync_val_compare_and_swap_##N, (volatile T *p, T o, T n)) { return (T)CASN(p, o, n, N); } \
FN(_Bool, sync_bool_compare_and_swap_##N, (volatile T *p, T o, T n)) { return (T)CASN(p, o, n, N) == o; } \
FN(T, sync_lock_test_and_set_##N, (volatile T *p, T v)) { T old; do { old = *p; } while ((T)CASN(p, old, v, N) != old); return old; } \
FN(void, sync_lock_release_##N, (volatile T *p)) { kuser_mb(); *p = 0; } \
OP(T, N, add, +) OP(T, N, sub, -) OP(T, N, and, &) OP(T, N, or, |) OP(T, N, xor, ^) \
FN(T, sync_fetch_and_nand_##N, (volatile T *p, T v)) { T old; do { old = *p; } while ((T)CASN(p, old, (T)~(old & v), N) != old); return old; } \
FN(T, sync_nand_and_fetch_##N, (volatile T *p, T v)) { T old, nw; do { old = *p; nw = (T)~(old & v); } while ((T)CASN(p, old, nw, N) != old); return nw; }

#define OP(T, N, name, op) \
FN(T, sync_fetch_and_##name##_##N, (volatile T *p, T v)) { T old; do { old = *p; } while ((T)CASN(p, old, (T)(old op v), N) != old); return old; } \
FN(T, sync_##name##_and_fetch_##N, (volatile T *p, T v)) { T old, nw; do { old = *p; nw = (T)(old op v); } while ((T)CASN(p, old, nw, N) != old); return nw; }

DEF(uint8_t, 1)
DEF(uint16_t, 2)
DEF(uint32_t, 4)

#define MM(T, ST, N) \
FN(T, sync_fetch_and_umax_##N, (volatile T *p, T v)) { T old; do { old = *p; } while ((T)CASN(p, old, old > v ? old : v, N) != old); return old; } \
FN(T, sync_fetch_and_umin_##N, (volatile T *p, T v)) { T old; do { old = *p; } while ((T)CASN(p, old, old < v ? old : v, N) != old); return old; } \
FN(T, sync_fetch_and_max_##N, (volatile T *p, T v)) { T old; do { old = *p; } while ((T)CASN(p, old, (ST)old > (ST)v ? old : v, N) != old); return old; } \
FN(T, sync_fetch_and_min_##N, (volatile T *p, T v)) { T old; do { old = *p; } while ((T)CASN(p, old, (ST)old < (ST)v ? old : v, N) != old); return old; }
MM(uint8_t, int8_t, 1)
MM(uint16_t, int16_t, 2)
MM(uint32_t, int32_t, 4)
