/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef XFS_DRAIN_H_
#define XFS_DRAIN_H_
/*
 * The kernel's !CONFIG_XFS_DRAIN_INTENTS branch: intent drains exist for
 * online scrub, which winuxfs does not run, so xfs_drain.c is not compiled
 * and intent items take plain group references.
 */
struct xfs_defer_drain { int unused; };
struct xfs_group;
#define xfs_defer_drain_busy(dr)		(false)
#define xfs_defer_drain_wait(dr)		(0)
#define xfs_defer_drain_init(dr)		((void)0)
#define xfs_defer_drain_free(dr)		((void)0)
#define xfs_group_intent_get(_mp, _fsbno, _type) \
	xfs_group_get_by_fsb((_mp), (_fsbno), (_type))
#define xfs_group_intent_put(xg)		xfs_group_put(xg)
#endif
