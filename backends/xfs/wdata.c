// SPDX-License-Identifier: GPL-2.0-only
/*
 * Data write, truncate, fsync and sync (M3a T7). No page cache: data goes to
 * the device at once (write-through) in the kernel's direct-IO order:
 *   1. holes in the range are allocated as UNWRITTEN extents (transaction),
 *   2. the data is written to the device (head/tail padding of new blocks is
 *      zero-filled, sub-sector edges are read-modify-write),
 *   3. blk_flush, then unwritten extents are converted (transaction),
 *   4. i_size grows in a last small transaction.
 * So a crash never exposes stale block contents and the size never covers
 * data that is not durable. Overwrites of written extents are in place.
 * Not supported yet: realtime files (-EOPNOTSUPP).
 */
#include "icache.h"
#include "xfs_bit.h"
#include "xfs_btree.h"
#include "xfs_bmap_btree.h"
#include "xfs_trans.h"
#include "xfs_bmap.h"
#include "xfs_bmap_util.h"
#include "xfs_trans_space.h"
#include "xfs_log.h"
#include "xfs_trans_priv.h"
#include "xfs_inode_util.h"
#include <stdlib.h>
#include <string.h>

#define WRITE_CHUNK	((uint64_t)16 << 20)

static int d_get(struct wfs_xfs *x, wfs_ino_t ino, struct xfs_inode **ipp)
{
	xfs_ino_t xino;
	int error = wfs_xfs_ino_from_wfs(&x->mp, ino, &xino);

	if (error)
		return error;
	error = wfs_xfs_iget(x, xino, ipp);
	return error ? wfs_xfs_host_iget_err(error) : 0;
}

static int d_check(struct wfs_xfs *x)
{
	if (!x->rw)
		return -WFS_EROFS;
	if (wfs_xfs_is_shutdown(&x->mp))
		return -WFS_EIO;
	return 0;
}

static int ddev_io(struct wfs_xfs *x, bool wr, uint64_t off, void *buf, size_t len)
{
	uint64_t size = x->blk.size_bytes[WFS_DEV_DATA];
	int rc;

	if (size && (off > size || len > size - off))
		return -WFS_EUCLEAN;
	rc = wr ? wfs_xfs_blk_write(&x->blk, WFS_DEV_DATA, off, buf, len)
		: wfs_xfs_blk_read(&x->blk, WFS_DEV_DATA, off, buf, len);
	return rc ? -WFS_EIO : 0;
}

/* Write len bytes at device offset off; sub-sector edges are read-modify-write. */
static int ddev_write(struct wfs_xfs *x, uint64_t off, const uint8_t *data, size_t len)
{
	uint32_t ss = x->blk.sector_size[WFS_DEV_DATA] ? x->blk.sector_size[WFS_DEV_DATA] : 512;
	uint8_t *tmp = NULL;
	int rc = 0;

	while (len && !rc) {
		uint32_t head = (uint32_t)(off % ss);

		if (!head && len >= ss) {
			size_t n = len - len % ss;

			rc = ddev_io(x, true, off, (void *)data, n);
			off += n; data += n; len -= n;
			continue;
		}
		if (!tmp) {
			tmp = malloc(ss);
			if (!tmp)
				return -WFS_ENOMEM;
		}
		rc = ddev_io(x, false, off - head, tmp, ss);
		if (!rc) {
			size_t n = len < ss - head ? len : ss - head;

			memcpy(tmp + head, data, n);
			rc = ddev_io(x, true, off - head, tmp, ss);
			off += n; data += n; len -= n;
		}
	}
	free(tmp);
	return rc;
}

static int ddev_zero(struct wfs_xfs *x, uint64_t off, uint64_t len)
{
	static const uint8_t z[4096];
	int rc = 0;

	while (len && !rc) {
		size_t n = len < sizeof(z) ? (size_t)len : sizeof(z);

		rc = ddev_write(x, off, z, n);
		off += n; len -= n;
	}
	return rc;
}

static int dev_off_of(struct wfs_xfs *x, const struct xfs_bmbt_irec *imap, uint64_t skip, uint64_t *off)
{
	struct xfs_mount *mp = &x->mp;

	if (!xfs_verify_fsbext(mp, imap->br_startblock, imap->br_blockcount))
		return -WFS_EUCLEAN;
	*off = (uint64_t)BBTOB(XFS_FSB_TO_DADDR(mp, imap->br_startblock)) + skip;
	return 0;
}

static int read_map(struct xfs_inode *ip, xfs_fileoff_t fsb, xfs_filblks_t cnt, struct xfs_bmbt_irec *imap)
{
	uint32_t lm = xfs_ilock_data_map_shared(ip);
	int n = 1, error = xfs_bmapi_read(ip, fsb, cnt, imap, &n, 0);

	xfs_iunlock(ip, lm);
	if (error)
		return wfs_xfs_err(error);
	if (n != 1 || imap->br_startoff != fsb || !imap->br_blockcount)
		return -WFS_EUCLEAN;
	return 0;
}

static void set_size(struct xfs_trans *tp, struct xfs_inode *ip, uint64_t size)
{
	ip->i_disk_size = size;
	i_size_write(VFS_I(ip), size);
	xfs_trans_ichgtime(tp, ip, XFS_ICHGTIME_MOD | XFS_ICHGTIME_CHG);
	xfs_trans_log_inode(tp, ip, XFS_ILOG_CORE);
}

/* Step 1: allocate unwritten extents for the holes in [fsb, end). *limit: first block not allocated. */
static int alloc_holes(struct xfs_inode *ip, xfs_fileoff_t fsb, xfs_fileoff_t end,
		       xfs_fileoff_t *limit, bool *alloced)
{
	struct xfs_mount *mp = ip->i_mount;

	*limit = end;
	while (fsb < end) {
		struct xfs_bmbt_irec imap;
		struct xfs_trans *tp;
		xfs_filblks_t cnt;
		uint32_t resblks;
		int error, n = 1;

		error = read_map(ip, fsb, end - fsb, &imap);
		if (error)
			return error;
		if (imap.br_startblock == DELAYSTARTBLOCK)
			return -WFS_EUCLEAN;
		if (imap.br_startblock != HOLESTARTBLOCK) {
			fsb = imap.br_startoff + imap.br_blockcount;
			continue;
		}
		cnt = imap.br_blockcount < end - fsb ? imap.br_blockcount : end - fsb;
		if (cnt > 1 << 20)
			cnt = 1 << 20;
		resblks = XFS_DIOSTRAT_SPACE_RES(mp, (uint32_t)cnt);
		error = xfs_trans_alloc(mp, &M_RES(mp)->tr_write, resblks, 0, 0, &tp);
		if (error == -ENOSPC && cnt > 1) {	/* try less */
			cnt = 1;
			resblks = XFS_DIOSTRAT_SPACE_RES(mp, 1);
			error = xfs_trans_alloc(mp, &M_RES(mp)->tr_write, resblks, 0, 0, &tp);
		}
		if (error) {
			if (error == -ENOSPC && fsb > *limit - (end - fsb) && fsb != end) {
				*limit = fsb;
				return 0;
			}
			return wfs_xfs_err(error);
		}
		xfs_ilock(ip, XFS_ILOCK_EXCL);
		xfs_trans_ijoin(tp, ip, 0);
		error = xfs_bmapi_write(tp, ip, fsb, cnt, XFS_BMAPI_PREALLOC, resblks, &imap, &n);
		if (!error)
			error = xfs_trans_commit(tp);
		else
			xfs_trans_cancel(tp);
		xfs_iunlock(ip, XFS_ILOCK_EXCL);
		if (error == -ENOSPC) {
			*limit = fsb;
			return 0;
		}
		if (error)
			return wfs_xfs_err(error);
		*alloced = true;
		if (n != 1 || !imap.br_blockcount)
			return -WFS_EUCLEAN;
		fsb = imap.br_startoff + imap.br_blockcount;
	}
	return 0;
}

/* Step 3: convert the unwritten extents in [fsb, end) to written. */
static int convert_range(struct xfs_inode *ip, xfs_fileoff_t fsb, xfs_fileoff_t end)
{
	struct xfs_mount *mp = ip->i_mount;

	while (fsb < end) {
		struct xfs_bmbt_irec imap;
		struct xfs_trans *tp;
		uint32_t resblks = XFS_DIOSTRAT_SPACE_RES(mp, 0) << 1;
		xfs_filblks_t cnt;
		int error, n = 1;

		error = read_map(ip, fsb, end - fsb, &imap);
		if (error)
			return error;
		cnt = imap.br_blockcount < end - fsb ? imap.br_blockcount : end - fsb;
		if (imap.br_state != XFS_EXT_UNWRITTEN || imap.br_startblock == HOLESTARTBLOCK) {
			fsb += cnt;
			continue;
		}
		error = xfs_trans_alloc(mp, &M_RES(mp)->tr_write, resblks, 0, 0, &tp);
		if (error)
			return wfs_xfs_err(error);
		xfs_ilock(ip, XFS_ILOCK_EXCL);
		xfs_trans_ijoin(tp, ip, 0);
		error = xfs_bmapi_write(tp, ip, fsb, cnt, XFS_BMAPI_CONVERT, resblks, &imap, &n);
		if (!error) {
			xfs_trans_log_inode(tp, ip, XFS_ILOG_CORE);
			error = xfs_trans_commit(tp);
		} else {
			xfs_trans_cancel(tp);
		}
		xfs_iunlock(ip, XFS_ILOCK_EXCL);
		if (error)
			return wfs_xfs_err(error);
		if (n != 1 || !imap.br_blockcount)
			return -WFS_EUCLEAN;
		fsb = imap.br_startoff + imap.br_blockcount;
	}
	return 0;
}

/* One chunk of a write at pos (len <= WRITE_CHUNK). *done: bytes written (may be < len on ENOSPC). */
static int write_chunk(struct wfs_xfs *x, struct xfs_inode *ip, uint64_t pos, const uint8_t *buf,
		       uint64_t len, uint64_t *done)
{
	struct xfs_mount *mp = &x->mp;
	uint32_t bl = mp->m_sb.sb_blocklog;
	uint64_t bs = mp->m_sb.sb_blocksize, end = pos + len, p;
	xfs_fileoff_t fsb = pos >> bl, endfsb = (end + bs - 1) >> bl, limit;
	bool alloced = false, unwritten = false;
	int error;

	*done = 0;
	error = alloc_holes(ip, fsb, endfsb, &limit, &alloced);
	if (error)
		return error;
	if (limit < endfsb) {		/* ENOSPC part way */
		if ((uint64_t)limit << bl <= pos)
			return -WFS_ENOSPC;
		end = (uint64_t)limit << bl;
		endfsb = limit;
	}
	for (p = pos; p < end;) {
		struct xfs_bmbt_irec imap;
		uint64_t ext_end, seg_end, skip, dev;

		error = read_map(ip, p >> bl, endfsb - (p >> bl), &imap);
		if (error)
			return error;
		if (imap.br_startblock == HOLESTARTBLOCK || imap.br_startblock == DELAYSTARTBLOCK)
			return -WFS_EUCLEAN;
		ext_end = (uint64_t)(imap.br_startoff + imap.br_blockcount) << bl;
		seg_end = ext_end < end ? ext_end : end;
		skip = p - ((uint64_t)imap.br_startoff << bl);
		error = dev_off_of(x, &imap, skip, &dev);
		if (error)
			return error;
		if (imap.br_state == XFS_EXT_UNWRITTEN) {
			uint64_t head = p & (bs - 1), tail;

			unwritten = true;
			/* the blocks become readable as a whole: zero what we do not write */
			if (head && (error = ddev_zero(x, dev - head, head)))
				return error;
			tail = seg_end & (bs - 1) ? bs - (seg_end & (bs - 1)) : 0;
			if (tail && seg_end == end && (error = ddev_zero(x, dev + (seg_end - p), tail)))
				return error;
		}
		error = ddev_write(x, dev, buf + (p - pos), seg_end - p);
		if (error)
			return error;
		p = seg_end;
	}
	if (alloced || unwritten) {
		if (wfs_xfs_blk_flush(&x->blk, WFS_DEV_DATA))
			return -WFS_EIO;
		error = convert_range(ip, fsb, endfsb);
		if (error)
			return error;
	}
	*done = end - pos;
	{							/* step 4: size (when it grows) and mtime/ctime */
		struct xfs_trans *tp;

		error = xfs_trans_alloc(mp, &M_RES(mp)->tr_fsyncts, 0, 0, 0, &tp);
		if (error)
			return wfs_xfs_err(error);
		xfs_ilock(ip, XFS_ILOCK_EXCL);
		xfs_trans_ijoin(tp, ip, 0);
		if (end > (uint64_t)i_size_read(VFS_I(ip))) {
			set_size(tp, ip, end);
		} else {		/* an overwrite still changes mtime/ctime */
			xfs_trans_ichgtime(tp, ip, XFS_ICHGTIME_MOD | XFS_ICHGTIME_CHG);
			xfs_trans_log_inode(tp, ip, XFS_ILOG_CORE);
		}
		error = xfs_trans_commit(tp);
		xfs_iunlock(ip, XFS_ILOCK_EXCL);
		if (error)
			return wfs_xfs_err(error);
	}
	return 0;
}

wfs_err_t wfs_xfs_op_write(struct wfs_xfs *x, wfs_ino_t ino, uint64_t off, const void *buf,
			   size_t len, size_t *done)
{
	struct xfs_inode *ip;
	uint64_t pos = off, left = len;
	int error;

	*done = 0;
	error = d_check(x);
	if (error)
		return error;
	error = d_get(x, ino, &ip);
	if (error)
		return error;
	if (S_ISDIR(VFS_I(ip)->i_mode)) {
		error = -WFS_EISDIR;
	} else if (!S_ISREG(VFS_I(ip)->i_mode)) {
		error = -WFS_EINVAL;
	} else if (ip->i_diflags & XFS_DIFLAG_REALTIME) {
		error = -WFS_EOPNOTSUPP;
	} else if (off > (uint64_t)INT64_MAX || len > (uint64_t)INT64_MAX - off) {
		error = -WFS_EFBIG;
	}
	if (error || !len)
		goto out;
	if (xfs_is_reflink_inode(ip)) {	/* shared blocks need copy-on-write (M4) */
		error = -WFS_EOPNOTSUPP;
		goto out;
	}
	xfs_ilock(ip, XFS_IOLOCK_EXCL);
	while (left) {
		uint64_t n = left < WRITE_CHUNK ? left : WRITE_CHUNK, got;

		error = write_chunk(x, ip, pos, (const uint8_t *)buf + (pos - off), n, &got);
		pos += got; left -= got;
		if (error || got < n)
			break;
	}
	xfs_iunlock(ip, XFS_IOLOCK_EXCL);
	*done = (size_t)(pos - off);
	if (*done)		/* partial progress is a success; the next write reports ENOSPC */
		error = 0;
out:
	xfs_irele(ip);
	return error;
}

/* Truncate or extend to newsize. Caller holds a reference; not the locks. */
int wfs_xfs_truncate(struct wfs_xfs *x, struct xfs_inode *ip, uint64_t newsize)
{
	struct xfs_mount *mp = &x->mp;
	struct xfs_trans *tp;
	uint64_t old, bs = mp->m_sb.sb_blocksize;
	int error;

	if (!S_ISREG(VFS_I(ip)->i_mode))
		return S_ISDIR(VFS_I(ip)->i_mode) ? -WFS_EISDIR : -WFS_EINVAL;
	if (ip->i_diflags & XFS_DIFLAG_REALTIME)
		return -WFS_EOPNOTSUPP;
	if (newsize > (uint64_t)INT64_MAX)
		return -WFS_EFBIG;
	xfs_ilock(ip, XFS_IOLOCK_EXCL);
	old = (uint64_t)i_size_read(VFS_I(ip));
	if (newsize < old && xfs_is_reflink_inode(ip)) {	/* freeing shared blocks (M4) */
		error = -WFS_EOPNOTSUPP;
		goto out;
	}
	if (newsize < old && (newsize & (bs - 1))) {
		/* zero the tail of the new last block so later growth reads zeros */
		struct xfs_bmbt_irec imap;
		xfs_fileoff_t fsb = newsize >> mp->m_sb.sb_blocklog;

		error = read_map(ip, fsb, 1, &imap);
		if (!error && imap.br_startblock != HOLESTARTBLOCK && imap.br_state != XFS_EXT_UNWRITTEN) {
			uint64_t dev, tailoff = newsize & (bs - 1);

			error = dev_off_of(x, &imap, tailoff, &dev);
			if (!error)
				error = ddev_zero(x, dev, bs - tailoff);
		}
		if (error)
			goto out;
	}
	error = xfs_trans_alloc(mp, &M_RES(mp)->tr_itruncate, 0, 0, 0, &tp);
	if (error) {
		error = wfs_xfs_err(error);
		goto out;
	}
	xfs_ilock(ip, XFS_ILOCK_EXCL);
	xfs_trans_ijoin(tp, ip, 0);
	if (newsize < old) {
		ip->i_disk_size = newsize;
		error = xfs_itruncate_extents(&tp, ip, XFS_DATA_FORK, newsize);
		if (error) {
			xfs_trans_cancel(tp);
			xfs_iunlock(ip, XFS_ILOCK_EXCL);
			error = wfs_xfs_err(error);
			goto out;
		}
	}
	set_size(tp, ip, newsize);
	error = wfs_xfs_err(xfs_trans_commit(tp));
	xfs_iunlock(ip, XFS_ILOCK_EXCL);
out:
	xfs_iunlock(ip, XFS_IOLOCK_EXCL);
	return error;
}

wfs_err_t wfs_xfs_op_fsync(struct wfs_xfs *x, wfs_ino_t ino, int datasync)
{
	struct xfs_inode *ip;
	int error = 0;

	(void)datasync;
	if (!x->rw)
		return 0;
	if (wfs_xfs_is_shutdown(&x->mp))
		return -WFS_EIO;
	error = d_get(x, ino, &ip);
	if (error)
		return error;
	if (wfs_xfs_blk_flush(&x->blk, WFS_DEV_DATA))
		error = -WFS_EIO;
	if (!error)
		error = wfs_xfs_err(xfs_log_force_inode(ip));
	if (!error && wfs_xfs_blk_flush(&x->blk, WFS_DEV_DATA))
		error = -WFS_EIO;
	xfs_irele(ip);
	return error;
}

/* sync: log forced, every metadata buffer written back (a clean log), device flushed. */
wfs_err_t wfs_xfs_op_sync(struct wfs_xfs *x)
{
	struct xfs_mount *mp = &x->mp;

	if (!x->rw)
		return 0;
	if (wfs_xfs_is_shutdown(mp))
		return -WFS_EIO;
	xfs_log_force(mp, XFS_LOG_SYNC);
	xfs_ail_push_all_sync(mp->m_ail);
	if (wfs_xfs_blk_flush(&x->blk, WFS_DEV_DATA))
		return -WFS_EIO;
	return wfs_xfs_is_shutdown(mp) ? -WFS_EIO : 0;
}
