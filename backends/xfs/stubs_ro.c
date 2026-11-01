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

void xfs_ag_geom_health(struct xfs_perag *pag, struct xfs_ag_geometry *ageo)
{
	WFS_STUB_RO();
}

void xfs_agno_mark_sick(struct xfs_mount *mp, xfs_agnumber_t agno, unsigned int mask)
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

void xfs_btree_mark_sick(struct xfs_btree_cur *cur)
{
	WFS_STUB_RO();
}

void xfs_da_mark_sick(struct xfs_da_args *args)
{
	WFS_STUB_RO();
}

void xfs_dirattr_mark_sick(struct xfs_inode *ip, int whichfork)
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

void xfs_zoned_add_available(struct xfs_mount *mp, xfs_filblks_t count_fsb)
{
	WFS_STUB_RO();
}

