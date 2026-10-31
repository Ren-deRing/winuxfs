/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * xfs_iomap.h without <linux/iomap.h>: libxfs only names struct iomap in
 * the (dead) delalloc-conversion path and calls two helpers. Both are
 * stubs at link time (Task 4).
 */
#ifndef __XFS_IOMAP_H__
#define __XFS_IOMAP_H__

struct xfs_inode;
struct xfs_bmbt_irec;
struct xfs_zone_alloc_ctx;
#define IOMAP_F_SHARED		(1U << 2)
#define IOMAP_F_NEW		(1U << 0)
struct iomap { u64 addr; loff_t offset; u64 length; u16 type; u16 flags; };

u64 xfs_iomap_inode_sequence(struct xfs_inode *ip, u16 iomap_flags);
int xfs_bmbt_to_iomap(struct xfs_inode *ip, struct iomap *iomap,
		struct xfs_bmbt_irec *imap, unsigned int mapping_flags,
		u16 iomap_flags, u64 sequence_cookie);
xfs_fileoff_t xfs_iomap_eof_align_last_fsb(struct xfs_inode *ip, xfs_fileoff_t end_fsb);

static inline xfs_filblks_t
xfs_aligned_fsb_count(xfs_fileoff_t offset_fsb, xfs_filblks_t count_fsb, xfs_extlen_t extsz)
{
	if (extsz) {
		xfs_extlen_t align;

		div_u64_rem(offset_fsb, extsz, &align);
		if (align)
			count_fsb += align;
		div_u64_rem(count_fsb, extsz, &align);
		if (align)
			count_fsb += extsz - align;
	}
	return count_fsb;
}
/* Zero a byte range of a file (the data write path, M3a T7). */
int xfs_zero_range(struct xfs_inode *ip, loff_t pos, loff_t len,
		struct xfs_zone_alloc_ctx *ac, bool *did_zero);
#endif
