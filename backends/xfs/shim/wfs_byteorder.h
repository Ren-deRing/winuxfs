/* SPDX-License-Identifier: GPL-2.0-only */
/* Endian conversion and unaligned access, on __BYTE_ORDER__ (no x86 assumption). */
#ifndef WFS_SHIM_BYTEORDER_H
#define WFS_SHIM_BYTEORDER_H

#include "wfs_types.h"

#if !defined(__BYTE_ORDER__) || !defined(__ORDER_LITTLE_ENDIAN__)
# error "compiler does not define __BYTE_ORDER__"
#endif
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
# define WFS_LITTLE_ENDIAN 1
# define __LITTLE_ENDIAN 1234
#else
# define __BIG_ENDIAN 4321
#endif

#define __swab16(x) __builtin_bswap16(x)
#define __swab32(x) __builtin_bswap32(x)
#define __swab64(x) __builtin_bswap64(x)

#ifdef WFS_LITTLE_ENDIAN
# define cpu_to_le16(x) ((__le16)(u16)(x))
# define cpu_to_le32(x) ((__le32)(u32)(x))
# define cpu_to_le64(x) ((__le64)(u64)(x))
# define le16_to_cpu(x) ((u16)(__le16)(x))
# define le32_to_cpu(x) ((u32)(__le32)(x))
# define le64_to_cpu(x) ((u64)(__le64)(x))
# define cpu_to_be16(x) ((__be16)__swab16((u16)(x)))
# define cpu_to_be32(x) ((__be32)__swab32((u32)(x)))
# define cpu_to_be64(x) ((__be64)__swab64((u64)(x)))
# define be16_to_cpu(x) ((u16)__swab16((__be16)(x)))
# define be32_to_cpu(x) ((u32)__swab32((__be32)(x)))
# define be64_to_cpu(x) ((u64)__swab64((__be64)(x)))
#else
# define cpu_to_be16(x) ((__be16)(u16)(x))
# define cpu_to_be32(x) ((__be32)(u32)(x))
# define cpu_to_be64(x) ((__be64)(u64)(x))
# define be16_to_cpu(x) ((u16)(__be16)(x))
# define be32_to_cpu(x) ((u32)(__be32)(x))
# define be64_to_cpu(x) ((u64)(__be64)(x))
# define cpu_to_le16(x) ((__le16)__swab16((u16)(x)))
# define cpu_to_le32(x) ((__le32)__swab32((u32)(x)))
# define cpu_to_le64(x) ((__le64)__swab64((u64)(x)))
# define le16_to_cpu(x) ((u16)__swab16((__le16)(x)))
# define le32_to_cpu(x) ((u32)__swab32((__le32)(x)))
# define le64_to_cpu(x) ((u64)__swab64((__le64)(x)))
#endif

static inline __be32 be32_add_cpu_(__be32 v, u32 d) { return cpu_to_be32(be32_to_cpu(v) + d); }
static inline void be16_add_cpu(__be16 *p, s16 d) { *p = cpu_to_be16((u16)(be16_to_cpu(*p) + d)); }
static inline void be32_add_cpu(__be32 *p, s32 d) { *p = be32_add_cpu_(*p, (u32)d); }
static inline void be64_add_cpu(__be64 *p, s64 d) { *p = cpu_to_be64(be64_to_cpu(*p) + (u64)d); }
static inline void le32_add_cpu(__le32 *p, u32 d) { *p = cpu_to_le32(le32_to_cpu(*p) + d); }

/* Unaligned access via memcpy (strict-aliasing and alignment safe). */
#define __wfs_get_un(T, p) ({ T _v; memcpy(&_v, (p), sizeof(_v)); _v; })
#define __wfs_put_un(T, v, p) do { T _v = (v); memcpy((p), &_v, sizeof(_v)); } while (0)
#define get_unaligned(p) ({ __typeof__(*(p)) _v; memcpy(&_v, (p), sizeof(_v)); _v; })
#define put_unaligned(v, p) do { __typeof__(*(p)) _v = (v); memcpy((p), &_v, sizeof(_v)); } while (0)
#define get_unaligned_be16(p) be16_to_cpu(__wfs_get_un(u16, p))
#define get_unaligned_be32(p) be32_to_cpu(__wfs_get_un(u32, p))
#define get_unaligned_be64(p) be64_to_cpu(__wfs_get_un(u64, p))
#define get_unaligned_le16(p) le16_to_cpu(__wfs_get_un(u16, p))
#define get_unaligned_le32(p) le32_to_cpu(__wfs_get_un(u32, p))
#define get_unaligned_le64(p) le64_to_cpu(__wfs_get_un(u64, p))
#define put_unaligned_be16(v, p) __wfs_put_un(u16, cpu_to_be16(v), p)
#define put_unaligned_be32(v, p) __wfs_put_un(u32, cpu_to_be32(v), p)
#define put_unaligned_be64(v, p) __wfs_put_un(u64, cpu_to_be64(v), p)
#define put_unaligned_le16(v, p) __wfs_put_un(u16, cpu_to_le16(v), p)
#define put_unaligned_le32(v, p) __wfs_put_un(u32, cpu_to_le32(v), p)
#define put_unaligned_le64(v, p) __wfs_put_un(u64, cpu_to_le64(v), p)

#endif /* WFS_SHIM_BYTEORDER_H */
