// SPDX-License-Identifier: GPL-2.0-only
/*
 * Definitions the compiled kernel files expect from fs/xfs files that are not
 * compiled here (xfs_mount.c, xfs_fsops.c, xfs_super.c, xfs_icache.c,
 * xfs_error.c, xfs_health.c, xfs_discard.c, and features M3 does not write).
 * Grouped by what they stand for; each says what it does instead.
 *
 * Unsupported write features (reflink, realtime, quota, parent pointers,
 * exchange-range, zoned) are refused at mount (M3 design 1); their entry
 * points here fail or report "nothing to do" so a stray call cannot write.
 */
#include <stdio.h>	/* vsnprintf */
#include "xfs_platform.h"
#include "xfs_fs.h"
#include "xfs_shared.h"
#include "xfs_format.h"
#include "xfs_log_format.h"
#include "xfs_trans_resv.h"
#include "xfs_mount.h"
#include "xfs_defer.h"
#include "xfs_inode.h"
#include "xfs_da_format.h"
#include "xfs_da_btree.h"
#include "xfs_dir2.h"
#include "xfs_attr.h"
#include "xfs_trans.h"
#include "xfs_btree.h"
#include "xfs_bmap.h"
#include "xfs_error.h"
#include "xfs_quota_defs.h"
#include "xfs_filestream.h"
#include "xfs_icache.h"
#include "xfs_symlink.h"
#include "xfs_trans_priv.h"
#include "xfs_log.h"
#include "xfs_log_priv.h"
#include "xfs_log_recover.h"
#include "xfs_reflink.h"
#include "xfs_health.h"
#include "xfs_parent.h"
#include "xfs_extent_busy.h"
#include "xfs_discard.h"
#include "xfs_iomap.h"
#include "xfs_exchmaps.h"
#include "xfs_rtbitmap.h"
#include "xfs_rtgroup.h"
#include "xfs_rtrmap_btree.h"
#include "xfs_rtrefcount_btree.h"
#include "xfs_zone_alloc.h"
#include "xfs_metafile.h"
#include "xfs_rtalloc.h"
#include "xfs_healthmon.h"
#include "xfs_icache.h"
#include "wfs_xfs.h"

/* ---- shutdown (kernel xfs_fsops.c) ------------------------------------- */

bool wfs_xfs_is_shutdown(struct xfs_mount *mp)
{
	return xfs_is_shutdown(mp);
}

void wfs_xfs_force_shutdown(struct xfs_mount *mp, uint32_t flags)
{
	xfs_force_shutdown(mp, flags);
}

/* ---- mount state (kernel xfs_mount.c / xfs_super.c) --------------------- */

/* No page cache: file data is never dirty in memory (M3 design 2). */
void xfs_flush_inodes(struct xfs_mount *mp)
{
	(void)mp;
}

/* ---- background garbage collection (kernel xfs_icache.c) ---------------
 * No speculative preallocation and no CoW forks are created, and inode
 * inactivation is done synchronously by the inode cache, so there is nothing
 * to collect.
 */
void xfs_blockgc_worker(struct work_struct *work) { (void)work; }
int xfs_blockgc_flush_all(struct xfs_mount *mp) { (void)mp; return 0; }
int xfs_inodegc_flush(struct xfs_mount *mp) { (void)mp; return 0; }
void xfs_inode_clear_eofblocks_tag(struct xfs_inode *ip) { (void)ip; }
void xfs_inode_set_cowblocks_tag(struct xfs_inode *ip) { (void)ip; }
void xfs_inode_clear_cowblocks_tag(struct xfs_inode *ip) { (void)ip; }
void xfs_filestream_deassociate(struct xfs_inode *ip) { (void)ip; }
void xfs_blockgc_start(struct xfs_mount *mp) { (void)mp; }
void xfs_blockgc_stop(struct xfs_mount *mp) { (void)mp; }
void xfs_inodegc_start(struct xfs_mount *mp) { (void)mp; }
void xfs_inodegc_stop(struct xfs_mount *mp) { (void)mp; }
int xfs_inodegc_register_shrinker(struct xfs_mount *mp) { (void)mp; return 0; }
/* The inode cache (icache.c) frees its entries itself when the mount goes. */
void xfs_reclaim_inodes(struct xfs_mount *mp) { (void)mp; }
void xfs_health_unmount(struct xfs_mount *mp) { (void)mp; }
void xfs_healthmon_unmount(struct xfs_mount *mp) { (void)mp; }
void xfs_healthmon_report_shutdown(struct xfs_mount *mp, uint32_t flags) { (void)mp; (void)flags; }

/* ---- realtime, zoned, metadir (kernel xfs_rtalloc.c, xfs_zone_*.c) ------
 * The RT device is read (M2) but never written (M3 design 1), zoned and
 * metadir filesystems are refused by the mount policy: these follow the
 * kernel's !CONFIG_XFS_RT inlines, except that RT data stays readable.
 */
int xfs_rtmount_init(struct xfs_mount *mp) { (void)mp; return 0; }
int xfs_rtmount_inodes(struct xfs_mount *mp) { (void)mp; return 0; }
void xfs_rtunmount_inodes(struct xfs_mount *mp) { (void)mp; }
int xfs_rtalloc_reinit_frextents(struct xfs_mount *mp) { (void)mp; return 0; }
int xfs_growfs_check_rtgeom(const struct xfs_mount *mp, xfs_rfsblock_t dblocks,
			    xfs_rfsblock_t rblocks, xfs_extlen_t rextsize)
{
	(void)mp; (void)dblocks; (void)rblocks; (void)rextsize;
	return 0;
}
void xfs_rtrmapbt_compute_maxlevels(struct xfs_mount *mp) { mp->m_rtrmap_maxlevels = 0; }
void xfs_rtrefcountbt_compute_maxlevels(struct xfs_mount *mp) { mp->m_rtrefc_maxlevels = 0; }
int xfs_mount_zones(struct xfs_mount *mp) { (void)mp; return -EOPNOTSUPP; }
void xfs_unmount_zones(struct xfs_mount *mp) { (void)mp; }
void xfs_zone_gc_start(struct xfs_mount *mp) { (void)mp; }
void xfs_zone_gc_stop(struct xfs_mount *mp) { (void)mp; }
int xfs_metafile_iget(struct xfs_mount *mp, xfs_ino_t ino, enum xfs_metafile_type metafile_type,
		      struct xfs_inode **ipp)
{
	(void)mp; (void)ino; (void)metafile_type; (void)ipp;
	return -EOPNOTSUPP;
}

/* ---- messages and health (kernel xfs_error.c, xfs_buf.c, xfs_health.c) -- */

void xfs_error_report(const char *tag, int level, int error, struct xfs_mount *mp,
		      const char *filename, int linenum, xfs_failaddr_t failaddr)
{
	(void)level;
	xfs_alert(mp, "Internal error %s (%d) at line %d of file %s. Caller %p",
		  tag, error, linenum, filename, failaddr);
}

void xfs_buf_alert_ratelimited(struct xfs_buf *bp, const char *rlmsg, const char *fmt, ...)
{
	char msg[256];
	va_list ap;

	(void)rlmsg;
	va_start(ap, fmt);
	vsnprintf(msg, sizeof(msg), fmt, ap);
	va_end(ap);
	xfs_alert(bp->b_mount, "%s", msg);
}

/* Sickness is not tracked (no online repair): report "all healthy, unchecked". */
void xfs_fs_measure_sickness(struct xfs_mount *mp, unsigned int *sick, unsigned int *checked)
{
	(void)mp;
	*sick = *checked = 0;
}

void xfs_group_measure_sickness(struct xfs_group *xg, unsigned int *sick, unsigned int *checked)
{
	(void)xg;
	*sick = *checked = 0;
}

void xfs_inode_measure_sickness(struct xfs_inode *ip, unsigned int *sick, unsigned int *checked)
{
	(void)ip;
	*sick = *checked = 0;
}

/* ---- discard (kernel xfs_discard.c) --------------------------------------
 * Only called with the discard mount option, which is not offered. Should it
 * ever be called, release the busy extents at once as a discard completion
 * would (a busy extent left behind would block the allocator forever).
 */
void xfs_discard_extents(struct xfs_mount *mp, struct xfs_busy_extents *extents)
{
	(void)mp;
	xfs_extent_busy_clear(&extents->extent_list, false);
	kfree(extents->owner);
}

/* ---- realtime: no realtime writes (mount refuses) ------------------------ */

/* Used for log reservation sizes: these must equal the kernel's (xfs_rtbitmap.c). */
xfs_rtxnum_t xfs_rtbitmap_rtx_per_rbmblock(struct xfs_mount *mp)
{
	unsigned int rbmblock_bytes = mp->m_sb.sb_blocksize;

	if (xfs_has_rtgroups(mp))
		rbmblock_bytes -= sizeof(struct xfs_rtbuf_blkinfo);
	return rbmblock_bytes * NBBY;
}

xfs_filblks_t xfs_rtbitmap_blockcount_len(struct xfs_mount *mp, xfs_rtbxlen_t rtextents)
{
	if (xfs_has_zoned(mp))
		return 0;
	return howmany_64(rtextents, xfs_rtbitmap_rtx_per_rbmblock(mp));
}

const struct xfs_buf_ops *xfs_rtblock_ops(struct xfs_mount *mp, enum xfs_rtg_inodes type)
{
	(void)mp; (void)type;
	return NULL;
}

/* The rt btrees are never walked or changed here; the ops objects exist for
 * identity comparisons (xfs_btree_is_rtrmap() and friends). */
const struct xfs_btree_ops xfs_rtrmapbt_ops;
const struct xfs_btree_ops xfs_rtrefcountbt_ops;
const struct xfs_buf_ops xfs_rtrmapbt_buf_ops = { .name = "xfs_rtrmapbt (unsupported)" };
const struct xfs_buf_ops xfs_rtrefcountbt_buf_ops = { .name = "xfs_rtrefcountbt (unsupported)" };

struct xfs_btree_cur *xfs_rtrmapbt_init_cursor(struct xfs_trans *tp, struct xfs_rtgroup *rtg)
{
	(void)tp; (void)rtg;
	return NULL;
}

struct xfs_btree_cur *xfs_rtrefcountbt_init_cursor(struct xfs_trans *tp, struct xfs_rtgroup *rtg)
{
	(void)tp; (void)rtg;
	return NULL;
}

void xfs_rtrmapbt_to_disk(struct xfs_mount *mp, struct xfs_btree_block *rblock,
			  unsigned int rblocklen, struct xfs_rtrmap_root *dblock,
			  unsigned int dblocklen)
{
	(void)mp; (void)rblock; (void)rblocklen; (void)dblock; (void)dblocklen;
	ASSERT(0);
}

void xfs_rtrefcountbt_to_disk(struct xfs_mount *mp, struct xfs_btree_block *rblock,
			      int rblocklen, struct xfs_rtrefcount_root *dblock, int dblocklen)
{
	(void)mp; (void)rblock; (void)rblocklen; (void)dblock; (void)dblocklen;
	ASSERT(0);
}

int xfs_zone_free_blocks(struct xfs_trans *tp, struct xfs_rtgroup *rtg,
			 xfs_fsblock_t fsbno, xfs_filblks_t len)
{
	(void)tp; (void)rtg; (void)fsbno; (void)len;
	return -EOPNOTSUPP;
}

/* ---- reflink: no shared extents are written (mount refuses reflink) ------ */

int xfs_reflink_trim_around_shared(struct xfs_inode *ip, struct xfs_bmbt_irec *irec,
				   bool *shared)
{
	(void)ip; (void)irec;
	*shared = false;
	return 0;
}

int xfs_reflink_cancel_cow_blocks(struct xfs_inode *ip, struct xfs_trans **tpp,
				  xfs_fileoff_t offset_fsb, xfs_fileoff_t end_fsb,
				  bool cancel_real)
{
	(void)ip; (void)tpp; (void)offset_fsb; (void)end_fsb; (void)cancel_real;
	return 0;	/* no CoW fork */
}

int xfs_reflink_cancel_cow_range(struct xfs_inode *ip, xfs_off_t offset, xfs_off_t count,
				 bool cancel_real)
{
	(void)ip; (void)offset; (void)count; (void)cancel_real;
	return 0;
}

/* no reflink writes (M3 design 1): no CoW-based atomic writes */
xfs_extlen_t xfs_reflink_max_atomic_cow(struct xfs_mount *mp) { (void)mp; return 0; }

int xfs_reflink_recover_cow(struct xfs_mount *mp)
{
	(void)mp;
	return 0;
}

/* ---- parent pointers, exchange-range, quota: not written ----------------- */

struct kmem_cache *xfs_parent_args_cache;

xfs_dahash_t xfs_parent_hashattr(struct xfs_mount *mp, const uint8_t *name, int namelen,
				 const void *value, int valuelen)
{
	(void)mp; (void)name; (void)namelen; (void)value; (void)valuelen;
	return 0;
}

/* Parent pointer attributes are not understood: treat them as invalid. */
bool xfs_parent_namecheck(unsigned int attr_flags, const void *name, size_t length)
{
	(void)attr_flags; (void)name; (void)length;
	return false;
}

bool xfs_parent_valuecheck(struct xfs_mount *mp, const void *value, size_t valuelen)
{
	(void)mp; (void)value; (void)valuelen;
	return false;
}

int xfs_exchmaps_intent_init_cache(void) { return 0; }
void xfs_exchmaps_intent_destroy_cache(void) { }

xfs_failaddr_t xfs_dquot_verify(struct xfs_mount *mp, struct xfs_disk_dquot *ddq, xfs_dqid_t id)
{
	(void)mp; (void)ddq; (void)id;
	return __this_address;	/* quota is not supported */
}

/*
 * Log items of unsupported features: a log that holds one cannot be replayed
 * here (recovery fails with EFSCORRUPTED and the mount is refused).
 */
static int unsupported_item(struct xlog *log, struct list_head *buffer_list,
			    struct xlog_recover_item *item, xfs_lsn_t lsn)
{
	(void)buffer_list; (void)item; (void)lsn;
	xfs_alert(log->l_mp, "log holds a quota or exchange-range item: not supported");
	return -EFSCORRUPTED;
}

const struct xlog_recover_item_ops xlog_dquot_item_ops = {
	.item_type	= XFS_LI_DQUOT,
	.commit_pass2	= unsupported_item,
};
const struct xlog_recover_item_ops xlog_quotaoff_item_ops = {
	.item_type	= XFS_LI_QUOTAOFF,
	.commit_pass2	= unsupported_item,
};
const struct xlog_recover_item_ops xlog_xmi_item_ops = {
	.item_type	= XFS_LI_XMI,
	.commit_pass2	= unsupported_item,
};
const struct xlog_recover_item_ops xlog_xmd_item_ops = {
	.item_type	= XFS_LI_XMD,
	.commit_pass2	= unsupported_item,
};

/* ---- later tasks ---------------------------------------------------------- */

/* Data write path (M3a T7). */
int xfs_zero_range(struct xfs_inode *ip, loff_t pos, loff_t len,
		   struct xfs_zone_alloc_ctx *ac, bool *did_zero)
{
	(void)ip; (void)pos; (void)len; (void)ac;
	if (did_zero)
		*did_zero = false;
	return -EOPNOTSUPP;
}
