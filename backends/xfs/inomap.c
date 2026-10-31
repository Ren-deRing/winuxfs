// SPDX-License-Identifier: GPL-2.0-only
/*
 * XFS inode number <-> ABI inode number (design 8.1), wfs_stat filling
 * (design 8.2) and the getattr/forget backend ops built on the inode cache.
 */
#include "icache.h"
#include "xfs_inode_buf.h"

wfs_ino_t wfs_xfs_ino_to_wfs(const struct xfs_mount *mp, xfs_ino_t ino)
{
	return ino == mp->m_sb.sb_rootino ? WFS_ROOT_INO : ino;
}

/*
 * ino 1 can never be a real XFS inode (xfs_verify_ino rejects agino below
 * agino_min), so 1 is free to stand for the root. The real root number is
 * never handed out, hence a host passing it is confused: -ESTALE.
 */
int wfs_xfs_ino_from_wfs(const struct xfs_mount *mp, wfs_ino_t w, xfs_ino_t *out)
{
	if (w == WFS_ROOT_INO) {
		*out = mp->m_sb.sb_rootino;
		return 0;
	}
	if (w == 0 || w == mp->m_sb.sb_rootino || w > XFS_MAXINUMBER)
		return -WFS_ESTALE;
	*out = w;
	return 0;
}

int wfs_xfs_fill_stat(struct xfs_inode *ip, wfs_stat *st)
{
	struct xfs_mount *mp = ip->i_mount;
	struct inode *inode = VFS_I(ip);
	uint32_t ft;

	switch (inode->i_mode & S_IFMT) {
	case S_IFREG:	ft = WFS_FT_REG; break;
	case S_IFDIR:	ft = WFS_FT_DIR; break;
	case S_IFLNK:	ft = WFS_FT_LNK; break;
	case S_IFCHR:	ft = WFS_FT_CHR; break;
	case S_IFBLK:	ft = WFS_FT_BLK; break;
	case S_IFIFO:	ft = WFS_FT_FIFO; break;
	case S_IFSOCK:	ft = WFS_FT_SOCK; break;
	default:
		return -WFS_EUCLEAN;
	}

	memset(st, 0, sizeof(*st));
	st->ino = wfs_xfs_ino_to_wfs(mp, I_INO(ip));
	st->ftype = ft;
	st->mode = inode->i_mode & 07777;
	st->uid = i_uid_read(inode);
	st->gid = i_gid_read(inode);
	st->nlink = inode->i_nlink;
	st->size = (uint64_t)inode->i_size;
	/* xfs_vn_getattr: (nblocks + delayed) in filesystem blocks -> 512 B */
	st->blocks = XFS_FSB_TO_BB(mp, ip->i_nblocks + ip->i_delayed_blks);
	if (ft == WFS_FT_CHR || ft == WFS_FT_BLK) {
		st->rdev_major = inode->i_rdev >> MINORBITS;
		st->rdev_minor = inode->i_rdev & ((1u << MINORBITS) - 1);
	}
	st->atime.sec = inode->i_atime_.tv_sec;
	st->atime.nsec = (uint32_t)inode->i_atime_.tv_nsec;
	st->mtime.sec = inode->i_mtime_.tv_sec;
	st->mtime.nsec = (uint32_t)inode->i_mtime_.tv_nsec;
	st->ctime.sec = inode->i_ctime_.tv_sec;
	st->ctime.nsec = (uint32_t)inode->i_ctime_.tv_nsec;
	st->btime.sec = ip->i_crtime.tv_sec;
	st->btime.nsec = (uint32_t)ip->i_crtime.tv_nsec;
	return 0;
}

/*
 * iget error -> ABI error, by where the inode number came from. -EINVAL and
 * -ENOENT mean "not an allocated inode": for a number the HOST supplied that
 * is a stale handle; for a number read from a directory entry the filesystem
 * is inconsistent. Everything else (corruption, I/O, ENOMEM) maps the same.
 */
int wfs_xfs_host_iget_err(int error)
{
	if (error == -EINVAL || error == -ENOENT)
		return -WFS_ESTALE;
	return wfs_xfs_err(error);
}

int wfs_xfs_dirent_iget_err(int error)
{
	if (error == -EINVAL || error == -ENOENT)
		return -WFS_EUCLEAN;
	return wfs_xfs_err(error);
}

wfs_err_t wfs_xfs_op_getattr(struct wfs_xfs *x, wfs_ino_t ino, wfs_stat *st)
{
	struct xfs_inode *ip;
	xfs_ino_t xino;
	int error;

	error = wfs_xfs_ino_from_wfs(&x->mp, ino, &xino);
	if (error)
		return error;
	error = wfs_xfs_iget(x, xino, &ip);
	if (error)
		return wfs_xfs_host_iget_err(error);
	error = wfs_xfs_fill_stat(ip, st);
	xfs_irele(ip);
	return error;
}

void wfs_xfs_op_forget(struct wfs_xfs *x, wfs_ino_t ino, uint64_t nlookup)
{
	xfs_ino_t xino;

	if (ino == WFS_ROOT_INO)	/* the root is pinned, forget is advisory */
		return;
	if (wfs_xfs_ino_from_wfs(&x->mp, ino, &xino))
		return;
	wfs_xfs_op_forget_w(x, xino, nlookup);
}
