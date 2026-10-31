// SPDX-License-Identifier: GPL-2.0-only
/*
 * XFS read-only mount (Task 6, M2 design 4.1, 5.5, 5.6, 9).
 *
 * A reduced xfs_mountfs. Steps (numbers are design 5.5):
 *   1 readsb with sector probing: the device sector is read first to learn
 *     sb_sectsize, then the superblock sector is re-read, its CRC checked,
 *     the feature policy applied (4.1) and finally the real libxfs verifier
 *     (xfs_sb_buf_ops, geometry checks) run
 *   2 policy + read-only flag (opstate READONLY is set before any read)
 *   3 geometry: xfs_sb_mount_common, *_compute_maxlevels, inode geometry,
 *     xfs_da_mount (skipping any of these gives NULL dereferences later)
 *   4 device checks: log / RT device presence (-ENODEV), size checks
 *   5 perag + rtgroup init, read ALL AGFs
 *   6 log state (logcheck.c, section 6)
 *   7 root inode: hook wfs_xfs_load_root() (Task 8)
 *   8 summary counters (5.6)
 * No write, no log replay, no xfs_sb_read_secondary.
 */
#include "wfs_xfs.h"
#include "xfs_sb.h"
#include "xfs_bit.h"
#include "xfs_types.h"
#include "xfs_ag.h"
#include "xfs_alloc.h"
#include "xfs_ialloc.h"
#include "xfs_bmap.h"
#include "xfs_btree.h"
#include "xfs_da_format.h"
#include "xfs_da_btree.h"
#include "xfs_dir2.h"
#include "xfs_rmap_btree.h"
#include "xfs_rtrmap_btree.h"
#include "xfs_rtrefcount_btree.h"
#include "xfs_refcount_btree.h"
#include "xfs_rtgroup.h"
#include "xfs_metafile.h"
#include "xfs_metadir.h"
#include "xfs_inode.h"
#include "xfs_cksum.h"
#include "xfs_error.h"
#include "xfs_log.h"
#include "logcheck.h"
#include "icache.h"
#include <stdarg.h>
#include <stdio.h>

#define LOGMSG(x, lvl, ...)	wfs_xfs_hostlog((x)->api, (lvl), __VA_ARGS__)

/* ------------------------------------------------------------------ log -- */

/*
 * The shim logs (xfs_warn & co.) through one process-wide sink. It forwards to
 * the host log of the first live mount; the count keeps it installed until
 * the last mount ends. A host passes one wfs_host_api for all mounts.
 */
static const wfs_host_api *g_log_api;
static int g_log_users;
static volatile int g_log_gate;	/* serialises attach/detach (count + hook change together) */

static void log_gate_lock(void)
{
	while (__sync_lock_test_and_set(&g_log_gate, 1))
		;
}
static void log_gate_unlock(void) { __sync_lock_release(&g_log_gate); }

static void shim_to_host(int level, const char *msg)
{
	const wfs_host_api *api = __atomic_load_n(&g_log_api, __ATOMIC_ACQUIRE);
	int l = level <= 3 ? 0 : level == 4 ? 1 : level <= 6 ? 2 : 3;

	if (api && api->log)
		api->log(l, msg);
}

static void log_attach(const wfs_host_api *api)
{
	log_gate_lock();
	if (g_log_users++ == 0) {
		__atomic_store_n(&g_log_api, api, __ATOMIC_RELEASE);
		wfs_log_set_hook(shim_to_host);
	}
	log_gate_unlock();
}

static void log_detach(void)
{
	log_gate_lock();
	if (--g_log_users == 0) {
		wfs_log_set_hook(NULL);
		__atomic_store_n(&g_log_api, NULL, __ATOMIC_RELEASE);
	}
	log_gate_unlock();
}

static void wfs_xfs_hostlog(const wfs_host_api *api, int level, const char *fmt, ...)
	__printf(3, 4);
static void wfs_xfs_hostlog(const wfs_host_api *api, int level, const char *fmt, ...)
{
	char msg[256];
	va_list ap;

	if (!api || !api->log)
		return;
	va_start(ap, fmt);
	vsnprintf(msg, sizeof(msg), fmt, ap);
	va_end(ap);
	api->log(level, msg);
}

/* ---------------------------------------------------------------- hooks -- */

/*
 * Mount step 7: create the inode cache and pin the root. The transient
 * reference is dropped again; the pin keeps m_rootip valid until unload.
 */
int wfs_xfs_load_root(struct wfs_xfs *x)
{
	struct xfs_inode *ip;
	int error;

	error = wfs_xfs_icache_create(x);
	if (error)
		return error;
	error = wfs_xfs_iget(x, x->mp.m_sb.sb_rootino, &ip);
	if (error == -EINVAL || error == -ENOENT)
		error = -EFSCORRUPTED;	/* the superblock points at a non-inode */
	if (error)
		return error;
	if (!S_ISDIR(VFS_I(ip)->i_mode)) {
		xfs_irele(ip);
		return -EFSCORRUPTED;
	}
	wfs_xfs_ipin(ip);
	x->mp.m_rootip = ip;
	x->root_loaded = true;
	xfs_irele(ip);
	return 0;
}

/* Drop the root pin and free the inode cache; safe if load_root never ran. */
void wfs_xfs_unload_root(struct wfs_xfs *x)
{
	x->mp.m_rootip = NULL;
	x->root_loaded = false;
	wfs_xfs_icache_destroy(x);
}

/* ------------------------------------------------- libxfs-called helpers -- */

/*
 * Inode allocation policy only matters for writes: every AG is usable for
 * reads, inode32 is not in effect (kernel returns the usable AG count).
 */
xfs_agnumber_t xfs_set_inode_alloc(struct xfs_mount *mp, xfs_agnumber_t agcount)
{
	xfs_clear_inode32(mp);
	return agcount;
}

/*
 * M2 does not open the metadata directory tree (design 4.1, METADIR is
 * accepted, sb_metadirino is ignored; RT groups come from superblock values).
 * Nothing in the read path calls this; if something ever does, refuse.
 */
int xfs_metadir_load(struct xfs_trans *tp, struct xfs_inode *dp, const char *path,
		     enum xfs_metafile_type metafile_type, struct xfs_inode **ipp)
{
	*ipp = NULL;
	return -EOPNOTSUPP;
}

/*
 * Max records per RT rmap / refcount btree block (xfs_rtrmap_btree.c,
 * xfs_rtrefcount_btree.c are not part of the read path). xfs_sb_mount_common
 * stores them in m_rtrmap_mxr/m_rtrefc_mxr; the trees themselves are never read.
 */
unsigned int xfs_rtrmapbt_maxrecs(struct xfs_mount *mp, unsigned int blocklen, bool leaf)
{
	blocklen -= XFS_RTRMAP_BLOCK_LEN;
	if (leaf)
		return blocklen / sizeof(struct xfs_rmap_rec);
	return blocklen / (2 * sizeof(struct xfs_rmap_key) + sizeof(xfs_rtrmap_ptr_t));
}

unsigned int xfs_rtrefcountbt_maxrecs(struct xfs_mount *mp, unsigned int blocklen, bool leaf)
{
	blocklen -= XFS_RTREFCOUNT_BLOCK_LEN;
	if (leaf)
		return blocklen / sizeof(struct xfs_refcount_rec);
	return blocklen / (sizeof(struct xfs_refcount_key) + sizeof(xfs_rtrefcount_ptr_t));
}

/* Health state is not tracked by the read-only port (no scrub, no repair). */
void xfs_fs_mark_healthy(struct xfs_mount *mp, unsigned int mask)
{
}

/* xfs_mount.c: sync the percpu counters from the in-core superblock. */
void xfs_reinit_percpu_counters(struct xfs_mount *mp)
{
	percpu_counter_set(&mp->m_icount, (s64)mp->m_sb.sb_icount);
	percpu_counter_set(&mp->m_ifree, (s64)mp->m_sb.sb_ifree);
	xfs_set_freecounter(mp, XC_FREE_BLOCKS, mp->m_sb.sb_fdblocks);
	if (!xfs_has_zoned(mp))
		xfs_set_freecounter(mp, XC_FREE_RTEXTENTS, mp->m_sb.sb_frextents);
}

/* ---------------------------------------------------------- step 1: sb -- */

/*
 * Pick the log buftarg from the raw superblock (the libxfs verifier compares
 * m_logdev_targp with m_ddev_targp, as the kernel's xfs_open_devices set it up):
 * internal log (sb_logstart != 0): the data device, a given log device is
 * ignored with a warning; external log: the log device; absent: -ENODEV, or
 * with norecovery an unbound placeholder (!= ddev, so the verifier passes)
 * and log_role -1, which check_devices turns into "no log checks".
 */
static int select_log_target(struct wfs_xfs *x, const struct xfs_sb *raw)
{
	struct xfs_mount *mp = &x->mp;

	if (raw->sb_logstart) {
		if (x->bt_bound[WFS_DEV_LOG])
			LOGMSG(x, 1, "xfs: the filesystem has an internal log: "
			       "the log device is ignored");
		mp->m_logdev_targp = mp->m_ddev_targp;
		x->log_role = WFS_DEV_DATA;
	} else if (x->bt_bound[WFS_DEV_LOG]) {
		mp->m_logdev_targp = &x->bt[WFS_DEV_LOG];
		x->log_role = WFS_DEV_LOG;
	} else if (x->opts.norecovery) {
		LOGMSG(x, 1, "xfs: external log device not given: mounting without a log "
		       "check (norecovery)");
		mp->m_logdev_targp = &x->bt[WFS_DEV_LOG];	/* placeholder, not bound */
		x->log_role = -1;
	} else {
		LOGMSG(x, 0, "xfs: the filesystem has an external log: pass the log device "
		       "(or mount with the norecovery option)");
		return -ENODEV;
	}
	return 0;
}

#define SB_SECT_OK(s)	((s) >= 512 && (s) <= 32768 && ((s) & ((s) - 1)) == 0)

/* Kernel errors of the verified superblock read: only I/O and ENOMEM keep
 * their meaning, every verifier verdict (EINVAL, EFBIG, ...) is "corrupt". */
static int sb_verdict(int err)
{
	err = err > 0 ? -err : err;
	if (err == -EIO || err == -ENOMEM)
		return err;
	return -EFSCORRUPTED;
}

/*
 * Feature policy (design 4.1) on the raw, CRC-checked superblock.
 * Returns 0 or a negative kernel errno; logs the reason with the hex mask.
 */
static int check_policy(struct wfs_xfs *x, const struct xfs_sb *sb)
{
	uint32_t incompat = sb->sb_features_incompat;
	uint32_t unknown = incompat & (uint32_t)XFS_SB_FEAT_INCOMPAT_UNKNOWN;
	uint32_t zoned = incompat & (XFS_SB_FEAT_INCOMPAT_ZONED | XFS_SB_FEAT_INCOMPAT_ZONE_GAPS);

	if (sb->sb_features_compat & (uint32_t)XFS_SB_FEAT_COMPAT_UNKNOWN)
		LOGMSG(x, 1, "xfs: unknown compatible features 0x%x ignored",
		       sb->sb_features_compat & (uint32_t)XFS_SB_FEAT_COMPAT_UNKNOWN);
	if (sb->sb_features_ro_compat & (uint32_t)XFS_SB_FEAT_RO_COMPAT_UNKNOWN)
		LOGMSG(x, 1, "xfs: unknown read-only compatible features 0x%x, "
		       "mounting read-only",
		       sb->sb_features_ro_compat & (uint32_t)XFS_SB_FEAT_RO_COMPAT_UNKNOWN);
	if (unknown) {
		LOGMSG(x, 0, "xfs: unsupported: unknown incompatible features 0x%x", unknown);
		return -EOPNOTSUPP;
	}
	if (zoned) {
		LOGMSG(x, 0, "xfs: unsupported: zoned allocator features 0x%x", zoned);
		return -EOPNOTSUPP;
	}
	if ((incompat & XFS_SB_FEAT_INCOMPAT_NEEDSREPAIR) && !x->opts.norecovery) {
		LOGMSG(x, 0, "xfs: superblock is marked NEEDSREPAIR: run xfs_repair "
		       "(or mount with the norecovery option)");
		return -EFSCORRUPTED;
	}
	if (sb->sb_inprogress) {
		LOGMSG(x, 0, "xfs: superblock sb_inprogress is set (mkfs did not finish)");
		return -EFSCORRUPTED;
	}
	return 0;
}

/*
 * libxfs' xfs_validate_sb_common shifts by sb_blocklog before its own range
 * check runs (the vendored tree is never edited). Range-check the log2 fields
 * and the sizes derived from them first, so a hostile value can not reach a
 * shift with an exponent >= the type width.
 */
static int prevalidate_sb(struct wfs_xfs *x, const struct xfs_sb *sb)
{
	/*
	 * The per-AG structures (~250 B each) are allocated before AG 0 is read;
	 * a hostile agcount on a huge sparse device would otherwise cost GBs.
	 * Real filesystems have hundreds of AGs at most.
	 */
	if (sb->sb_agcount > WFS_XFS_MAX_AGCOUNT) {
		LOGMSG(x, 0, "xfs: unsupported: %u allocation groups (limit %u)",
		       sb->sb_agcount, WFS_XFS_MAX_AGCOUNT);
		return -EFSCORRUPTED;
	}
	if (sb->sb_blocklog < XFS_MIN_BLOCKSIZE_LOG || sb->sb_blocklog > XFS_MAX_BLOCKSIZE_LOG ||
	    sb->sb_blocksize != (1u << sb->sb_blocklog) ||
	    sb->sb_sectlog < XFS_MIN_SECTORSIZE_LOG || sb->sb_sectlog > XFS_MAX_SECTORSIZE_LOG ||
	    sb->sb_logsectlog > XFS_MAX_SECTORSIZE_LOG ||
	    sb->sb_sectsize != (1u << sb->sb_sectlog) ||
	    sb->sb_inodelog < XFS_DINODE_MIN_LOG || sb->sb_inodelog > XFS_DINODE_MAX_LOG ||
	    sb->sb_inodesize != (1u << sb->sb_inodelog) ||
	    sb->sb_inopblog > XFS_MAX_BLOCKSIZE_LOG || sb->sb_agblklog > 32 ||
	    sb->sb_dirblklog > XFS_MAX_BLOCKSIZE_LOG) {
		LOGMSG(x, 0, "xfs: corrupt superblock: block/sector/inode size fields "
		       "(blocklog %u sectlog %u inodelog %u)", sb->sb_blocklog,
		       sb->sb_sectlog, sb->sb_inodelog);
		return -EFSCORRUPTED;
	}
	return 0;
}

static int read_sb(struct wfs_xfs *x)
{
	struct xfs_mount *mp = &x->mp;
	struct xfs_buftarg *bt = mp->m_ddev_targp;
	unsigned int dev_ss = (unsigned int)bt->bt_logical_sectorsize;
	struct xfs_sb raw;
	struct xfs_buf *bp;
	int error;

	/* pass 1: one device sector, only to learn sb_sectsize */
	error = xfs_buf_read_uncached(bt, XFS_SB_DADDR, BTOBB(dev_ss), &bp, NULL);
	if (error) {
		LOGMSG(x, 0, "xfs: cannot read the superblock (error %d)", error);
		return error;
	}
	xfs_sb_from_disk(&raw, bp->b_addr);
	xfs_buf_relse(bp);
	if (raw.sb_magicnum != XFS_SB_MAGIC) {
		LOGMSG(x, 0, "xfs: not an XFS filesystem (bad superblock magic)");
		return -EINVAL;
	}
	if ((raw.sb_versionnum & XFS_SB_VERSION_NUMBITS) != XFS_SB_VERSION_5) {
		LOGMSG(x, 0, "xfs: unsupported: filesystem version %u (only v5/CRC is supported)",
		       raw.sb_versionnum & XFS_SB_VERSION_NUMBITS);
		return -EOPNOTSUPP;
	}
	if (!SB_SECT_OK(raw.sb_sectsize)) {
		LOGMSG(x, 0, "xfs: corrupt superblock: sector size %u", raw.sb_sectsize);
		return -EFSCORRUPTED;
	}
	if (dev_ss > raw.sb_sectsize) {
		LOGMSG(x, 0, "xfs: device sector size %u is larger than the filesystem's %u",
		       dev_ss, raw.sb_sectsize);
		return -EINVAL;
	}

	/* pass 2: the whole sb sector, CRC before anything else is trusted */
	error = xfs_buf_read_uncached(bt, XFS_SB_DADDR, BTOBB(raw.sb_sectsize), &bp, NULL);
	if (error) {
		LOGMSG(x, 0, "xfs: cannot read the superblock sector (error %d)", error);
		return error;
	}
	if (!xfs_buf_verify_cksum(bp, XFS_SB_CRC_OFF)) {
		xfs_buf_relse(bp);
		LOGMSG(x, 0, "xfs: superblock CRC mismatch");
		return -EFSBADCRC;
	}
	xfs_sb_from_disk(&raw, bp->b_addr);
	xfs_buf_relse(bp);
	error = check_policy(x, &raw);
	if (error)
		return error;
	error = prevalidate_sb(x, &raw);
	if (error)
		return error;
	error = select_log_target(x, &raw);
	if (error)
		return error;

	/* pass 3: the libxfs verifier (geometry, limits). Its sanity checks
	 * use macros that read mp->m_sb (XFS_FSB_TO_B ...), so the CRC-checked
	 * raw copy goes there first, as the kernel's xfs_readsb does. */
	mp->m_sb = raw;
	error = xfs_buf_read_uncached(bt, XFS_SB_DADDR, BTOBB(raw.sb_sectsize), &bp,
				      &xfs_sb_buf_ops);
	if (error) {
		LOGMSG(x, 0, "xfs: superblock validation failed (error %d)", error);
		return sb_verdict(error);
	}
	xfs_sb_from_disk(&mp->m_sb, bp->b_addr);
	xfs_buf_relse(bp);
	mp->m_features |= xfs_sb_version_to_features(&mp->m_sb);
	xfs_reinit_percpu_counters(mp);
	return 0;
}

/* ------------------------------------------------------- step 3: geometry -- */

static int setup_geometry(struct wfs_xfs *x)
{
	struct xfs_mount *mp = &x->mp;
	struct xfs_sb *sbp = &mp->m_sb;
	struct xfs_ino_geometry *igeo = M_IGEO(mp);
	unsigned int levels;
	int error;

	xfs_sb_mount_common(mp, sbp);
	xfs_alloc_compute_maxlevels(mp);
	xfs_bmap_compute_maxlevels(mp, XFS_DATA_FORK);
	xfs_bmap_compute_maxlevels(mp, XFS_ATTR_FORK);
	igeo->attr_fork_offset = xfs_bmap_compute_attr_offset(mp);
	xfs_ialloc_setup_geometry(mp);
	xfs_rmapbt_compute_maxlevels(mp);
	/*
	 * xfs_rtrmapbt/xfs_rtrefcountbt_compute_maxlevels live in files that
	 * are not part of the read path: the RT rmap/refcount trees are never
	 * walked (design 4.2), their heights stay 0.
	 */
	mp->m_rtrmap_maxlevels = 0;
	mp->m_rtrefc_maxlevels = 0;
	xfs_refcountbt_compute_maxlevels(mp);
	levels = max(mp->m_alloc_maxlevels, igeo->inobt_maxlevels);
	levels = max(levels, mp->m_rmap_maxlevels);
	mp->m_agbtree_maxlevels = max(levels, mp->m_refc_maxlevels);
	mp->m_rtbtree_maxlevels = max(mp->m_rtrmap_maxlevels, mp->m_rtrefc_maxlevels);

	mp->m_allocsize_log = max_t(uint32_t, sbp->sb_blocklog, mp->m_allocsize_log);
	mp->m_allocsize_blocks = 1U << (mp->m_allocsize_log - sbp->sb_blocklog);

	if (xfs_has_sparseinodes(mp) &&
	    sbp->sb_spino_align != XFS_B_TO_FSBT(mp, igeo->inode_cluster_size_raw)) {
		LOGMSG(x, 0, "xfs: corrupt superblock: sparse inode alignment %u",
		       sbp->sb_spino_align);
		return -EFSCORRUPTED;
	}
	error = xfs_da_mount(mp);
	if (error) {
		LOGMSG(x, 0, "xfs: directory geometry setup failed (%d)", error);
		return error;
	}
	return 0;
}

/* ----------------------------------------- step 4: devices and sizes -- */

/* nblocks (fs blocks) must fit the device (when its size is known). */
static int check_dev_size(struct wfs_xfs *x, int role, uint64_t nblocks, const char *what)
{
	struct xfs_mount *mp = &x->mp;
	uint64_t bytes;

	if (nblocks == 0 ||
	    __builtin_mul_overflow(nblocks, (uint64_t)mp->m_sb.sb_blocksize, &bytes) ||
	    bytes > (uint64_t)INT64_MAX) {
		LOGMSG(x, 0, "xfs: corrupt superblock: %s size of %llu blocks", what,
		       (unsigned long long)nblocks);
		return -EFSCORRUPTED;
	}
	if (x->blk.size_bytes[role] && bytes > x->blk.size_bytes[role]) {
		LOGMSG(x, 0, "xfs: %s is %llu bytes, the filesystem needs %llu",
		       what, (unsigned long long)x->blk.size_bytes[role],
		       (unsigned long long)bytes);
		return -EFSCORRUPTED;
	}
	return 0;
}

/* Read the last block of a device; kernel xfs_check_sizes / xfs_rtmount_init. */
static int read_last_block(struct wfs_xfs *x, struct xfs_buftarg *bt, uint64_t nblocks,
			   const char *what)
{
	struct xfs_mount *mp = &x->mp;
	xfs_daddr_t d = (xfs_daddr_t)XFS_FSB_TO_BB(mp, nblocks);
	struct xfs_buf *bp;
	int error;

	if (XFS_BB_TO_FSB(mp, d) != nblocks) {
		LOGMSG(x, 0, "xfs: %s size mismatch", what);
		return -EFSCORRUPTED;
	}
	error = xfs_buf_read_uncached(bt, d - XFS_FSB_TO_BB(mp, 1), XFS_FSB_TO_BB(mp, 1),
				      &bp, NULL);
	if (error) {
		LOGMSG(x, 0, "xfs: last block of the %s cannot be read (error %d)", what, error);
		return error;
	}
	xfs_buf_relse(bp);
	return 0;
}

static int check_devices(struct wfs_xfs *x)
{
	struct xfs_mount *mp = &x->mp;
	struct xfs_sb *sbp = &mp->m_sb;
	int error;

	if (sbp->sb_logblocks == 0) {
		LOGMSG(x, 0, "xfs: corrupt superblock: no log defined");
		return -EFSCORRUPTED;
	}
	if (x->log_role < 0)
		mp->m_logdev_targp = NULL;	/* external log absent, norecovery */

	/* realtime device: required whenever the filesystem has an RT section */
	if (sbp->sb_rblocks) {
		if (!x->bt_bound[WFS_DEV_RT]) {
			LOGMSG(x, 0, "xfs: the filesystem has a realtime section: "
			       "pass the realtime device");
			return -ENODEV;
		}
		mp->m_rtdev_targp = &x->bt[WFS_DEV_RT];
	}

	if (sbp->sb_logstart && !xfs_verify_fsbext(mp, sbp->sb_logstart, sbp->sb_logblocks)) {
		LOGMSG(x, 0, "xfs: corrupt superblock: internal log extent %llu+%u is outside "
		       "the data device", (unsigned long long)sbp->sb_logstart, sbp->sb_logblocks);
		return -EFSCORRUPTED;
	}
	error = check_dev_size(x, WFS_DEV_DATA, sbp->sb_dblocks, "data device");
	if (!error && mp->m_logdev_targp && mp->m_logdev_targp != mp->m_ddev_targp)
		error = check_dev_size(x, WFS_DEV_LOG, sbp->sb_logblocks, "log device");
	if (!error && mp->m_rtdev_targp)
		error = check_dev_size(x, WFS_DEV_RT, sbp->sb_rblocks, "realtime device");
	if (error)
		return error;

	error = read_last_block(x, mp->m_ddev_targp, sbp->sb_dblocks, "data device");
	if (!error && mp->m_logdev_targp && mp->m_logdev_targp != mp->m_ddev_targp)
		error = read_last_block(x, mp->m_logdev_targp, sbp->sb_logblocks, "log device");
	if (!error && mp->m_rtdev_targp)
		error = read_last_block(x, mp->m_rtdev_targp, sbp->sb_rblocks, "realtime device");
	return error;
}

/* ------------------------------------------------ step 5: perag and AGFs -- */

static int init_groups(struct wfs_xfs *x)
{
	struct xfs_mount *mp = &x->mp;
	struct xfs_sb *sbp = &mp->m_sb;
	xfs_agnumber_t agno;
	int error;

	error = xfs_initialize_perag(mp, 0, sbp->sb_agcount, sbp->sb_dblocks, &mp->m_maxagi);
	if (error) {
		LOGMSG(x, 0, "xfs: per-AG init failed (%d)", error);
		return error;
	}
	x->perag_up = true;
	error = xfs_initialize_rtgroups(mp, 0, sbp->sb_rgcount, sbp->sb_rextents);
	if (error) {
		LOGMSG(x, 0, "xfs: rtgroup init failed (%d)", error);
		return error;
	}
	x->rtg_up = true;
	/* statfs needs the btree block counts of every AG (design 5.6) */
	for (agno = 0; agno < sbp->sb_agcount; agno++) {
		struct xfs_perag *pag = xfs_perag_get(mp, agno);

		if (!pag)
			return -EFSCORRUPTED;
		error = xfs_alloc_read_agf(pag, NULL, 0, NULL);
		xfs_perag_put(pag);
		if (error) {
			LOGMSG(x, 0, "xfs: cannot read the free space header of AG %u (error %d)",
			       agno, error);
			return error;
		}
	}
	return 0;
}

/* ----------------------------------------------- step 6: log, 8: counters -- */

static int log_read(void *ctx, uint64_t off, void *buf, size_t len)
{
	struct wfs_xfs *x = ctx;

	return wfs_xfs_blk_read(&x->blk, x->log_role, off, buf, len) ? -1 : 0;
}

static int log_provider(struct xfs_mount *mp, struct wfs_log_reader *r)
{
	struct wfs_xfs *x = wfs_xfs_of(mp);

	if (!x || x->log_role < 0)
		return -EINVAL;
	r->read = log_read;
	r->ctx = x;
	return 0;
}

/*
 * Mount step 8, the summary counters (design 5.6 / kernel
 * xfs_check_summary_counts). Every AGF is read: statfs needs the allocbt
 * block total (m_allocbt_blks, summed by xfs_alloc_read_agf as each AG is
 * first read). The sb counters are trusted when the log is clean and they are
 * sane; a dirty log (norecovery; v5 is always lazy-count, so they are stale)
 * or insane values (fdblocks > dblocks, ifree > icount, icount outside the
 * possible range) recompute them from the AGF/AGI sums, which also reads all
 * AGIs. A failure there means corrupt AG headers and fails the mount.
 */
static int check_summary_counts(struct wfs_xfs *x, bool log_dirty)
{
	struct xfs_mount *mp = &x->mp;
	struct xfs_sb *sbp = &mp->m_sb;
	bool bad = sbp->sb_fdblocks > sbp->sb_dblocks ||
		   !xfs_verify_icount(mp, sbp->sb_icount) ||
		   sbp->sb_ifree > sbp->sb_icount;
	xfs_agnumber_t agno;
	int error;

	if (bad || log_dirty) {
		LOGMSG(x, 1, "xfs: summary counters %s: recomputing from the AG headers",
		       log_dirty ? "may be stale (dirty log)" : "are insane");
		error = xfs_initialize_perag_data(mp, sbp->sb_agcount);
		if (error)
			LOGMSG(x, 0, "xfs: summary counters are inconsistent (error %d)", error);
		return error;
	}
	for (agno = 0; agno < sbp->sb_agcount; agno++) {
		struct xfs_perag *pag = xfs_perag_get(mp, agno);

		if (!pag)
			return -EFSCORRUPTED;
		error = xfs_alloc_read_agf(pag, NULL, 0, NULL);
		xfs_perag_put(pag);
		if (error) {
			LOGMSG(x, 0, "xfs: cannot read the AGF of AG %u (error %d)", agno, error);
			return error;
		}
	}
	xfs_reinit_percpu_counters(mp);
	return 0;
}

/* ------------------------------------------------------------ mount API -- */

static void free_mount(struct wfs_xfs *x)
{
	struct xfs_mount *mp = &x->mp;
	int r;

	wfs_xfs_unload_root(x);
	/* Cached buffers hold perag references (b_pag): drop them first. */
	if (x->bcache)
		wfs_xfs_bcache_purge(x->bcache);
	if (x->rtg_up)
		xfs_free_rtgroups(mp, 0, mp->m_sb.sb_rgcount);
	if (x->perag_up)
		xfs_free_perag_range(mp, 0, mp->m_sb.sb_agcount);
	for (r = 0; r < XG_TYPE_MAX; r++)
		xa_destroy(&mp->m_groups[r].xa);	/* the shim table outlives its entries */
	if (mp->m_dir_geo || mp->m_attr_geo)
		xfs_da_unmount(mp);
	if (x->bcache)
		wfs_xfs_bcache_purge(x->bcache);
	for (r = 0; r < WFS_DEV_COUNT; r++)
		if (x->bt_bound[r])
			wfs_xfs_buftarg_unbind(&x->bt[r]);
	if (x->bcache)
		wfs_xfs_bcache_destroy(x->bcache);
	log_detach();
	if (x->mem_budget)
		wfs_mem_mount_budget(-(int64_t)x->mem_budget);
	kfree(x);
}

wfs_err_t wfs_xfs_mount(const wfs_host_api *api, wfs_blkdev *const devs[WFS_DEV_COUNT],
			const char *options, struct wfs_xfs **out)
{
	struct wfs_xfs *x;
	struct xfs_mount *mp;
	int error, r;
	bool log_dirty = false;	/* set by the log check; stale counters need a recompute */

	*out = NULL;
	if (!api || !devs || !devs[WFS_DEV_DATA] || !api->blk_read ||
	    !api->blk_size || !api->blk_sector_size)
		return -WFS_EINVAL;
	x = kzalloc(sizeof(*x), GFP_KERNEL);
	if (!x)
		return -WFS_ENOMEM;
	error = wfs_xfs_parse_options(api, options, &x->opts);
	if (error) {
		kfree(x);
		return error;
	}
	mp = &x->mp;
	x->api = api;
	x->log_role = -1;
	mp->m_wfs = x;
	log_attach(api);

	{
		uint32_t ss = api->blk_sector_size(devs[WFS_DEV_DATA]);

		if (ss < 512 || ss > 32768 || (ss & (ss - 1))) {
			LOGMSG(x, 0, "xfs: unsupported device sector size %u", ss);
			error = -EINVAL;
			goto fail;
		}
	}
	spin_lock_init(&mp->m_sb_lock);
	mutex_init(&mp->m_metafile_resv_lock);
	mutex_init(&mp->m_growlock);
	set_bit(XFS_OPSTATE_READONLY, &mp->m_opstate);	/* before any read */
	if (x->opts.norecovery)
		mp->m_features |= XFS_FEAT_NORECOVERY;

	wfs_xfs_blk_init_host(&x->blk, api, devs);
	/*
	 * Allocation cap for this mount: buffer cache + inode cache (8 KiB per
	 * entry covers inode, forks and a modest extent map) + 1 GiB of headroom
	 * for in-flight operations. Hostile growth past it fails the allocation
	 * (checked sites) instead of consuming the host's memory; see wfs_mem.c
	 * for __GFP_NOFAIL.
	 */
	x->mem_budget = x->opts.bufcache_bytes + (uint64_t)x->opts.inodecache_entries * 8192 +
			((uint64_t)1 << 30);
	wfs_mem_mount_budget((int64_t)x->mem_budget);
	x->bcache = wfs_xfs_bcache_create(&x->blk, (size_t)x->opts.bufcache_bytes);
	if (!x->bcache) {
		error = -ENOMEM;
		goto fail;
	}
	for (r = 0; r < WFS_DEV_COUNT; r++) {
		if (!devs[r])
			continue;
		x->bt[r].bt_mount = mp;
		error = wfs_xfs_buftarg_bind(&x->bt[r], x->bcache, r);
		if (error)
			goto fail;
		x->bt_bound[r] = true;
	}
	mp->m_ddev_targp = &x->bt[WFS_DEV_DATA];
	wfs_log_set_reader_provider(log_provider);

	/* 1, 2 */
	error = read_sb(x);
	if (error)
		goto fail;
	/* 3 */
	error = setup_geometry(x);
	if (error)
		goto fail;
	/* 4 */
	error = check_devices(x);
	if (error)
		goto fail;
	/* 5 */
	error = init_groups(x);
	if (error)
		goto fail;
	/* 6 */
	if (mp->m_logdev_targp) {
		error = xfs_log_mount_ro_check(mp, x->opts.norecovery, &log_dirty);
		if (error) {
			if (error == -EUCLEAN || error == -WFS_EUCLEAN)
				LOGMSG(x, 0, "xfs: log check failed: not mountable without "
				       "recovery (use the norecovery option)");
			goto fail;
		}
	}
	/* 7 */
	error = wfs_xfs_load_root(x);
	if (error)
		goto fail;
	/* 8 */
	error = check_summary_counts(x, log_dirty);
	if (error)
		goto fail;

	*out = x;
	return 0;
fail:
	free_mount(x);
	return wfs_xfs_err(error);
}

wfs_err_t wfs_xfs_unmount(struct wfs_xfs *x)
{
	if (!x)
		return -WFS_EINVAL;
	free_mount(x);
	return 0;
}

/* ---------------------------------------------------------------- probe -- */

wfs_err_t wfs_xfs_probe(const wfs_host_api *api, wfs_blkdev *const devs[WFS_DEV_COUNT])
{
	/* one device sector, at most 32 KiB; 8-byte aligned for xfs_dsb */
	uint64_t sect[32768 / 8];
	const struct xfs_dsb *dsb = (const struct xfs_dsb *)sect;
	wfs_blkdev *dev;
	uint32_t ss;
	int err;

	if (!api || !devs || !(dev = devs[WFS_DEV_DATA]) || !api->blk_read ||
	    !api->blk_size || !api->blk_sector_size)
		return -WFS_EINVAL;
	ss = api->blk_sector_size(dev);
	if (ss < 512 || ss > 32768 || (ss & (ss - 1)) || api->blk_size(dev) < ss)
		return -WFS_EINVAL;
	err = api->blk_read(dev, 0, sect, ss);
	if (err)
		return -WFS_EIO;
	if (be32_to_cpu(dsb->sb_magicnum) != XFS_SB_MAGIC)
		return -WFS_EINVAL;
	if ((be16_to_cpu(dsb->sb_versionnum) & XFS_SB_VERSION_NUMBITS) != XFS_SB_VERSION_5)
		return -WFS_EINVAL;
	return 0;
}

/*
 * xfs_log_check_lsn: the AGF/AGFL verifiers ask whether a metadata LSN lies
 * ahead of the log head (an inconsistency left by xfs_repair -L). This mount
 * is read-only and never replays or writes the log, so the check is not
 * enforced: a metadata block is accepted whatever LSN it carries. That is the
 * safe direction for reading (the check is a diagnostic, never a bounds
 * property of the data) and a real definition, not a placeholder, so nothing is
 * logged per mount. A stricter check would need the log head, which a
 * norecovery mount may not have; revisit if a write backend lands (M3).
 */
bool xfs_log_check_lsn(struct xfs_mount *mp, xfs_lsn_t lsn)
{
	(void)mp;
	(void)lsn;
	return true;
}

bool wfs_xfs_is_shutdown(struct xfs_mount *mp)
{
	struct wfs_xfs *x = wfs_xfs_of(mp);

	return x && __atomic_load_n(&x->shutdown, __ATOMIC_ACQUIRE) != 0;
}

void wfs_xfs_force_shutdown(struct xfs_mount *mp, uint32_t flags)
{
	struct wfs_xfs *x = wfs_xfs_of(mp);

	if (!x || !flags)
		return;
	if (__atomic_fetch_or(&x->shutdown, flags, __ATOMIC_ACQ_REL) == 0)
		xfs_alert(mp, "filesystem shut down (reason 0x%x): no further changes are written",
			  (unsigned int)flags);
}
