/* SPDX-License-Identifier: GPL-2.0-only */
/* fs/xfs/xfs_super.h subset: what libxfs and the log call (mount.c defines them). */
#ifndef __XFS_SUPER_H__
#define __XFS_SUPER_H__

struct xfs_mount;

void xfs_flush_inodes(struct xfs_mount *mp);
xfs_agnumber_t xfs_set_inode_alloc(struct xfs_mount *mp, xfs_agnumber_t agcount);
void xfs_reinit_percpu_counters(struct xfs_mount *mp);
/* created by kinit.c (the kernel: init_xfs_fs); discards never queue work here */
extern struct workqueue_struct *xfs_discard_wq;

#define XFS_M(sb)		((struct xfs_mount *)((sb)->s_fs_info))

#endif
