// SPDX-License-Identifier: GPL-2.0-only
/*
 * Stubs for libxfs-called symbols that a read-only mount never reaches
 * (transactions, allocation, rmap/refcount updates, quota, health marking,
 * ...): they return -EROFS (-ENOSYS where that is the more natural kernel
 * code) and log the function name for the first calls. They are here so the
 * 29 read-path files link as a whole and --gc-sections drops what is unused.
 * Nothing here aborts.
 */
#include "xfs_platform.h"
#include "xfs_fs.h"
#include "xfs_format.h"
#include "xfs_log_format.h"
#include "xfs_shared.h"
#include "xfs_trans_resv.h"
#include "xfs_bit.h"
#include "xfs_mount.h"
#include "xfs_inode.h"
#include "xfs_buf.h"
#include "xfs_da_format.h"
#include "xfs_da_btree.h"
#include "xfs_defer.h"
#include "xfs_btree.h"
#include "xfs_btree_staging.h"
#include "xfs_rmap.h"
#include "xfs_alloc.h"
#include "xfs_error.h"
#include "xfs_trans.h"
#include "xfs_ag.h"
#include "xfs_ag_resv.h"
#include "xfs_health.h"
#include "xfs_attr.h"
#include "xfs_attr_remote.h"
#include "xfs_bmap.h"
#include "xfs_bmap_item.h"
#include "xfs_bmap_util.h"
#include "xfs_extent_busy.h"
#include "xfs_extfree_item.h"
#include "xfs_filestream.h"
#include "xfs_icreate_item.h"
#include "xfs_inode_util.h"
#include "xfs_iomap.h"
#include "xfs_iunlink_item.h"
#include "xfs_log.h"
#include "xfs_metadir.h"
#include "xfs_metafile.h"
#include "xfs_parent.h"
#include "xfs_refcount.h"
#include "xfs_rtbitmap.h"
#include "xfs_rtgroup.h"
#include "xfs_rtrefcount_btree.h"
#include "xfs_rtrmap_btree.h"
#include "xfs_zone_alloc.h"
#include "stubs.h"

#ifdef _WIN32
#define WFS_WEAK
#else
#define WFS_WEAK __attribute__((weak))
#endif

/* Stub signatures follow the kernel prototypes (the headers above declare
 * them, so a mismatch is a compile error). Stubs never ASSERT or abort. */
#pragma GCC diagnostic ignored "-Wunused-parameter"

/* Owner infos defined in xfs_rmap.c upstream (not compiled): data, not stubs. */
const struct xfs_owner_info XFS_RMAP_OINFO_SKIP_UPDATE = { .oi_owner = XFS_RMAP_OWN_NULL };
const struct xfs_owner_info XFS_RMAP_OINFO_AG = { .oi_owner = XFS_RMAP_OWN_AG };
const struct xfs_owner_info XFS_RMAP_OINFO_INOBT = { .oi_owner = XFS_RMAP_OWN_INOBT };
const struct xfs_owner_info XFS_RMAP_OINFO_INODES = { .oi_owner = XFS_RMAP_OWN_INODES };
const struct xfs_owner_info XFS_RMAP_OINFO_REFC = { .oi_owner = XFS_RMAP_OWN_REFC };

void xfs_add_freecounter(struct xfs_mount *mp, enum xfs_free_counter ctr, uint64_t delta)
{
	WFS_STUB_RO();
}

void xfs_ag_geom_health(struct xfs_perag *pag, struct xfs_ag_geometry *ageo)
{
	WFS_STUB_RO();
}

void xfs_ag_resv_alloc_extent(struct xfs_perag *pag, enum xfs_ag_resv_type type, struct xfs_alloc_arg *args)
{
	WFS_STUB_RO();
}

void xfs_ag_resv_free(struct xfs_perag *pag)
{
	WFS_STUB_RO();
}

void xfs_ag_resv_free_extent(struct xfs_perag *pag, enum xfs_ag_resv_type type, struct xfs_trans *tp, xfs_extlen_t len)
{
	WFS_STUB_RO();
}

int xfs_ag_resv_init(struct xfs_perag *pag, struct xfs_trans *tp)
{
	WFS_STUB_RO();
	return -EROFS;
}

xfs_extlen_t xfs_ag_resv_needed(struct xfs_perag *pag, enum xfs_ag_resv_type type)
{
	/* No reservations are made on a read-only mount. */
	return 0;
}

void xfs_agno_mark_sick(struct xfs_mount *mp, xfs_agnumber_t agno, unsigned int mask)
{
	WFS_STUB_RO();
}

unsigned int xfs_attr3_rmt_blocks(struct xfs_mount *mp, unsigned int attrlen)
{
	/* Copy of xfs_attr_remote.c (not compiled). */
	if (xfs_has_crc(mp)) {
		unsigned int buflen = mp->m_sb.sb_blocksize - sizeof(struct xfs_attr3_rmt_hdr);

		return howmany(attrlen, buflen);
	}
	return XFS_B_TO_FSB(mp, attrlen);
}

bool xfs_attr_check_namespace(unsigned int attr_flags)
{
	/* Copy of xfs_attr.c (not compiled): at most one namespace flag. */
	return hweight8(attr_flags & XFS_ATTR_NSP_ONDISK_MASK) < 2;
}

xfs_dahash_t xfs_attr_hashval(struct xfs_mount *mp, unsigned int attr_flags, const uint8_t *name, int namelen, const void *value, int valuelen)
{
	WFS_STUB_RO();
	return 0;
}

int xfs_attr_rmtval_get(struct xfs_da_args *args)
{
	WFS_STUB_RO();
	return -ENOSYS;
}

void xfs_bmap_defer_add(struct xfs_trans *tp, struct xfs_bmap_intent *bi)
{
	WFS_STUB_RO();
}

void xfs_bmap_mark_sick(struct xfs_inode *ip, int whichfork)
{
	WFS_STUB_RO();
}

int xfs_bmap_rtalloc(struct xfs_bmalloca *ap)
{
	WFS_STUB_RO();
	return -ENOSYS;
}

int xfs_bmbt_to_iomap(struct xfs_inode *ip, struct iomap *iomap, struct xfs_bmbt_irec *imap, unsigned int mapping_flags, u16 iomap_flags, u64 sequence_cookie)
{
	WFS_STUB_RO();
	return -ENOSYS;
}

void xfs_btree_commit_afakeroot(struct xfs_btree_cur *cur, struct xfs_trans *tp, struct xfs_buf *agbp)
{
	WFS_STUB_RO();
}

void xfs_btree_commit_ifakeroot(struct xfs_btree_cur *cur, struct xfs_trans *tp, int whichfork)
{
	WFS_STUB_RO();
}

void xfs_btree_mark_sick(struct xfs_btree_cur *cur)
{
	WFS_STUB_RO();
}

bool xfs_buf_delwri_queue(struct xfs_buf *a0, struct list_head *a1)
{
	WFS_STUB_RO();
	return false;
}

int xfs_buf_delwri_submit(struct list_head *a0)
{
	WFS_STUB_RO();
	return -EROFS;
}

void xfs_da_mark_sick(struct xfs_da_args *args)
{
	WFS_STUB_RO();
}

int xfs_dec_freecounter(struct xfs_mount *mp, enum xfs_free_counter ctr, uint64_t delta, bool rsvd)
{
	WFS_STUB_RO();
	return -EROFS;
}

int xfs_defer_finish(struct xfs_trans **tp)
{
	WFS_STUB_RO();
	return -EROFS;
}

void xfs_defer_item_pause(struct xfs_trans *tp, struct xfs_defer_pending *dfp)
{
	WFS_STUB_RO();
}

void xfs_defer_item_unpause(struct xfs_trans *tp, struct xfs_defer_pending *dfp)
{
	WFS_STUB_RO();
}

void xfs_dirattr_mark_sick(struct xfs_inode *ip, int whichfork)
{
	WFS_STUB_RO();
}

void xfs_do_force_shutdown(struct xfs_mount *mp, uint32_t flags, char *fname, int lnnum)
{
	WFS_STUB_RO();
}

int xfs_extent_busy_flush(struct xfs_trans *tp, struct xfs_group *xg, unsigned busy_gen, uint32_t alloc_flags)
{
	WFS_STUB_RO();
	return -ENOSYS;
}

void xfs_extent_busy_insert(struct xfs_trans *tp, struct xfs_group *xg, xfs_agblock_t bno, xfs_extlen_t len, unsigned int flags)
{
	WFS_STUB_RO();
}

void xfs_extent_busy_reuse(struct xfs_group *xg, xfs_agblock_t fbno, xfs_extlen_t flen, bool userdata)
{
	WFS_STUB_RO();
}

bool xfs_extent_busy_trim(struct xfs_group *xg, xfs_extlen_t minlen, xfs_extlen_t maxlen, xfs_agblock_t *bno, xfs_extlen_t *len, unsigned *busy_gen)
{
	WFS_STUB_RO();
	return false;
}

void xfs_extent_free_defer_add(struct xfs_trans *tp, struct xfs_extent_free_item *xefi, struct xfs_defer_pending **dfpp)
{
	WFS_STUB_RO();
}

int xfs_filestream_select_ag(struct xfs_bmalloca *ap, struct xfs_alloc_arg *args, xfs_extlen_t *blen)
{
	WFS_STUB_RO();
	return -ENOSYS;
}

void xfs_fs_mark_sick(struct xfs_mount *mp, unsigned int mask)
{
	WFS_STUB_RO();
}

void xfs_group_mark_sick(struct xfs_group *xg, unsigned int mask)
{
	WFS_STUB_RO();
}

void xfs_group_measure_sickness(struct xfs_group *xg, unsigned int *sick, unsigned int *checked)
{
	WFS_STUB_RO();
}

void xfs_icreate_log(struct xfs_trans *tp, xfs_agnumber_t agno, xfs_agblock_t agbno, unsigned int count, unsigned int inode_size, xfs_agblock_t length, unsigned int generation)
{
	WFS_STUB_RO();
}

void xfs_iflush_rtrefcount(struct xfs_inode *ip, struct xfs_dinode *dip)
{
	WFS_STUB_RO();
}

void xfs_iflush_rtrmap(struct xfs_inode *ip, struct xfs_dinode *dip)
{
	WFS_STUB_RO();
}

int xfs_iformat_rtrefcount(struct xfs_inode *ip, struct xfs_dinode *dip)
{
	WFS_STUB_RO();
	return -ENOSYS;
}

int xfs_iformat_rtrmap(struct xfs_inode *ip, struct xfs_dinode *dip)
{
	WFS_STUB_RO();
	return -ENOSYS;
}

void xfs_inode_mark_sick(struct xfs_inode *ip, unsigned int mask)
{
	WFS_STUB_RO();
}

u64 xfs_iomap_inode_sequence(struct xfs_inode *ip, u16 iomap_flags)
{
	WFS_STUB_RO();
	return 0;
}

int xfs_iunlink_log_inode(struct xfs_trans *tp, struct xfs_inode *ip, struct xfs_perag *pag, xfs_agino_t next_agino)
{
	WFS_STUB_RO();
	return -EROFS;
}

struct xfs_inode * xfs_iunlink_lookup(struct xfs_perag *pag, xfs_agino_t agino)
{
	WFS_STUB_RO();
	return NULL;
}

int xfs_iunlink_reload_next(struct xfs_trans *tp, struct xfs_buf *agibp, xfs_agino_t prev_agino, xfs_agino_t next_agino)
{
	WFS_STUB_RO();
	return -EROFS;
}

int xfs_metadir_create_file(struct xfs_metadir_update *upd, umode_t mode, xfs_metadir_createfn create, void *priv, struct xfs_inode **ipp)
{
	WFS_STUB_RO();
	return -EROFS;
}

int xfs_metadir_mkdir(struct xfs_inode *dp, const char *path, struct xfs_inode **ipp)
{
	WFS_STUB_RO();
	return -EROFS;
}

void xfs_mod_delalloc(struct xfs_inode *ip, int64_t data_delta, int64_t ind_delta)
{
	WFS_STUB_RO();
}

int xfs_parent_addname(struct xfs_trans *tp, struct xfs_parent_args *ppargs, struct xfs_inode *dp, const struct xfs_name *parent_name, struct xfs_inode *child)
{
	WFS_STUB_RO();
	return -EROFS;
}

int xfs_parent_removename(struct xfs_trans *tp, struct xfs_parent_args *ppargs, struct xfs_inode *dp, const struct xfs_name *parent_name, struct xfs_inode *child)
{
	WFS_STUB_RO();
	return -EROFS;
}

int xfs_parent_replacename(struct xfs_trans *tp, struct xfs_parent_args *ppargs, struct xfs_inode *old_dp, const struct xfs_name *old_name, struct xfs_inode *new_dp, const struct xfs_name *new_name, struct xfs_inode *child)
{
	WFS_STUB_RO();
	return -EROFS;
}

void xfs_refcount_alloc_cow_extent(struct xfs_trans *tp, bool isrt, xfs_fsblock_t fsb, xfs_extlen_t len)
{
	WFS_STUB_RO();
}

void xfs_refcount_decrease_extent(struct xfs_trans *tp, bool isrt, struct xfs_bmbt_irec *irec)
{
	WFS_STUB_RO();
}

int xfs_rmap_alloc(struct xfs_trans *tp, struct xfs_buf *agbp, struct xfs_perag *pag, xfs_agblock_t bno, xfs_extlen_t len, const struct xfs_owner_info *oinfo)
{
	WFS_STUB_RO();
	return -EROFS;
}

void xfs_rmap_convert_extent(struct xfs_mount *mp, struct xfs_trans *tp, struct xfs_inode *ip, int whichfork, struct xfs_bmbt_irec *imap)
{
	WFS_STUB_RO();
}

int xfs_rmap_free(struct xfs_trans *tp, struct xfs_buf *agbp, struct xfs_perag *pag, xfs_agblock_t bno, xfs_extlen_t len, const struct xfs_owner_info *oinfo)
{
	WFS_STUB_RO();
	return -EROFS;
}

void xfs_rmap_map_extent(struct xfs_trans *tp, struct xfs_inode *ip, int whichfork, struct xfs_bmbt_irec *imap)
{
	WFS_STUB_RO();
}

void xfs_rmap_unmap_extent(struct xfs_trans *tp, struct xfs_inode *ip, int whichfork, struct xfs_bmbt_irec *imap)
{
	WFS_STUB_RO();
}

int xfs_rtbitmap_create(struct xfs_rtgroup *rtg, struct xfs_inode *ip, struct xfs_trans *tp, bool init)
{
	WFS_STUB_RO();
	return -EROFS;
}

int xfs_rtfree_blocks(struct xfs_trans *tp, struct xfs_rtgroup *rtg, xfs_fsblock_t rtbno, xfs_filblks_t rtlen)
{
	WFS_STUB_RO();
	return -EROFS;
}

void xfs_rtgroup_geom_health(struct xfs_rtgroup *rtg, struct xfs_rtgroup_geometry *rgeo)
{
	WFS_STUB_RO();
}

xfs_filblks_t xfs_rtrefcountbt_calc_reserves(struct xfs_mount *mp)
{
	WFS_STUB_RO();
	return 0;
}

int xfs_rtrefcountbt_create(struct xfs_rtgroup *rtg, struct xfs_inode *ip, struct xfs_trans *tp, bool init)
{
	WFS_STUB_RO();
	return -EROFS;
}

void xfs_rtrefcountbt_destroy_cur_cache(void)
{
	WFS_STUB_RO();
}

int xfs_rtrefcountbt_init_cur_cache(void)
{
	WFS_STUB_RO();
	return -ENOSYS;
}

xfs_filblks_t xfs_rtrmapbt_calc_reserves(struct xfs_mount *mp)
{
	WFS_STUB_RO();
	return 0;
}

int xfs_rtrmapbt_create(struct xfs_rtgroup *rtg, struct xfs_inode *ip, struct xfs_trans *tp, bool init)
{
	WFS_STUB_RO();
	return -EROFS;
}

void xfs_rtrmapbt_destroy_cur_cache(void)
{
	WFS_STUB_RO();
}

int xfs_rtrmapbt_init_cur_cache(void)
{
	WFS_STUB_RO();
	return -ENOSYS;
}

int xfs_rtsummary_create(struct xfs_rtgroup *rtg, struct xfs_inode *ip, struct xfs_trans *tp, bool init)
{
	WFS_STUB_RO();
	return -EROFS;
}

int xfs_trans_alloc(struct xfs_mount *mp, struct xfs_trans_res *resp, uint blocks, uint rtextents, uint flags, struct xfs_trans **tpp)
{
	WFS_STUB_RO();
	return -EROFS;
}

void xfs_trans_bhold(xfs_trans_t *a0, struct xfs_buf *a1)
{
	WFS_STUB_RO();
}

void xfs_trans_binval(xfs_trans_t *a0, struct xfs_buf *a1)
{
	WFS_STUB_RO();
}

void xfs_trans_bjoin(xfs_trans_t *a0, struct xfs_buf *a1)
{
	WFS_STUB_RO();
}

void xfs_trans_buf_copy_type(struct xfs_buf *dst_bp, struct xfs_buf *src_bp)
{
	WFS_STUB_RO();
}

void xfs_trans_buf_set_type(struct xfs_trans *a0, struct xfs_buf *a1, enum xfs_blft a2)
{
	/* the kernel does nothing without a transaction; reads here run with tp == NULL */
	if (!a0)
		return;
	WFS_STUB_RO();
}

void xfs_trans_cancel_error(struct xfs_trans *a0, int a1)
{
	WFS_STUB_RO();
}

int xfs_trans_commit(struct xfs_trans *a0)
{
	WFS_STUB_RO();
	return -EROFS;
}

int xfs_trans_get_buf_map(struct xfs_trans *tp, struct xfs_buftarg *target, struct xfs_buf_map *map, int nmaps, xfs_buf_flags_t flags, struct xfs_buf **bpp)
{
	WFS_STUB_RO();
	return -EROFS;
}

struct xfs_buf * xfs_trans_getrtsb(struct xfs_trans *tp)
{
	WFS_STUB_RO();
	return NULL;
}

struct xfs_buf * xfs_trans_getsb(struct xfs_trans *a0)
{
	WFS_STUB_RO();
	return NULL;
}

void xfs_trans_ichgtime(struct xfs_trans *tp, struct xfs_inode *ip, int flags)
{
	WFS_STUB_RO();
}

void xfs_trans_ijoin(struct xfs_trans *a0, struct xfs_inode *a1, uint a2)
{
	WFS_STUB_RO();
}

void xfs_trans_inode_alloc_buf(xfs_trans_t *a0, struct xfs_buf *a1)
{
	WFS_STUB_RO();
}

void xfs_trans_log_buf(struct xfs_trans *a0, struct xfs_buf *a1, uint a2, uint a3)
{
	WFS_STUB_RO();
}

void xfs_trans_log_inode(xfs_trans_t *a0, struct xfs_inode *a1, uint a2)
{
	WFS_STUB_RO();
}

void xfs_trans_mod_sb(xfs_trans_t *a0, uint a1, int64_t a2)
{
	WFS_STUB_RO();
}

bool xfs_trans_ordered_buf(xfs_trans_t *a0, struct xfs_buf *a1)
{
	WFS_STUB_RO();
	return false;
}

int xfs_trans_roll(struct xfs_trans **a0)
{
	WFS_STUB_RO();
	return -EROFS;
}

int xfs_zero_extent(struct xfs_inode *ip, xfs_fsblock_t start_fsb, xfs_off_t count_fsb)
{
	WFS_STUB_RO();
	return -EROFS;
}

void xfs_zoned_add_available(struct xfs_mount *mp, xfs_filblks_t count_fsb)
{
	WFS_STUB_RO();
}

