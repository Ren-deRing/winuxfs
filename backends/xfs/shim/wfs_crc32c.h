/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef WFS_SHIM_CRC32C_H
#define WFS_SHIM_CRC32C_H

#include "wfs_types.h"

/*
 * Software CRC32C (Castagnoli), slice-by-8, no hardware dependency.
 * Same contract as the kernel's crc32c_le()/crc32c(): `crc` is the running
 * value (seed ~0, no final inversion), the result is host-endian.
 * RFC 3720: ~crc32c_le(~0, 32 zero bytes) == 0x8A9136AA.
 */
u32 crc32c_le(u32 crc, const void *p, size_t len);
#define crc32c(crc, p, len)	crc32c_le((crc), (p), (len))

#endif
