// SPDX-License-Identifier: GPL-2.0-only
/*
 * Inode-changing operations (M3a T6): create/mkdir, unlink/rmdir, setattr
 * (non-size) and the nlink=0 inactive pass. Thin wrappers over the kernel's
 * xfs_create/xfs_remove/xfs_trans_alloc_ichange; every error is mapped by
 * wfs_xfs_err(). Size changes are T7 (truncate).
 */
#include "icache.h"
#include "xfs_trans.h"
#include "xfs_inode_item.h"
#include "xfs_inode_util.h"
#include "xfs_dir2.h"

static const char w_idmap_dummy;	/* xfs_inode_init: non-NULL idmap = child of a directory */

static int w_get(struct wfs_xfs *x, wfs_ino_t ino, struct xfs_inode **ipp)
{
	xfs_ino_t xino;
	int error = wfs_xfs_ino_from_wfs(&x->mp, ino, &xino);

	if (error)
		return error;
	error = wfs_xfs_iget(x, xino, ipp);
	return error ? wfs_xfs_host_iget_err(error) : 0;
}

static int w_check_rw(struct wfs_xfs *x)
{
	if (!x->rw)
		return -WFS_EROFS;
	if (wfs_xfs_is_shutdown(&x->mp))
		return -WFS_EIO;
	return 0;
}

static int w_name(const char *name, uint32_t len, struct xfs_name *xn)
{
	if (len == 0)
		return -WFS_EINVAL;
	if (len > WFS_NAME_MAX)
		return -WFS_ENAMETOOLONG;
	xn->name = (const unsigned char *)name;
	xn->len = (int)len;
	xn->type = XFS_DIR3_FT_UNKNOWN;
	return 0;
}

static bool dotname(const char *n, uint32_t len)
{
	return (len == 1 && n[0] == '.') || (len == 2 && n[0] == '.' && n[1] == '.');
}

/* nlink==0 and nobody holds a host reference: free the inode on disk. */
static void w_maybe_inactive(struct xfs_inode *ip)
{
	if (VFS_I(ip)->i_nlink != 0 || wfs_xfs_inlookup(ip) != 0)
		return;
	if (xfs_inactive(ip) == 0)
		wfs_xfs_idead(ip);
}

wfs_err_t wfs_xfs_op_create(struct wfs_xfs *x, wfs_ino_t dir, const char *name, uint32_t len,
			    uint32_t ftype, uint32_t mode, uint32_t uid, uint32_t gid,
			    wfs_ino_t *out, wfs_stat *st)
{
	struct xfs_inode *dp, *ip = NULL;
	struct xfs_name xn;
	struct xfs_icreate_args args = { 0 };
	int error;

	if (ftype != WFS_FT_REG && ftype != WFS_FT_DIR)
		return -WFS_EOPNOTSUPP;
	error = w_check_rw(x);
	if (!error)
		error = w_name(name, len, &xn);
	if (!error && dotname(name, len))
		error = -WFS_EEXIST;
	if (error)
		return error;
	error = w_get(x, dir, &dp);
	if (error)
		return error;
	if (!S_ISDIR(VFS_I(dp)->i_mode)) {
		xfs_irele(dp);
		return -WFS_ENOTDIR;
	}
	xn.type = ftype == WFS_FT_DIR ? XFS_DIR3_FT_DIR : XFS_DIR3_FT_REG_FILE;
	args.idmap = (const struct mnt_idmap *)&w_idmap_dummy;
	args.pip = dp;
	args.mode = (ftype == WFS_FT_DIR ? S_IFDIR : S_IFREG) | (mode & 07777);
	wfs_set_current_fsids(uid, gid);
	inode_lock(VFS_I(dp));		/* like the VFS: names are checked and created under the dir lock */
	error = xfs_dir_lookup(NULL, dp, &xn, &(xfs_ino_t){ 0 }, NULL);
	if (error == -ENOENT)
		error = xfs_create(&args, &xn, &ip);
	else if (!error)
		error = -EEXIST;
	inode_unlock(VFS_I(dp));
	xfs_irele(dp);
	if (error)
		return wfs_xfs_err(error);
	error = wfs_xfs_fill_stat(ip, st);
	if (!error) {
		wfs_xfs_ilookup_ref(ip);
		*out = st->ino;
	}
	xfs_irele(ip);
	return error;
}

static wfs_err_t w_remove(struct wfs_xfs *x, wfs_ino_t dir, const char *name, uint32_t len,
			  bool want_dir)
{
	struct xfs_inode *dp, *ip = NULL;
	struct xfs_name xn;
	int error;

	error = w_check_rw(x);
	if (!error)
		error = w_name(name, len, &xn);
	if (!error && dotname(name, len))
		error = want_dir ? -WFS_EINVAL : -WFS_EISDIR;
	if (error)
		return error;
	error = w_get(x, dir, &dp);
	if (error)
		return error;
	if (!S_ISDIR(VFS_I(dp)->i_mode)) {
		error = -WFS_ENOTDIR;
		goto out_dp;
	}
	error = xfs_lookup(dp, &xn, &ip, NULL);
	if (error) {
		error = wfs_xfs_err(error);
		goto out_dp;
	}
	if (want_dir != S_ISDIR(VFS_I(ip)->i_mode)) {
		error = want_dir ? -WFS_ENOTDIR : -WFS_EISDIR;
		goto out_ip;
	}
	inode_lock(VFS_I(dp));
	error = wfs_xfs_err(xfs_remove(dp, &xn, ip));
	inode_unlock(VFS_I(dp));
	if (!error)
		w_maybe_inactive(ip);
out_ip:
	xfs_irele(ip);
out_dp:
	xfs_irele(dp);
	return error;
}

wfs_err_t wfs_xfs_op_unlink(struct wfs_xfs *x, wfs_ino_t dir, const char *name, uint32_t len)
{
	return w_remove(x, dir, name, len, false);
}

wfs_err_t wfs_xfs_op_rmdir(struct wfs_xfs *x, wfs_ino_t dir, const char *name, uint32_t len)
{
	return w_remove(x, dir, name, len, true);
}

wfs_err_t wfs_xfs_op_setattr(struct wfs_xfs *x, wfs_ino_t ino, const wfs_stat *st, uint32_t mask)
{
	struct xfs_inode *ip;
	struct xfs_trans *tp;
	struct inode *inode;
	int error;

	error = w_check_rw(x);
	if (error)
		return error;
	error = w_get(x, ino, &ip);
	if (error)
		return error;
	inode = VFS_I(ip);
	if ((mask & WFS_SET_SIZE) && st->size != (uint64_t)i_size_read(inode)) {
		error = -WFS_EOPNOTSUPP;		/* T7 */
		goto out;
	}
	mask &= ~WFS_SET_SIZE;
	if (!mask)
		goto out;
	error = xfs_trans_alloc_ichange(ip, NULL, NULL, NULL, false, &tp);
	if (error) {
		error = wfs_xfs_err(error);
		goto out;
	}
	if (mask & WFS_SET_MODE)
		inode->i_mode = (inode->i_mode & S_IFMT) | (st->mode & 07777);
	if (mask & WFS_SET_UID)
		inode->i_uid = st->uid;
	if (mask & WFS_SET_GID)
		inode->i_gid = st->gid;
	if (mask & WFS_SET_ATIME)
		inode_set_atime_to_ts(inode, (struct timespec64){ st->atime.sec, st->atime.nsec });
	if (mask & WFS_SET_MTIME)
		inode_set_mtime_to_ts(inode, (struct timespec64){ st->mtime.sec, st->mtime.nsec });
	xfs_trans_ichgtime(tp, ip, XFS_ICHGTIME_CHG);
	xfs_trans_log_inode(tp, ip, XFS_ILOG_CORE);
	error = wfs_xfs_err(xfs_trans_commit(tp));
	xfs_iunlock(ip, XFS_ILOCK_EXCL);
out:
	xfs_irele(ip);
	return error;
}

/* forget: drop host references; the last one on an unlinked inode frees it. */
void wfs_xfs_op_forget_w(struct wfs_xfs *x, xfs_ino_t xino, uint64_t n)
{
	struct xfs_inode *ip;

	if (x->rw && !wfs_xfs_is_shutdown(&x->mp) && wfs_xfs_iget(x, xino, &ip) == 0) {
		wfs_xfs_iforget(x, xino, n);
		w_maybe_inactive(ip);
		xfs_irele(ip);
		return;
	}
	wfs_xfs_iforget(x, xino, n);
}
