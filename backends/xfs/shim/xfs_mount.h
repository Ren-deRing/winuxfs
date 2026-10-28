/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * xfs_mount for the read-only port: derived from fs/xfs/xfs_mount.h with the
 * fields the read path uses; work queues, percpu/rcu/shrinker, kobjects,
 * log, quota, inodegc and sysfs members are removed or reduced to plain
 * types (design 3.3). Field names are unchanged so libxfs compiles as is.
 * Fields marked (T6) are filled by the mount code of Task 6.
 */
#ifndef __XFS_MOUNT_H__
#define	__XFS_MOUNT_H__

struct xlog;
struct xfs_inode;
struct xfs_mru_cache;
struct xfs_ail;
struct xfs_quotainfo;
struct xfs_da_geometry;
struct xfs_perag;
struct xfs_healthmon;
struct xfs_zone_info;

enum {
	XFS_LOWSP_1_PCNT = 0,
	XFS_LOWSP_2_PCNT,
	XFS_LOWSP_3_PCNT,
	XFS_LOWSP_4_PCNT,
	XFS_LOWSP_5_PCNT,
	XFS_LOWSP_MAX,
};

/* Per-group (AG/RT group) geometry. Groups are a fixed array after mount
 * (design 3.3: xa_* replaced by array indexing); xa holds that array. */
struct xfs_groups {
	struct xarray		xa;
	uint32_t		blocks;
	uint8_t			blklog;
	bool			has_daddr_gaps;
	uint64_t		blkmask;
	xfs_fsblock_t		start_fsb;
	xfs_extlen_t		awu_max;
	/* winuxfs: fixed table of struct xfs_group *, `nr` entries */
	void			**tab;
	uint32_t		nr;
};

struct xfs_freecounter {
	struct percpu_counter	count;
	uint64_t		res_total;
	uint64_t		res_avail;
	uint64_t		res_saved;
};

typedef struct xfs_mount {
	struct xfs_sb		m_sb;		/* copy of fs superblock */
	struct xfs_buf		*m_sb_bp;
	struct xfs_buf		*m_rtsb_bp;
	char			*m_rtname;
	char			*m_logname;
	struct xfs_da_geometry	*m_dir_geo;
	struct xfs_da_geometry	*m_attr_geo;
	struct xlog		*m_log;		/* NULL: log is never used */
	struct xfs_inode	*m_rootip;
	struct xfs_inode	*m_metadirip;
	struct xfs_inode	*m_rtdirip;
	struct xfs_quotainfo	*m_quotainfo;	/* NULL */
	struct xfs_buftarg	*m_ddev_targp;	/* data device */
	struct xfs_buftarg	*m_logdev_targp;
	struct xfs_buftarg	*m_rtdev_targp;	/* rt device, NULL if none */

	int			m_bsize;	/* fs logical block size */
	uint8_t			m_blkbit_log;
	uint8_t			m_blkbb_log;
	uint8_t			m_agno_log;
	uint8_t			m_sectbb_log;
	int8_t			m_rtxblklog;

	uint			m_blockmask;
	uint			m_blockwsize;
	unsigned int		m_rtx_per_rbmblock;
	uint			m_alloc_mxr[2];
	uint			m_alloc_mnr[2];
	uint			m_bmap_dmxr[2];
	uint			m_bmap_dmnr[2];
	uint			m_rmap_mxr[2];
	uint			m_rmap_mnr[2];
	uint			m_rtrmap_mxr[2];
	uint			m_rtrmap_mnr[2];
	uint			m_refc_mxr[2];
	uint			m_refc_mnr[2];
	uint			m_rtrefc_mxr[2];
	uint			m_rtrefc_mnr[2];
	uint			m_alloc_maxlevels;
	uint			m_bm_maxlevels[2];
	uint			m_rmap_maxlevels;
	uint			m_rtrmap_maxlevels;
	uint			m_refc_maxlevels;
	uint			m_rtrefc_maxlevels;
	unsigned int		m_agbtree_maxlevels;
	unsigned int		m_rtbtree_maxlevels;
	xfs_extlen_t		m_ag_prealloc_blocks;
	uint			m_alloc_set_aside;
	uint			m_ag_max_usable;
	int			m_dalign;	/* stripe unit */
	int			m_swidth;	/* stripe width */
	xfs_agnumber_t		m_maxagi;
	uint			m_allocsize_log;
	uint			m_allocsize_blocks;
	int			m_logbufs;
	int			m_logbsize;
	unsigned int		m_rsumlevels;
	xfs_filblks_t		m_rsumblocks;
	int			m_fixedfsid[2];
	uint			m_qflags;
	uint64_t		m_features;	/* active filesystem features */
	uint64_t		m_low_space[XFS_LOWSP_MAX];
	uint64_t		m_low_rtexts[XFS_LOWSP_MAX];
	uint64_t		m_rtxblkmask;
	struct xfs_ino_geometry	m_ino_geo;	/* inode geometry */
	struct xfs_trans_resv	m_resv;
	unsigned long		m_opstate;	/* dynamic state flags (bits < 32) */
	bool			m_always_cow;
	bool			m_fail_unmount;
	bool			m_finobt_nores;
	bool			m_update_sb;
	unsigned int		m_max_open_zones;
	unsigned int		m_zonegc_low_space;
	unsigned long long	m_awu_max_bytes;

	uint8_t			m_fs_checked;
	uint8_t			m_fs_sick;
	uint8_t			m_rt_checked;
	uint8_t			m_rt_sick;

	spinlock_t		m_sb_lock;
	struct percpu_counter	m_icount;
	struct percpu_counter	m_ifree;
	struct xfs_freecounter	m_free[XC_FREE_NR];
	struct percpu_counter	m_delalloc_blks;
	struct percpu_counter	m_delalloc_rtextents;
	atomic64_t		m_allocbt_blks;

	struct xfs_groups	m_groups[XG_TYPE_MAX];
	struct xfs_zone_info	*m_zone_info;
	xfs_agnumber_t		m_agfrotor;
	atomic_t		m_agirotor;
	atomic_t		m_rtgrotor;

	struct mutex		m_metafile_resv_lock;
	uint64_t		m_metafile_resv_target;
	uint64_t		m_metafile_resv_used;
	uint64_t		m_metafile_resv_avail;

	uint32_t		m_generation;
	struct mutex		m_growlock;

	struct xfs_hooks	m_dir_update_hooks;
	unsigned int		m_uuid_table_index;

	/* winuxfs: back pointer to the backend instance (T6). */
	void			*m_wfs;
} xfs_mount_t;

#define M_IGEO(mp)		(&(mp)->m_ino_geo)

#define XFS_FEAT_ATTR		(1ULL << 0)
#define XFS_FEAT_NLINK		(1ULL << 1)
#define XFS_FEAT_QUOTA		(1ULL << 2)
#define XFS_FEAT_ALIGN		(1ULL << 3)
#define XFS_FEAT_DALIGN		(1ULL << 4)
#define XFS_FEAT_LOGV2		(1ULL << 5)
#define XFS_FEAT_SECTOR		(1ULL << 6)
#define XFS_FEAT_EXTFLG		(1ULL << 7)
#define XFS_FEAT_ASCIICI	(1ULL << 8)
#define XFS_FEAT_LAZYSBCOUNT	(1ULL << 9)
#define XFS_FEAT_PARENT		(1ULL << 11)
#define XFS_FEAT_PROJID32	(1ULL << 12)
#define XFS_FEAT_CRC		(1ULL << 13)
#define XFS_FEAT_V3INODES	(1ULL << 14)
#define XFS_FEAT_PQUOTINO	(1ULL << 15)
#define XFS_FEAT_FTYPE		(1ULL << 16)
#define XFS_FEAT_FINOBT		(1ULL << 17)
#define XFS_FEAT_RMAPBT		(1ULL << 18)
#define XFS_FEAT_REFLINK	(1ULL << 19)
#define XFS_FEAT_SPINODES	(1ULL << 20)
#define XFS_FEAT_META_UUID	(1ULL << 21)
#define XFS_FEAT_REALTIME	(1ULL << 22)
#define XFS_FEAT_INOBTCNT	(1ULL << 23)
#define XFS_FEAT_BIGTIME	(1ULL << 24)
#define XFS_FEAT_NEEDSREPAIR	(1ULL << 25)
#define XFS_FEAT_NREXT64	(1ULL << 26)
#define XFS_FEAT_EXCHANGE_RANGE	(1ULL << 27)
#define XFS_FEAT_METADIR	(1ULL << 28)
#define XFS_FEAT_ZONED		(1ULL << 29)
#define XFS_FEAT_NOLIFETIME	(1ULL << 47)
#define XFS_FEAT_NOALIGN	(1ULL << 49)
#define XFS_FEAT_ALLOCSIZE	(1ULL << 50)
#define XFS_FEAT_LARGE_IOSIZE	(1ULL << 51)
#define XFS_FEAT_WSYNC		(1ULL << 52)
#define XFS_FEAT_DIRSYNC	(1ULL << 53)
#define XFS_FEAT_DISCARD	(1ULL << 54)
#define XFS_FEAT_GRPID		(1ULL << 55)
#define XFS_FEAT_SMALL_INUMS	(1ULL << 56)
#define XFS_FEAT_SWALLOC	(1ULL << 58)
#define XFS_FEAT_FILESTREAMS	(1ULL << 59)
#define XFS_FEAT_DAX_ALWAYS	(1ULL << 60)
#define XFS_FEAT_DAX_NEVER	(1ULL << 61)
#define XFS_FEAT_NORECOVERY	(1ULL << 62)
#define XFS_FEAT_NOUUID		(1ULL << 63)

#define __XFS_HAS_FEAT(name, NAME) \
static inline bool xfs_has_ ## name (const struct xfs_mount *mp) \
{ \
	return mp->m_features & XFS_FEAT_ ## NAME; \
}

/* Adding a feature also updates the in-core sb (needed by mount code). */
#define __XFS_ADD_FEAT(name, NAME) \
	__XFS_HAS_FEAT(name, NAME); \
static inline void xfs_add_ ## name (struct xfs_mount *mp) \
{ \
	mp->m_features |= XFS_FEAT_ ## NAME; \
	xfs_sb_version_add ## name(&mp->m_sb); \
}

__XFS_ADD_FEAT(attr, ATTR)
__XFS_HAS_FEAT(nlink, NLINK)
__XFS_ADD_FEAT(quota, QUOTA)
__XFS_HAS_FEAT(dalign, DALIGN)
__XFS_HAS_FEAT(sector, SECTOR)
__XFS_HAS_FEAT(asciici, ASCIICI)
__XFS_HAS_FEAT(parent, PARENT)
__XFS_HAS_FEAT(ftype, FTYPE)
__XFS_HAS_FEAT(finobt, FINOBT)
__XFS_HAS_FEAT(rmapbt, RMAPBT)
__XFS_HAS_FEAT(reflink, REFLINK)
__XFS_HAS_FEAT(sparseinodes, SPINODES)
__XFS_HAS_FEAT(metauuid, META_UUID)
__XFS_HAS_FEAT(realtime, REALTIME)
__XFS_HAS_FEAT(inobtcounts, INOBTCNT)
__XFS_HAS_FEAT(bigtime, BIGTIME)
__XFS_HAS_FEAT(needsrepair, NEEDSREPAIR)
__XFS_HAS_FEAT(large_extent_counts, NREXT64)
__XFS_HAS_FEAT(exchange_range, EXCHANGE_RANGE)
__XFS_HAS_FEAT(metadir, METADIR)
__XFS_HAS_FEAT(zoned, ZONED)
__XFS_HAS_FEAT(nolifetime, NOLIFETIME)

static inline bool xfs_has_rtgroups(const struct xfs_mount *mp) { return xfs_has_metadir(mp); }
static inline bool xfs_has_rtsb(const struct xfs_mount *mp)
{ return xfs_has_rtgroups(mp) && xfs_has_realtime(mp) && !xfs_has_zoned(mp); }
static inline bool xfs_has_rtrmapbt(const struct xfs_mount *mp)
{ return xfs_has_rtgroups(mp) && xfs_has_realtime(mp) && xfs_has_rmapbt(mp); }
static inline bool xfs_has_rtreflink(const struct xfs_mount *mp)
{ return xfs_has_metadir(mp) && xfs_has_realtime(mp) && xfs_has_reflink(mp); }
static inline bool xfs_has_nonzoned(const struct xfs_mount *mp) { return !xfs_has_zoned(mp); }
static inline bool xfs_can_sw_atomic_write(struct xfs_mount *mp) { return xfs_has_reflink(mp); }

/* v4 filesystems are rejected at mount: these features are always on (v5). */
#define __XFS_HAS_V4_FEAT(name, NAME) \
static inline bool xfs_has_ ## name (struct xfs_mount *mp) \
{ \
	return !IS_ENABLED(CONFIG_XFS_SUPPORT_V4) || \
		(mp->m_features & XFS_FEAT_ ## NAME); \
}
#define __XFS_ADD_V4_FEAT(name, NAME) \
	__XFS_HAS_V4_FEAT(name, NAME); \
static inline void xfs_add_ ## name (struct xfs_mount *mp) \
{ \
	if (IS_ENABLED(CONFIG_XFS_SUPPORT_V4)) { \
		mp->m_features |= XFS_FEAT_ ## NAME; \
		xfs_sb_version_add ## name(&mp->m_sb); \
	} \
}

__XFS_HAS_V4_FEAT(align, ALIGN)
__XFS_HAS_V4_FEAT(logv2, LOGV2)
__XFS_HAS_V4_FEAT(extflg, EXTFLG)
__XFS_HAS_V4_FEAT(lazysbcount, LAZYSBCOUNT)
__XFS_ADD_V4_FEAT(projid32, PROJID32)
__XFS_HAS_V4_FEAT(v3inodes, V3INODES)
__XFS_HAS_V4_FEAT(crc, CRC)
__XFS_HAS_V4_FEAT(pquotino, PQUOTINO)

static inline void xfs_add_attr2(struct xfs_mount *mp)
{
	if (IS_ENABLED(CONFIG_XFS_SUPPORT_V4))
		xfs_sb_version_addattr2(&mp->m_sb);
}

__XFS_HAS_FEAT(noalign, NOALIGN)
__XFS_HAS_FEAT(allocsize, ALLOCSIZE)
__XFS_HAS_FEAT(large_iosize, LARGE_IOSIZE)
__XFS_HAS_FEAT(wsync, WSYNC)
__XFS_HAS_FEAT(dirsync, DIRSYNC)
__XFS_HAS_FEAT(discard, DISCARD)
__XFS_HAS_FEAT(grpid, GRPID)
__XFS_HAS_FEAT(small_inums, SMALL_INUMS)
__XFS_HAS_FEAT(swalloc, SWALLOC)
__XFS_HAS_FEAT(filestreams, FILESTREAMS)
__XFS_HAS_FEAT(dax_always, DAX_ALWAYS)
__XFS_HAS_FEAT(dax_never, DAX_NEVER)
__XFS_HAS_FEAT(norecovery, NORECOVERY)
__XFS_HAS_FEAT(nouuid, NOUUID)

#define XFS_OPSTATE_UNMOUNTING		0
#define XFS_OPSTATE_CLEAN		1
#define XFS_OPSTATE_SHUTDOWN		2
#define XFS_OPSTATE_INODE32		3
#define XFS_OPSTATE_READONLY		4
#define XFS_OPSTATE_INODEGC_ENABLED	5
#define XFS_OPSTATE_BLOCKGC_ENABLED	6
#define XFS_OPSTATE_WARNED_SHRINK	9
#define XFS_OPSTATE_WARNED_LARP		10
#define XFS_OPSTATE_QUOTACHECK_RUNNING	11
#define XFS_OPSTATE_UNSET_LOG_INCOMPAT	12
#define XFS_OPSTATE_USE_LARP		13
#define XFS_OPSTATE_WARNED_LBS		14
#define XFS_OPSTATE_WARNED_METADIR	17
#define XFS_OPSTATE_RESUMING_QUOTAON	18
#define XFS_OPSTATE_ZONEGC_RUNNING	20

#define __XFS_IS_OPSTATE(name, NAME) \
static inline bool xfs_is_ ## name (struct xfs_mount *mp) \
{ \
	return test_bit(XFS_OPSTATE_ ## NAME, &mp->m_opstate); \
} \
static inline bool xfs_clear_ ## name (struct xfs_mount *mp) \
{ \
	return test_and_clear_bit(XFS_OPSTATE_ ## NAME, &mp->m_opstate); \
} \
static inline bool xfs_set_ ## name (struct xfs_mount *mp) \
{ \
	return test_and_set_bit(XFS_OPSTATE_ ## NAME, &mp->m_opstate); \
}

__XFS_IS_OPSTATE(unmounting, UNMOUNTING)
__XFS_IS_OPSTATE(clean, CLEAN)
__XFS_IS_OPSTATE(shutdown, SHUTDOWN)
__XFS_IS_OPSTATE(inode32, INODE32)
__XFS_IS_OPSTATE(readonly, READONLY)
__XFS_IS_OPSTATE(inodegc_enabled, INODEGC_ENABLED)
__XFS_IS_OPSTATE(blockgc_enabled, BLOCKGC_ENABLED)
static inline bool xfs_is_quotacheck_running(struct xfs_mount *mp) { (void)mp; return false; }
static inline bool xfs_is_resuming_quotaon(struct xfs_mount *mp) { (void)mp; return false; }
static inline void xfs_set_resuming_quotaon(struct xfs_mount *m) { (void)m; }
static inline bool xfs_clear_resuming_quotaon(struct xfs_mount *mp) { (void)mp; return false; }
__XFS_IS_OPSTATE(done_with_log_incompat, UNSET_LOG_INCOMPAT)
__XFS_IS_OPSTATE(using_logged_xattrs, USE_LARP)
__XFS_IS_OPSTATE(zonegc_running, ZONEGC_RUNNING)

static inline bool xfs_should_warn(struct xfs_mount *mp, long nr)
{
	return !test_and_set_bit((unsigned int)nr, &mp->m_opstate);
}

#define XFS_MAX_IO_LOG		30
#define XFS_MIN_IO_LOG		PAGE_SHIFT

/* A read-only mount has nothing to shut down: record and log. */
void xfs_do_force_shutdown(struct xfs_mount *mp, uint32_t flags, char *fname, int lnnum);
#define xfs_force_shutdown(m, f)	xfs_do_force_shutdown(m, f, __FILE__, __LINE__)

#define SHUTDOWN_META_IO_ERROR	(1u << 0)
#define SHUTDOWN_LOG_IO_ERROR	(1u << 1)
#define SHUTDOWN_FORCE_UMOUNT	(1u << 2)
#define SHUTDOWN_CORRUPT_INCORE	(1u << 3)
#define SHUTDOWN_CORRUPT_ONDISK	(1u << 4)
#define SHUTDOWN_DEVICE_REMOVED	(1u << 5)

#define XFS_MFSI_QUIET		0x40

static inline xfs_agnumber_t
xfs_daddr_to_agno(struct xfs_mount *mp, xfs_daddr_t d)
{
	xfs_rfsblock_t ld = XFS_BB_TO_FSBT(mp, d);
	do_div(ld, mp->m_sb.sb_agblocks);
	return (xfs_agnumber_t) ld;
}

static inline xfs_agblock_t
xfs_daddr_to_agbno(struct xfs_mount *mp, xfs_daddr_t d)
{
	xfs_rfsblock_t ld = XFS_BB_TO_FSBT(mp, d);
	return (xfs_agblock_t) do_div(ld, mp->m_sb.sb_agblocks);
}

#define XFS_FDBLOCKS_BATCH	1024

uint64_t xfs_default_resblks(struct xfs_mount *mp, enum xfs_free_counter ctr);
uint64_t xfs_freecounter_unavailable(struct xfs_mount *mp, enum xfs_free_counter ctr);

static inline s64 xfs_sum_freecounter(struct xfs_mount *mp, enum xfs_free_counter ctr)
{ return percpu_counter_sum_positive(&mp->m_free[ctr].count); }
static inline s64 xfs_sum_freecounter_raw(struct xfs_mount *mp, enum xfs_free_counter ctr)
{ return percpu_counter_sum(&mp->m_free[ctr].count); }
static inline s64 xfs_estimate_freecounter(struct xfs_mount *mp, enum xfs_free_counter ctr)
{ return percpu_counter_read_positive(&mp->m_free[ctr].count); }
static inline void xfs_set_freecounter(struct xfs_mount *mp, enum xfs_free_counter ctr, uint64_t val)
{ percpu_counter_set(&mp->m_free[ctr].count, (s64)val); }

int xfs_dec_freecounter(struct xfs_mount *mp, enum xfs_free_counter ctr,
		uint64_t delta, bool rsvd);
void xfs_add_freecounter(struct xfs_mount *mp, enum xfs_free_counter ctr, uint64_t delta);

static inline int xfs_dec_fdblocks(struct xfs_mount *mp, uint64_t delta, bool reserved)
{ return xfs_dec_freecounter(mp, XC_FREE_BLOCKS, delta, reserved); }
static inline void xfs_add_fdblocks(struct xfs_mount *mp, uint64_t delta)
{ xfs_add_freecounter(mp, XC_FREE_BLOCKS, delta); }
static inline int xfs_dec_frextents(struct xfs_mount *mp, uint64_t delta)
{ return xfs_dec_freecounter(mp, XC_FREE_RTEXTENTS, delta, false); }
static inline void xfs_add_frextents(struct xfs_mount *mp, uint64_t delta)
{ xfs_add_freecounter(mp, XC_FREE_RTEXTENTS, delta); }

static inline int xfs_compare_freecounter(struct xfs_mount *mp, enum xfs_free_counter ctr, s64 rhs, s32 batch)
{ return __percpu_counter_compare(&mp->m_free[ctr].count, rhs, batch); }
void	xfs_reinit_percpu_counters(struct xfs_mount *mp);
xfs_agnumber_t xfs_set_inode_alloc(struct xfs_mount *mp, xfs_agnumber_t agcount);
int	xfs_sb_validate_fsb_count(struct xfs_sb *, uint64_t);
void	xfs_set_low_space_thresholds(struct xfs_mount *);
void	xfs_force_summary_recalc(struct xfs_mount *mp);
int	xfs_zero_extent(struct xfs_inode *ip, xfs_fsblock_t start_fsb, xfs_off_t count_fsb);
void	xfs_mod_delalloc(struct xfs_inode *ip, int64_t data_delta, int64_t ind_delta);
static inline void xfs_mod_sb_delalloc(struct xfs_mount *mp, int64_t delta)
{ percpu_counter_add(&mp->m_delalloc_blks, delta); }
xfs_extlen_t xfs_calc_group_awu_align_max(struct xfs_mount *mp, enum xfs_group_type type);

static inline struct xfs_buftarg *
xfs_group_type_buftarg(struct xfs_mount *mp, enum xfs_group_type type)
{
	switch (type) {
	case XG_TYPE_AG:
		return mp->m_ddev_targp;
	case XG_TYPE_RTG:
		return mp->m_rtdev_targp;
	default:
		ASSERT(0);
		break;
	}
	return NULL;
}

#endif	/* __XFS_MOUNT_H__ */
