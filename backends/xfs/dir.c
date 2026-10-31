// SPDX-License-Identifier: GPL-2.0-only
/*
 * Directory lookup and readdir (design 8.3, 8.4).
 *
 * lookup uses xfs_dir_lookup() as is. readdir drives the kernel's
 * xfs_readdir() (xfs_dir2_readdir.c, built with the cookie-mask patch, see
 * mk/dir.mk): the kernel code sets ctx->pos to the dataptr of the entry it is
 * about to emit and the shim's dir_emit() (shim/xfs_inode.h) drops "." and
 * ".." and calls rd_actor() below. The ABI cookie after an entry is
 * dataptr + 1; resuming with cookie c emits every entry whose dataptr >= c.
 *
 * Errors of an entry's inode (unallocated, out of range, bad dinode) are
 * filesystem corruption here, not a stale handle: the number came from disk.
 */
#include "icache.h"
#include "xfs_da_format.h"
#include "xfs_da_btree.h"
#include "xfs_dir2.h"
#include "xfs_dir2_priv.h"	/* xfs_readdir */
#include "xfs_types.h"

#define WFS_NAME_MAX	255	/* MAXNAMELEN - 1 */

/* iget error for an inode number read from a directory entry */
static int entry_iget_err(int error)
{
	if (error == -EINVAL || error == -ENOENT)
		return -WFS_EUCLEAN;
	return wfs_xfs_err(error);
}

/* iget error for an inode number the host supplied */
static int host_iget_err(int error)
{
	if (error == -EINVAL || error == -ENOENT)
		return -WFS_ESTALE;
	return wfs_xfs_err(error);
}

/* Get the directory the host named; -ENOTDIR if it is not one. */
static int get_dir(struct wfs_xfs *x, wfs_ino_t dir, struct xfs_inode **dpp)
{
	struct xfs_inode *dp;
	xfs_ino_t xino;
	int error;

	error = wfs_xfs_ino_from_wfs(&x->mp, dir, &xino);
	if (error)
		return error;
	error = wfs_xfs_iget(x, xino, &dp);
	if (error)
		return host_iget_err(error);
	if (!S_ISDIR(VFS_I(dp)->i_mode)) {
		xfs_irele(dp);
		return -WFS_ENOTDIR;
	}
	*dpp = dp;
	return 0;
}

wfs_err_t wfs_xfs_op_lookup(struct wfs_xfs *x, wfs_ino_t dir, const char *name, uint32_t len,
			    wfs_ino_t *out, wfs_stat *st)
{
	struct xfs_mount *mp = &x->mp;
	struct xfs_inode *dp, *ip;
	struct xfs_name xname;
	xfs_ino_t ino;
	int error;

	if (len == 0)
		return -WFS_EINVAL;
	if (len > WFS_NAME_MAX)
		return -WFS_ENAMETOOLONG;
	error = get_dir(x, dir, &dp);
	if (error)
		return error;

	xname.name = (const unsigned char *)name;
	xname.len = (int)len;
	xname.type = XFS_DIR3_FT_UNKNOWN;
	error = xfs_dir_lookup(NULL, dp, &xname, &ino, NULL);
	xfs_irele(dp);
	if (error)
		return wfs_xfs_err(error);
	if (!xfs_verify_dir_ino(mp, ino))
		return -WFS_EUCLEAN;

	error = wfs_xfs_iget(x, ino, &ip);
	if (error)
		return entry_iget_err(error);
	error = wfs_xfs_fill_stat(ip, st);
	if (!error) {
		wfs_xfs_ilookup_ref(ip);
		*out = st->ino;
	}
	xfs_irele(ip);
	return error;
}

struct rd {
	struct dir_context	dc;	/* first: the actor gets &dc */
	struct wfs_xfs		*x;
	wfs_dirent_cb		cb;
	void			*cb_ctx;
	int			err;
};

static uint32_t dtype_to_ftype(unsigned int dtype)
{
	switch (dtype) {
	case DT_REG:	return WFS_FT_REG;
	case DT_DIR:	return WFS_FT_DIR;
	case DT_LNK:	return WFS_FT_LNK;
	case DT_CHR:	return WFS_FT_CHR;
	case DT_BLK:	return WFS_FT_BLK;
	case DT_FIFO:	return WFS_FT_FIFO;
	case DT_SOCK:	return WFS_FT_SOCK;
	default:	return 0;
	}
}

/* Returns true to continue, false to stop (rd->err says why; 0 = host stop). */
static bool rd_actor(struct dir_context *dc, const char *name, int namelen, loff_t pos,
		     u64 ino, unsigned int dtype)
{
	struct rd *rd = (struct rd *)dc;
	struct xfs_mount *mp = &rd->x->mp;
	wfs_dirent ent;
	uint32_t ftype;

	if (dtype == DT_WHT)
		return true;
	if (!xfs_verify_dir_ino(mp, ino)) {
		rd->err = -WFS_EUCLEAN;
		return false;
	}
	ftype = dtype_to_ftype(dtype);
	if (!ftype) {	/* no ftype feature, or an unknown value: ask the inode */
		struct xfs_inode *ip;
		wfs_stat st;
		int error = wfs_xfs_iget(rd->x, ino, &ip);

		if (error) {
			rd->err = entry_iget_err(error);
			return false;
		}
		error = wfs_xfs_fill_stat(ip, &st);
		xfs_irele(ip);
		if (error) {
			rd->err = error;
			return false;
		}
		ftype = st.ftype;
	}

	ent.next_cookie = (uint64_t)pos + 1;
	ent.ino = wfs_xfs_ino_to_wfs(mp, ino);
	ent.ftype = ftype;
	ent.name_len = (uint32_t)namelen;
	ent.name = name;
	if (rd->cb(rd->cb_ctx, &ent)) {
		rd->err = 1;	/* host stop: not an error */
		return false;
	}
	return true;
}

wfs_err_t wfs_xfs_op_readdir(struct wfs_xfs *x, wfs_ino_t dir, uint64_t cookie,
			     wfs_dirent_cb cb, void *ctx)
{
	struct xfs_inode *dp;
	struct rd rd;
	int error;

	/* cookies are dataptr + 1 <= 2^32; anything above is past the end */
	if (cookie > (uint64_t)XFS_DIR2_MAX_DATAPTR + 1)
		return 0;
	error = get_dir(x, dir, &dp);
	if (error)
		return error;

	memset(&rd, 0, sizeof(rd));
	rd.dc.actor = rd_actor;
	rd.dc.pos = (loff_t)cookie;
	rd.x = x;
	rd.cb = cb;
	rd.cb_ctx = ctx;
	error = xfs_readdir(NULL, dp, &rd.dc, 0);
	xfs_irele(dp);
	if (rd.err > 0)		/* host stop */
		return 0;
	if (rd.err)
		return rd.err;
	return wfs_xfs_err(error);
}
