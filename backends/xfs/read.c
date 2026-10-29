// SPDX-License-Identifier: GPL-2.0-only
/*
 * File read and readlink (design 8.5, 8.6).
 *
 * File data is not cached: the extent map is walked with xfs_bmapi_read, one
 * extent at a time, and physically adjacent written extents are merged into
 * a single host blk_read straight into the caller's buffer. Holes and
 * unwritten extents read as zeros; a delayed-allocation extent cannot exist
 * on a clean read-only disk and is reported as corruption. The host accepts
 * any byte offset and length (it aligns), so no bounce buffer is needed.
 */
#include "icache.h"
#include "xfs_bit.h"
#include "xfs_bmap.h"
#include "xfs_symlink_remote.h"
#include "xfs_rtgroup.h"

/* Largest single host read / zero fill unit (design 8.6: INT_MAX). */
#define READ_CHUNK	((size_t)0x7fffffff)

/* iget error for an inode number supplied by the host (see inomap.c). */
static int host_iget_err(int error)
{
	if (error == -EINVAL || error == -ENOENT)
		return -WFS_ESTALE;
	return wfs_xfs_err(error);
}

static int get_inode(struct wfs_xfs *x, wfs_ino_t ino, struct xfs_inode **ipp)
{
	xfs_ino_t xino;
	int error = wfs_xfs_ino_from_wfs(&x->mp, ino, &xino);

	if (error)
		return error;
	error = wfs_xfs_iget(x, xino, ipp);
	return error ? host_iget_err(error) : 0;
}

/* A run of device bytes waiting to be read into dst. */
struct run {
	int		role;
	uint64_t	dev_off;
	size_t		len;
	uint8_t		*dst;
};

static int flush_run(struct wfs_xfs *x, struct run *r, size_t *done)
{
	while (r->len) {
		size_t n = r->len < READ_CHUNK ? r->len : READ_CHUNK;
		int err = wfs_xfs_blk_read(&x->blk, r->role, r->dev_off, r->dst, n);

		if (err)	/* an in-range request the host calls invalid is its problem too */
			return err == -WFS_EINVAL ? -WFS_EUCLEAN : -WFS_EIO;
		r->dev_off += n;
		r->dst += n;
		r->len -= n;
		*done += n;
	}
	return 0;
}

/* Map [imap + skip, +n) to a device role and byte offset, validated. */
static int extent_dev_off(struct wfs_xfs *x, struct xfs_inode *ip,
		const struct xfs_bmbt_irec *imap, uint64_t skip, uint64_t n,
		int *role, uint64_t *dev_off)
{
	struct xfs_mount *mp = &x->mp;
	uint64_t off, size;

	if (ip->i_diflags & XFS_DIFLAG_REALTIME) {
		if (!x->blk.dev[WFS_DEV_RT] && !x->blk.read)
			return -WFS_EUCLEAN;
		if (!xfs_verify_rtbext(mp, imap->br_startblock, imap->br_blockcount))
			return -WFS_EUCLEAN;
		*role = WFS_DEV_RT;
		off = (uint64_t)BBTOB(xfs_rtb_to_daddr(mp, imap->br_startblock));
	} else {
		if (!xfs_verify_fsbext(mp, imap->br_startblock, imap->br_blockcount))
			return -WFS_EUCLEAN;
		*role = WFS_DEV_DATA;
		off = (uint64_t)BBTOB(XFS_FSB_TO_DADDR(mp, imap->br_startblock));
	}
	if (skip > UINT64_MAX - off)
		return -WFS_EUCLEAN;
	off += skip;
	size = x->blk.size_bytes[*role];
	if (size && (off > size || n > size - off))
		return -WFS_EUCLEAN;	/* extent beyond the device */
	*dev_off = off;
	return 0;
}

wfs_err_t wfs_xfs_op_read(struct wfs_xfs *x, wfs_ino_t ino, uint64_t off, void *buf,
			  size_t len, size_t *done)
{
	struct xfs_inode *ip;
	struct xfs_mount *mp = &x->mp;
	struct run run = { 0 };
	uint8_t *out = buf;
	uint64_t size, pos, end;
	uint32_t lock_mode;
	size_t got = 0;
	int err;

	*done = 0;
	err = get_inode(x, ino, &ip);
	if (err)
		return err;
	switch (VFS_I(ip)->i_mode & S_IFMT) {
	case S_IFREG:
		break;
	case S_IFDIR:
		xfs_irele(ip);
		return -WFS_EISDIR;
	default:
		xfs_irele(ip);
		return -WFS_EINVAL;
	}

	size = (uint64_t)VFS_I(ip)->i_size;
	if (off >= size || len == 0) {
		xfs_irele(ip);
		return 0;
	}
	if (len > size - off)
		len = (size_t)(size - off);
	pos = off;
	end = off + len;

	lock_mode = xfs_ilock_data_map_shared(ip);
	while (pos < end) {
		struct xfs_bmbt_irec imap;
		xfs_fileoff_t fsb = pos >> mp->m_sb.sb_blocklog;
		xfs_filblks_t cnt = ((end + mp->m_sb.sb_blocksize - 1) >> mp->m_sb.sb_blocklog) - fsb;
		uint64_t ext_end, skip, n;
		int nimaps = 1;

		err = xfs_bmapi_read(ip, fsb, cnt, &imap, &nimaps, 0);
		if (err) {
			err = wfs_xfs_err(err);
			break;
		}
		if (nimaps != 1 || imap.br_startoff != fsb || imap.br_blockcount == 0) {
			err = -WFS_EUCLEAN;
			break;
		}
		ext_end = (uint64_t)(imap.br_startoff + imap.br_blockcount) << mp->m_sb.sb_blocklog;
		n = (ext_end < end ? ext_end : end) - pos;
		skip = pos - ((uint64_t)imap.br_startoff << mp->m_sb.sb_blocklog);
		if (n > READ_CHUNK)
			n = READ_CHUNK;

		if (imap.br_startblock == DELAYSTARTBLOCK) {
			err = -WFS_EUCLEAN;
			break;
		}
		if (imap.br_startblock == HOLESTARTBLOCK || imap.br_state == XFS_EXT_UNWRITTEN) {
			err = flush_run(x, &run, &got);
			if (err)
				break;
			memset(out + got, 0, n);
			got += n;
		} else {
			int role;
			uint64_t dev_off;

			err = extent_dev_off(x, ip, &imap, skip, n, &role, &dev_off);
			if (err)
				break;
			if (run.len && run.role == role && run.dev_off + run.len == dev_off &&
			    run.len + n <= READ_CHUNK) {
				run.len += n;
			} else {
				err = flush_run(x, &run, &got);
				if (err)
					break;
				run = (struct run){ role, dev_off, n, out + got };
			}
		}
		pos += n;
	}
	/* the pending run is valid even if a later extent failed */
	{
		int ferr = flush_run(x, &run, &got);

		if (!err)
			err = ferr;
	}
	xfs_iunlock(ip, lock_mode);
	xfs_irele(ip);
	*done = got;
	return err;
}

wfs_err_t wfs_xfs_op_readlink(struct wfs_xfs *x, wfs_ino_t ino, char *buf, size_t cap,
			      size_t *len)
{
	struct xfs_inode *ip;
	struct xfs_mount *mp = &x->mp;
	char link[XFS_SYMLINK_MAXLEN + 1];
	uint32_t lock_mode;
	int64_t size;
	int err;

	*len = 0;
	err = get_inode(x, ino, &ip);
	if (err)
		return err;
	if ((VFS_I(ip)->i_mode & S_IFMT) != S_IFLNK) {
		xfs_irele(ip);
		return -WFS_EINVAL;
	}
	size = ip->i_disk_size;
	if (size < 1 || size > XFS_SYMLINK_MAXLEN) {
		xfs_irele(ip);
		return -WFS_EUCLEAN;
	}

	lock_mode = xfs_ilock_data_map_shared(ip);
	if (ip->i_df.if_format == XFS_DINODE_FMT_LOCAL) {
		if (!ip->i_df.if_data || size > ip->i_df.if_bytes)
			err = -WFS_EUCLEAN;
		else
			memcpy(link, ip->i_df.if_data, (size_t)size);
	} else if (ip->i_df.if_format == XFS_DINODE_FMT_EXTENTS) {
		/*
		 * xfs_symlink_remote_read trusts the map; check that it covers
		 * the whole path with real blocks (no hole, delalloc) first.
		 */
		struct xfs_bmbt_irec map[XFS_SYMLINK_MAPS];
		int nmaps = XFS_SYMLINK_MAPS, i;
		int fsblocks = xfs_symlink_blocks(mp, (int)size);
		xfs_filblks_t total = 0;

		err = xfs_bmapi_read(ip, 0, fsblocks, map, &nmaps, 0);
		if (err) {
			err = wfs_xfs_err(err);
		} else {
			for (i = 0; i < nmaps; i++) {
				if (!xfs_bmap_is_real_extent(&map[i]) ||
				    !xfs_verify_fsbext(mp, map[i].br_startblock, map[i].br_blockcount))
					break;
				total += map[i].br_blockcount;
			}
			if (i != nmaps || total != (xfs_filblks_t)fsblocks)
				err = -WFS_EUCLEAN;
		}
		if (!err) {
			err = xfs_symlink_remote_read(ip, link);
			err = wfs_xfs_err(err);
		}
	} else {
		err = -WFS_EUCLEAN;
	}
	xfs_iunlock(ip, lock_mode);
	xfs_irele(ip);
	if (err)
		return err;
	*len = (size_t)size;
	if (cap < (size_t)size)
		return -WFS_ERANGE;
	memcpy(buf, link, (size_t)size);
	return 0;
}
