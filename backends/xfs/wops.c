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
#include "xfs_symlink.h"
#include <stdlib.h>
#include <string.h>

#define I_INO_OF(ip)	I_INO(ip)

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

/*
 * A reflink inode may share blocks with others. Freeing them needs the refcount
 * decrease path and writing them needs copy-on-write, neither of which is tested
 * here (M4): such files are refused instead of risking another file's data.
 */
static bool w_last_link_of_shared(struct xfs_inode *ip)
{
	return xfs_is_reflink_inode(ip) && VFS_I(ip)->i_nlink <= 1;
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
	if (w_last_link_of_shared(ip)) {
		error = -WFS_EOPNOTSUPP;
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
	if ((mask & WFS_SET_SIZE) && (!S_ISREG(inode->i_mode) || st->size != (uint64_t)i_size_read(inode))) {
		error = wfs_xfs_truncate(x, ip, st->size);
		if (error)
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
	error = wfs_xfs_err(xfs_trans_commit(tp));	/* the commit drops the ILOCK joined by alloc_ichange */
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

static uint8_t w_ftype_of(umode_t mode)
{
	switch (mode & S_IFMT) {
	case S_IFDIR:	return XFS_DIR3_FT_DIR;
	case S_IFLNK:	return XFS_DIR3_FT_SYMLINK;
	case S_IFREG:	return XFS_DIR3_FT_REG_FILE;
	default:	return XFS_DIR3_FT_UNKNOWN;
	}
}

/* Lock two directories (once if the same), in address order. */
static void w_lock2(struct xfs_inode *a, struct xfs_inode *b)
{
	if (a == b) {
		inode_lock(VFS_I(a));
		return;
	}
	if (a > b) {
		struct xfs_inode *t = a; a = b; b = t;
	}
	inode_lock(VFS_I(a));
	inode_lock(VFS_I(b));
}

static void w_unlock2(struct xfs_inode *a, struct xfs_inode *b)
{
	inode_unlock(VFS_I(a));
	if (a != b)
		inode_unlock(VFS_I(b));
}

wfs_err_t wfs_xfs_op_link(struct wfs_xfs *x, wfs_ino_t ino, wfs_ino_t ndir, const char *name,
			  uint32_t len)
{
	struct xfs_inode *ip, *dp;
	struct xfs_name xn;
	int error;

	error = w_check_rw(x);
	if (!error)
		error = w_name(name, len, &xn);
	if (!error && dotname(name, len))
		error = -WFS_EEXIST;
	if (error)
		return error;
	error = w_get(x, ino, &ip);
	if (error)
		return error;
	error = w_get(x, ndir, &dp);
	if (error) {
		xfs_irele(ip);
		return error;
	}
	if (!S_ISDIR(VFS_I(dp)->i_mode)) {
		error = -WFS_ENOTDIR;
	} else if (S_ISDIR(VFS_I(ip)->i_mode)) {
		error = -WFS_EPERM;
	} else if (VFS_I(ip)->i_nlink == 0) {
		error = -WFS_ENOENT;
	} else {
		xn.type = w_ftype_of(VFS_I(ip)->i_mode);
		inode_lock(VFS_I(dp));
		error = xfs_dir_lookup(NULL, dp, &xn, &(xfs_ino_t){ 0 }, NULL);
		if (error == -ENOENT)
			error = wfs_xfs_err(xfs_link(dp, ip, &xn));
		else
			error = error ? wfs_xfs_err(error) : -WFS_EEXIST;
		inode_unlock(VFS_I(dp));
	}
	xfs_irele(dp);
	xfs_irele(ip);
	return error;
}

wfs_err_t wfs_xfs_op_symlink(struct wfs_xfs *x, wfs_ino_t dir, const char *name, uint32_t len,
			     const char *target, uint32_t tlen, uint32_t uid, uint32_t gid,
			     wfs_ino_t *out, wfs_stat *st)
{
	struct xfs_inode *dp, *ip = NULL;
	struct xfs_name xn;
	char *path;
	int error;

	error = w_check_rw(x);
	if (!error)
		error = w_name(name, len, &xn);
	if (!error && dotname(name, len))
		error = -WFS_EEXIST;
	if (!error && tlen == 0)
		error = -WFS_EINVAL;
	if (!error && tlen >= XFS_SYMLINK_MAXLEN)	/* xfs_symlink: pathlen >= MAXLEN */
		error = -WFS_ENAMETOOLONG;
	if (error)
		return error;
	path = malloc((size_t)tlen + 1);
	if (!path)
		return -WFS_ENOMEM;
	memcpy(path, target, tlen);
	path[tlen] = 0;
	error = w_get(x, dir, &dp);
	if (error)
		goto out_path;
	if (!S_ISDIR(VFS_I(dp)->i_mode)) {
		xfs_irele(dp);
		error = -WFS_ENOTDIR;
		goto out_path;
	}
	xn.type = XFS_DIR3_FT_SYMLINK;
	wfs_set_current_fsids(uid, gid);
	inode_lock(VFS_I(dp));
	error = xfs_dir_lookup(NULL, dp, &xn, &(xfs_ino_t){ 0 }, NULL);
	if (error == -ENOENT)
		error = xfs_symlink((const struct mnt_idmap *)&w_idmap_dummy, dp, &xn, path, 0777, &ip);
	else if (!error)
		error = -EEXIST;
	inode_unlock(VFS_I(dp));
	xfs_irele(dp);
	if (error) {
		error = wfs_xfs_err(error);
		goto out_path;
	}
	error = wfs_xfs_fill_stat(ip, st);
	if (!error) {
		wfs_xfs_ilookup_ref(ip);
		*out = st->ino;
	}
	xfs_irele(ip);
out_path:
	free(path);
	return error;
}

/* Is "anc" an ancestor of (or equal to) directory "d"? Walks ".." to the root. */
static int w_is_ancestor(struct wfs_xfs *x, struct xfs_inode *anc, struct xfs_inode *d, bool *yes)
{
	struct xfs_mount *mp = &x->mp;
	struct xfs_name dd = { .name = (const unsigned char *)"..", .len = 2, .type = XFS_DIR3_FT_UNKNOWN };
	struct xfs_inode *cur = d;
	int depth, error = 0;

	*yes = false;
	for (depth = 0; depth < 65536; depth++) {
		xfs_ino_t parent;
		struct xfs_inode *p;

		if (cur == anc || I_INO_OF(cur) == I_INO_OF(anc)) {
			*yes = true;
			break;
		}
		if (I_INO_OF(cur) == mp->m_sb.sb_rootino)
			break;
		error = xfs_dir_lookup(NULL, cur, &dd, &parent, NULL);
		if (error) {
			error = wfs_xfs_err(error);
			break;
		}
		if (parent == I_INO_OF(cur)) {
			break;
		}
		error = wfs_xfs_iget(x, parent, &p);
		if (cur != d)
			xfs_irele(cur);
		if (error) {
			error = wfs_xfs_err(error);
			cur = d;
			break;
		}
		cur = p;
	}
	if (cur != d)
		xfs_irele(cur);
	return error;
}

wfs_err_t wfs_xfs_op_rename(struct wfs_xfs *x, wfs_ino_t odir, const char *oname, uint32_t olen,
			    wfs_ino_t ndir, const char *nname, uint32_t nlen, uint32_t flags)
{
	struct xfs_inode *sdp, *tdp, *ip = NULL, *tip = NULL;
	struct xfs_name sn, tn;
	bool yes;
	int error;

	if (flags & ~(uint32_t)WFS_RENAME_NOREPLACE)
		return -WFS_EINVAL;
	error = w_check_rw(x);
	if (!error)
		error = w_name(oname, olen, &sn);
	if (!error)
		error = w_name(nname, nlen, &tn);
	if (!error && (dotname(oname, olen) || dotname(nname, nlen)))
		error = -WFS_EINVAL;
	if (error)
		return error;
	down_write(&x->rename_lock);
	error = w_get(x, odir, &sdp);
	if (error)
		goto out_lock;
	error = w_get(x, ndir, &tdp);
	if (error) {
		xfs_irele(sdp);
		goto out_lock;
	}
	if (!S_ISDIR(VFS_I(sdp)->i_mode) || !S_ISDIR(VFS_I(tdp)->i_mode)) {
		error = -WFS_ENOTDIR;
		goto out_dirs;
	}
	w_lock2(sdp, tdp);
	error = xfs_lookup(sdp, &sn, &ip, NULL);
	if (error) {
		error = wfs_xfs_err(error);
		goto out_unlock;
	}
	error = xfs_lookup(tdp, &tn, &tip, NULL);
	if (error == -ENOENT) {
		tip = NULL;
		error = 0;
	} else if (error) {
		error = wfs_xfs_err(error);
		goto out_ip;
	}
	if (tip == ip) {			/* same inode: nothing to do (POSIX) */
		error = (flags & WFS_RENAME_NOREPLACE) ? -WFS_EEXIST : 0;
		goto out_tip;
	}
	if (tip && (flags & WFS_RENAME_NOREPLACE)) {
		error = -WFS_EEXIST;
		goto out_tip;
	}
	if (tip && S_ISDIR(VFS_I(ip)->i_mode) != S_ISDIR(VFS_I(tip)->i_mode)) {
		error = S_ISDIR(VFS_I(ip)->i_mode) ? -WFS_ENOTDIR : -WFS_EISDIR;
		goto out_tip;
	}
	if (S_ISDIR(VFS_I(ip)->i_mode) && sdp != tdp) {
		error = w_is_ancestor(x, ip, tdp, &yes);
		if (!error && yes)
			error = -WFS_EINVAL;
		if (error)
			goto out_tip;
	}
	sn.type = tn.type = w_ftype_of(VFS_I(ip)->i_mode);
	if (tip && w_last_link_of_shared(tip)) {
		error = -WFS_EOPNOTSUPP;
		goto out_tip;
	}
	error = xfs_rename((const struct mnt_idmap *)&w_idmap_dummy, sdp, &sn, ip, tdp, &tn, tip, 0);
	if (error == -EEXIST && tip && S_ISDIR(VFS_I(tip)->i_mode))
		error = -ENOTEMPTY;		/* xfs_rename: non-empty target directory */
	error = wfs_xfs_err(error);
	if (!error && tip)
		w_maybe_inactive(tip);
out_tip:
	if (tip)
		xfs_irele(tip);
out_ip:
	xfs_irele(ip);
out_unlock:
	w_unlock2(sdp, tdp);
out_dirs:
	xfs_irele(tdp);
	xfs_irele(sdp);
out_lock:
	up_write(&x->rename_lock);
	return error;
}
