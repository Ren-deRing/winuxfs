// SPDX-License-Identifier: GPL-2.0-only
/*
 * Weak placeholders for libxfs-called functions that later tasks implement
 * with strong definitions elsewhere. Each is tagged PENDING(Tn); the task
 * named deletes the stub (the whole #if block) when it adds the real code.
 *
 * Why per-symbol objects: PE/COFF weak definitions in a separate object are
 * NOT honoured by GNU ld (mingw reports the symbol undefined), so the Makefile
 * compiles this file once per stub (-DWFS_ONLY_<symbol>) into libxfs_pending.a.
 * A strong definition in a directly linked object (buf.o, ...) wins because the
 * archive member is only pulled for a symbol that is still undefined; the
 * attribute below additionally keeps ELF semantics if the file is linked whole
 * (-DWFS_PENDING_ALL, used by the stub test). A stub returns an error
 * (-ENOSYS / false / NULL) and logs its name once. Nothing here aborts.
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

/* ---- PENDING(T5) ---- */

/* PENDING(T5) */
#if defined(WFS_PENDING_ALL) || defined(WFS_ONLY___xfs_buf_ioerror)
WFS_WEAK void __xfs_buf_ioerror(struct xfs_buf *bp, int error, xfs_failaddr_t failaddr)
{
	WFS_STUB_PENDING();
}
#endif

/* PENDING(T5) */
#if defined(WFS_PENDING_ALL) || defined(WFS_ONLY___xfs_buf_mark_corrupt)
WFS_WEAK void __xfs_buf_mark_corrupt(struct xfs_buf *bp, xfs_failaddr_t fa)
{
	WFS_STUB_PENDING();
}
#endif

/* PENDING(T5) */
#if defined(WFS_PENDING_ALL) || defined(WFS_ONLY_xfs_buf_get_map)
WFS_WEAK int xfs_buf_get_map(struct xfs_buftarg *target, struct xfs_buf_map *map, int nmaps, xfs_buf_flags_t flags, struct xfs_buf **bpp)
{
	WFS_STUB_PENDING();
	return -ENOSYS;
}
#endif

/* PENDING(T5) */
#if defined(WFS_PENDING_ALL) || defined(WFS_ONLY_xfs_buf_get_uncached)
WFS_WEAK int xfs_buf_get_uncached(struct xfs_buftarg *target, size_t numblks, struct xfs_buf **bpp)
{
	WFS_STUB_PENDING();
	return -ENOSYS;
}
#endif

/* PENDING(T5) */
#if defined(WFS_PENDING_ALL) || defined(WFS_ONLY_xfs_buf_read_map)
WFS_WEAK int xfs_buf_read_map(struct xfs_buftarg *target, struct xfs_buf_map *map, int nmaps, xfs_buf_flags_t flags, struct xfs_buf **bpp, const struct xfs_buf_ops *ops, xfs_failaddr_t fa)
{
	WFS_STUB_PENDING();
	return -ENOSYS;
}
#endif

/* PENDING(T5) */
#if defined(WFS_PENDING_ALL) || defined(WFS_ONLY_xfs_buf_readahead_map)
WFS_WEAK void xfs_buf_readahead_map(struct xfs_buftarg *target, struct xfs_buf_map *map, int nmaps, const struct xfs_buf_ops *ops)
{
	WFS_STUB_PENDING();
}
#endif

/* PENDING(T5) */
#if defined(WFS_PENDING_ALL) || defined(WFS_ONLY_xfs_buf_rele)
WFS_WEAK void xfs_buf_rele(struct xfs_buf *a0)
{
	WFS_STUB_PENDING();
}
#endif

/* PENDING(T5) */
#if defined(WFS_PENDING_ALL) || defined(WFS_ONLY_xfs_buf_set_ref)
WFS_WEAK void xfs_buf_set_ref(struct xfs_buf *bp, int lru_ref)
{
	WFS_STUB_PENDING();
}
#endif

/* PENDING(T5) */
#if defined(WFS_PENDING_ALL) || defined(WFS_ONLY_xfs_buf_set_uptodate)
WFS_WEAK void xfs_buf_set_uptodate(struct xfs_buf *bp)
{
	WFS_STUB_PENDING();
}
#endif

/* PENDING(T5) */
#if defined(WFS_PENDING_ALL) || defined(WFS_ONLY_xfs_buf_verifier_error)
WFS_WEAK void xfs_buf_verifier_error(struct xfs_buf *bp, int error, const char *name, const void *buf, size_t bufsz, xfs_failaddr_t failaddr)
{
	WFS_STUB_PENDING();
}
#endif

/* PENDING(T5) */
#if defined(WFS_PENDING_ALL) || defined(WFS_ONLY_xfs_verifier_error)
WFS_WEAK void xfs_verifier_error(struct xfs_buf *bp, int error, xfs_failaddr_t failaddr)
{
	WFS_STUB_PENDING();
}
#endif

/* PENDING(T5) */
#if defined(WFS_PENDING_ALL) || defined(WFS_ONLY_xfs_verify_magic)
WFS_WEAK bool xfs_verify_magic(struct xfs_buf *bp, __be32 dmagic)
{
	WFS_STUB_PENDING();
	struct xfs_mount *mp = bp->b_mount;
	int idx = xfs_has_crc(mp);

	if (!bp->b_ops || !bp->b_ops->magic[idx])
		return false;
	return dmagic == bp->b_ops->magic[idx];
}
#endif

/* PENDING(T5) */
#if defined(WFS_PENDING_ALL) || defined(WFS_ONLY_xfs_verify_magic16)
WFS_WEAK bool xfs_verify_magic16(struct xfs_buf *bp, __be16 dmagic)
{
	WFS_STUB_PENDING();
	struct xfs_mount *mp = bp->b_mount;
	int idx = xfs_has_crc(mp);

	if (!bp->b_ops || !bp->b_ops->magic16[idx])
		return false;
	return dmagic == bp->b_ops->magic16[idx];
}
#endif

/* ---- PENDING(T6) ---- */

/* PENDING(T6) */
#if defined(WFS_PENDING_ALL) || defined(WFS_ONLY_xfs_metadir_load)
WFS_WEAK int xfs_metadir_load(struct xfs_trans *tp, struct xfs_inode *dp, const char *path, enum xfs_metafile_type metafile_type, struct xfs_inode **ipp)
{
	WFS_STUB_PENDING();
	return -ENOSYS;
}
#endif

/* PENDING(T6) */
#if defined(WFS_PENDING_ALL) || defined(WFS_ONLY_xfs_set_inode_alloc)
WFS_WEAK xfs_agnumber_t xfs_set_inode_alloc(struct xfs_mount *mp, xfs_agnumber_t agcount)
{
	WFS_STUB_PENDING();
	/* Only matters for inode allocation; every AG is usable for reads. */
	return agcount;
}
#endif

/* ---- PENDING(T7) ---- */

/* PENDING(T7) */
#if defined(WFS_PENDING_ALL) || defined(WFS_ONLY_xfs_log_check_lsn)
WFS_WEAK bool xfs_log_check_lsn(struct xfs_mount *a0, xfs_lsn_t a1)
{
	WFS_STUB_PENDING();
	/* Permissive until logcheck.c knows the log head: a verified LSN is
	 * accepted. */
	return true;
}
#endif

/* ---- PENDING(T8) ---- */

/* PENDING(T8) */
#if defined(WFS_PENDING_ALL) || defined(WFS_ONLY_xfs_assert_ilocked)
WFS_WEAK void xfs_assert_ilocked(struct xfs_inode *a0, uint a1)
{
	WFS_STUB_PENDING();
}
#endif

/* PENDING(T8) */
#if defined(WFS_PENDING_ALL) || defined(WFS_ONLY_xfs_corruption_error)
WFS_WEAK void xfs_corruption_error(const char *tag, int level, struct xfs_mount *mp, const void *buf, size_t bufsize, const char *filename, int linenum, xfs_failaddr_t failaddr)
{
	WFS_STUB_PENDING();
}
#endif

/* PENDING(T8) */
#if defined(WFS_PENDING_ALL) || defined(WFS_ONLY_xfs_ilock)
WFS_WEAK void xfs_ilock(xfs_inode_t *a0, uint a1)
{
	WFS_STUB_PENDING();
}
#endif

/* PENDING(T8) */
#if defined(WFS_PENDING_ALL) || defined(WFS_ONLY_xfs_ilock_data_map_shared)
WFS_WEAK uint xfs_ilock_data_map_shared(struct xfs_inode *a0)
{
	WFS_STUB_PENDING();
	return 0;
}
#endif

/* PENDING(T8) */
#if defined(WFS_PENDING_ALL) || defined(WFS_ONLY_xfs_inode_verifier_error)
WFS_WEAK void xfs_inode_verifier_error(struct xfs_inode *ip, int error, const char *name, const void *buf, size_t bufsz, xfs_failaddr_t failaddr)
{
	WFS_STUB_PENDING();
}
#endif

/* PENDING(T8) */
#if defined(WFS_PENDING_ALL) || defined(WFS_ONLY_xfs_irele)
WFS_WEAK void xfs_irele(struct xfs_inode *ip)
{
	WFS_STUB_PENDING();
}
#endif

/* PENDING(T8) */
#if defined(WFS_PENDING_ALL) || defined(WFS_ONLY_xfs_is_always_cow_inode)
WFS_WEAK bool xfs_is_always_cow_inode(const struct xfs_inode *ip)
{
	WFS_STUB_PENDING();
	return false;
}
#endif

/* PENDING(T8) */
#if defined(WFS_PENDING_ALL) || defined(WFS_ONLY_xfs_iunlock)
WFS_WEAK void xfs_iunlock(xfs_inode_t *a0, uint a1)
{
	WFS_STUB_PENDING();
}
#endif

/* PENDING(T8) */
#if defined(WFS_PENDING_ALL) || defined(WFS_ONLY_xfs_trans_metafile_iget)
WFS_WEAK int xfs_trans_metafile_iget(struct xfs_trans *tp, xfs_ino_t ino, enum xfs_metafile_type metafile_type, struct xfs_inode **ipp)
{
	WFS_STUB_PENDING();
	return -ENOSYS;
}
#endif

