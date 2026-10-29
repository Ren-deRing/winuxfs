/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Wraps libxfs/xfs_cksum.h (xfs_platform.h includes this instead). Normal builds are the vendored header unchanged.
 * WFS_XFS_FUZZ_SKIP_CRC (fuzz builds only, design 10.6) makes every checksum
 * verification succeed, so mutations get past the CRC and exercise the
 * structural verifiers and the logic behind them. Never defined in the plugin.
 */
#ifndef WFS_SHIM_CKSUM_H
#define WFS_SHIM_CKSUM_H

#ifdef WFS_XFS_FUZZ_SKIP_CRC
#define xfs_verify_cksum wfs_fuzz_real_verify_cksum
#include <xfs_cksum.h>
#undef xfs_verify_cksum
static inline int
xfs_verify_cksum(char *buffer, size_t length, unsigned long cksum_offset)
{
	(void)buffer; (void)length; (void)cksum_offset;
	return 1;
}
#else
#include <xfs_cksum.h>
#endif

#endif
