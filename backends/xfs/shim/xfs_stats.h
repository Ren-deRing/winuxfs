/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef __XFS_STATS_H__
#define __XFS_STATS_H__
/* Statistics are compiled out; the counter names are dropped by the macros. */
struct xstats { int unused; };	/* xfs_mount.m_stats */
#define XFS_STATS_INC(mp, v)		((void)(mp))
#define XFS_STATS_DEC(mp, v)		((void)(mp))
#define XFS_STATS_ADD(mp, v, n)		((void)(mp), (void)(n))
#define XFS_STATS_INC_OFF(mp, off)	((void)(mp))
#define XFS_STATS_ADD_OFF(mp, off, n)	((void)(mp))
#define XFS_STATS_CALC_INDEX(member)	0
#endif
