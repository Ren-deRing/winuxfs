/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef XFS_DRAIN_H_
#define XFS_DRAIN_H_
/* Intent drains exist for scrub only: an empty member, no-op calls. */
struct xfs_defer_drain { int unused; };
struct xfs_group;
#define xfs_defer_drain_busy(dr)		(false)
#define xfs_defer_drain_wait(dr)		(0)
#define xfs_defer_drain_init(dr)		((void)0)
#define xfs_defer_drain_free(dr)		((void)0)
#define xfs_defer_drain_bump(dr)		((void)0)
#define xfs_defer_drain_drop(dr)		((void)0)
#endif
