/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Bit operations and 64-bit math.
 *
 * Bitmaps are arrays of `unsigned long`, exactly as in the kernel, with
 * BITS_PER_LONG = sizeof(long) * 8 (32 on mingw/LLP64, 64 on LP64). All
 * helpers use that word size consistently, so no caller needs 64-bit words;
 * counting helpers take explicit u32/u64 arguments (ffs/fls/hweight32/64,
 * fls64) and never `unsigned long`. Atomic variants use __atomic builtins.
 */
#ifndef WFS_SHIM_BITOPS_H
#define WFS_SHIM_BITOPS_H

#include "wfs_types.h"

#define BITS_PER_LONG		((int)(sizeof(long) * 8))
#define BITS_PER_BYTE		8
#define BITS_TO_LONGS(n)	DIV_ROUND_UP(n, BITS_PER_LONG)
#define BIT(n)			(1ULL << (n))
#define GENMASK(h, l)		(((~0ULL) << (l)) & (~0ULL >> (63 - (h))))
#define NBBY			8

#undef ffs
#define ffs(x) __builtin_ffs(x)
static inline int ffs64(u64 x) { return __builtin_ffsll((long long)x); }
static inline int fls(u32 x) { return x ? 32 - __builtin_clz(x) : 0; }
static inline int fls64(u64 x) { return x ? 64 - __builtin_clzll(x) : 0; }
static inline unsigned int hweight32(u32 x) { return (unsigned)__builtin_popcount(x); }
static inline unsigned int hweight64(u64 x) { return (unsigned)__builtin_popcountll(x); }
static inline unsigned int hweight8(u8 x) { return (unsigned)__builtin_popcount(x); }
static inline bool is_power_of_2(u64 n) { return n != 0 && (n & (n - 1)) == 0; }
#define ilog2(n) ((int)(fls64((u64)(n)) - 1))
static inline u64 roundup_pow_of_two_(u64 n) { return n <= 1 ? 1 : 1ULL << fls64(n - 1); }
#define roundup_pow_of_two(n)	roundup_pow_of_two_(n)

#define __WFS_BIT_WORD(nr)	((nr) / BITS_PER_LONG)
#define __WFS_BIT_MASK(nr)	(1UL << ((nr) % BITS_PER_LONG))

static inline void set_bit(unsigned int nr, volatile unsigned long *a)
{ __atomic_fetch_or(&a[__WFS_BIT_WORD(nr)], __WFS_BIT_MASK(nr), __ATOMIC_SEQ_CST); }
static inline void clear_bit(unsigned int nr, volatile unsigned long *a)
{ __atomic_fetch_and(&a[__WFS_BIT_WORD(nr)], ~__WFS_BIT_MASK(nr), __ATOMIC_SEQ_CST); }
static inline bool test_and_set_bit(unsigned int nr, volatile unsigned long *a)
{ return __atomic_fetch_or(&a[__WFS_BIT_WORD(nr)], __WFS_BIT_MASK(nr), __ATOMIC_SEQ_CST) & __WFS_BIT_MASK(nr); }
static inline bool test_and_clear_bit(unsigned int nr, volatile unsigned long *a)
{ return __atomic_fetch_and(&a[__WFS_BIT_WORD(nr)], ~__WFS_BIT_MASK(nr), __ATOMIC_SEQ_CST) & __WFS_BIT_MASK(nr); }
static inline bool test_bit(unsigned int nr, const volatile unsigned long *a)
{ return (__atomic_load_n(&a[__WFS_BIT_WORD(nr)], __ATOMIC_RELAXED) & __WFS_BIT_MASK(nr)) != 0; }
static inline void __set_bit(unsigned int nr, unsigned long *a) { a[__WFS_BIT_WORD(nr)] |= __WFS_BIT_MASK(nr); }
static inline void __clear_bit(unsigned int nr, unsigned long *a) { a[__WFS_BIT_WORD(nr)] &= ~__WFS_BIT_MASK(nr); }

static inline unsigned long find_next_bit(const unsigned long *a, unsigned long size, unsigned long off)
{
	for (; off < size; off++)
		if (a[off / BITS_PER_LONG] & (1UL << (off % BITS_PER_LONG)))
			return off;
	return size;
}
static inline unsigned long find_next_zero_bit(const unsigned long *a, unsigned long size, unsigned long off)
{
	for (; off < size; off++)
		if (!(a[off / BITS_PER_LONG] & (1UL << (off % BITS_PER_LONG))))
			return off;
	return size;
}
static inline unsigned long find_first_bit(const unsigned long *a, unsigned long size)
{ return find_next_bit(a, size, 0); }
static inline unsigned long find_first_zero_bit(const unsigned long *a, unsigned long size)
{ return find_next_zero_bit(a, size, 0); }

/* 64-bit division: targets are 64 bit, so plain operators. */
static inline u64 div_u64(u64 a, u32 b) { return a / b; }
static inline s64 div_s64(s64 a, s32 b) { return a / b; }
static inline u64 div64_u64(u64 a, u64 b) { return a / b; }
static inline u64 div64_u64_rem(u64 a, u64 b, u64 *rem) { *rem = a % b; return a / b; }
static inline u64 div_u64_rem(u64 a, u32 b, u32 *rem) { *rem = (u32)(a % b); return a / b; }
static inline u64 mul_u32_u32(u32 a, u32 b) { return (u64)a * b; }
/* do_div(n, base): n /= base, evaluates to the remainder. */
#define do_div(n, base) ({ u32 _rem = (u32)((n) % (u32)(base)); (n) = (n) / (u32)(base); _rem; })
#define div_u64_ceil(a, b) DIV_ROUND_UP_ULL(a, b)

static inline u64 rounddown_64(u64 x, u32 y) { return x / y * y; }
static inline u64 roundup_64(u64 x, u32 y) { return (x + y - 1) / y * y; }
static inline u64 howmany_64(u64 x, u32 y) { return (x + y - 1) / y; }
static inline bool isaligned_64(u64 x, u32 y) { return x % y == 0; }
#define howmany(x, y)	(((x) + ((y) - 1)) / (y))

/* If b is a power of 2 return log2(b), else -1 (64-bit argument: LLP64 safe). */
static inline int8_t log2_if_power2(u64 b) { return is_power_of_2(b) ? (int8_t)ilog2(b) : -1; }
/* If b is a power of 2 return the mask of the lower bits, else zero. */
static inline unsigned long long mask64_if_power2(u64 b) { return is_power_of_2(b) ? b - 1 : 0; }

#endif /* WFS_SHIM_BITOPS_H */
