/* SPDX-License-Identifier: GPL-2.0-only */
/* fs/xfs/xfs_iops.h subset: VFS inode setup without a VFS (icache.c). */
#ifndef __XFS_IOPS_H__
#define __XFS_IOPS_H__

struct xfs_inode;
struct inode;
struct qstr;

void xfs_setup_inode(struct xfs_inode *ip);
static inline void xfs_setup_iops(struct xfs_inode *ip) { (void)ip; }
/* No security modules: nothing to label. */
static inline int xfs_inode_init_security(struct inode *inode, struct inode *dir,
					  const struct qstr *qstr)
{
	(void)inode; (void)dir; (void)qstr;
	return 0;
}
#endif
