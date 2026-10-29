// SPDX-License-Identifier: GPL-2.0-only
/*
 * statfs (M2 design 8.7): the numbers of the kernel's xfs_fs_statfs for a
 * read-only mount, from the in-core superblock counters that mount.c fixed
 * (trusted when the log is clean and they are sane, else the AGF/AGI sums;
 * m_allocbt_blks from the AGFs). The mount is read-only, so the counters
 * never change and there is no reserved block pool to subtract.
 *
 *   f_bsize   sb_blocksize                       -> block_size
 *   f_namelen MAXNAMELEN - 1 = 255               -> name_max
 *   f_blocks  sb_dblocks - (sb_logstart ? sb_logblocks : 0)   (internal log)
 *   f_bfree   fdblocks - (m_alloc_set_aside + m_allocbt_blks)
 *   f_bavail  = f_bfree (XFS has no privileged reserve)
 *   f_files   min(icount + (f_bfree << inopblog), 2^56 - 1, maxicount),
 *             at least icount; maxicount only when imax_pct != 0
 *   f_ffree   f_files - (icount - ifree)
 *
 * Differences from the kernel, all in the safe direction: every subtraction
 * saturates at 0 (the kernel wraps or relies on its counters being sane),
 * free space never exceeds total, bytes saturate at UINT64_MAX instead of
 * overflowing. Not reported: the kernel's realtime view for a directory with
 * the rtinherit flag (statfs of an rtinherit directory on an RT volume shows
 * the RT section there); this reports the data section only, as for any other
 * directory.
 */
#include "wfs_xfs.h"
#include "ops.h"

#include <stdint.h>

static uint64_t sat_mul(uint64_t a, uint64_t b)
{
	return b && a > UINT64_MAX / b ? UINT64_MAX : a * b;
}

wfs_err_t wfs_xfs_op_statfs(struct wfs_xfs *x, wfs_statfs *st)
{
	struct xfs_mount *mp;
	const struct xfs_sb *sb;
	uint64_t icount, ifree, fdblocks, total, bfree, unavail, files, used, maxino, bs;
	int64_t allocbt;

	if (!x || !st)
		return -WFS_EINVAL;
	mp = &x->mp;
	sb = &mp->m_sb;
	bs = sb->sb_blocksize;
	icount = sb->sb_icount;
	ifree = sb->sb_ifree;
	fdblocks = sb->sb_fdblocks;

	/* internal log blocks are inside dblocks; an external log has logstart 0 */
	total = sb->sb_dblocks;
	if (sb->sb_logstart)
		total = total > sb->sb_logblocks ? total - sb->sb_logblocks : 0;

	allocbt = atomic64_read(&mp->m_allocbt_blks);
	unavail = (uint64_t)mp->m_alloc_set_aside + (uint64_t)(allocbt > 0 ? allocbt : 0);
	bfree = fdblocks > unavail ? fdblocks - unavail : 0;
	if (bfree > total)
		bfree = total;

	/* XFS_FSB_TO_INO(bfree), saturating, then the 56 bit inode number cap */
	files = bfree > (XFS_MAXINUMBER >> sb->sb_inopblog) ? XFS_MAXINUMBER :
		bfree << sb->sb_inopblog;
	files = icount > XFS_MAXINUMBER - files ? XFS_MAXINUMBER : icount + files;
	maxino = M_IGEO(mp)->maxicount;
	if (maxino && files > maxino)
		files = maxino;
	if (files < icount)		/* sb_icount overshot maxicount: report what exists */
		files = icount;
	used = icount > ifree ? icount - ifree : 0;

	st->total_bytes = sat_mul(total, bs);
	st->free_bytes = sat_mul(bfree, bs);
	st->avail_bytes = st->free_bytes;
	st->total_inodes = files;
	st->free_inodes = files > used ? files - used : 0;
	st->block_size = sb->sb_blocksize;
	st->name_max = 255;
	return 0;
}
