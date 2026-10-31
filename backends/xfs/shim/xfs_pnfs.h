/* SPDX-License-Identifier: GPL-2.0-only */
/* fs/xfs/xfs_pnfs.h: no pNFS block layouts (the kernel's !CONFIG_EXPORTFS_BLOCK_OPS branch). */
#ifndef _XFS_PNFS_H
#define _XFS_PNFS_H 1
struct inode;
static inline int xfs_break_leased_layouts(struct inode *inode, uint *iolock, bool *did_unlock)
{
	(void)inode; (void)iolock; (void)did_unlock;
	return 0;
}
#endif
