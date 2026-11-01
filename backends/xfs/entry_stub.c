// SPDX-License-Identifier: GPL-2.0-only
/*
 * Backend descriptor "xfs": ABI glue only. Every operation is implemented in its
 * own file (mount.c, icache.c/inomap.c: getattr+forget, dir.c: lookup+readdir,
 * read.c: read+readlink, statfs.c, wops.c: create/unlink/rmdir/setattr/rename/
 * link/symlink, wdata.c: write/truncate/fsync/sync) and wired here.
 * WFS_CAP_READONLY is not declared: whether a mount is writable is decided at
 * mount time (read-only device, "ro", dirty log, unsupported features), and a
 * read-only mount makes every changing operation fail with -WFS_EROFS.
 * The single exported symbol is wfs_backend_entry.
 */
#include "winuxfs/backend.h"
#include "winuxfs/errno.h"
#include "mount.h"
#include "ops.h"

#ifdef _WIN32
#define WFS_EXPORT __declspec(dllexport)
#else
#define WFS_EXPORT __attribute__((visibility("default")))
#endif

static wfs_err_t x_probe(const wfs_host_api *api, wfs_blkdev *devs[WFS_DEV_COUNT])
{
	return wfs_xfs_probe(api, devs);
}

static wfs_err_t x_mount(const wfs_host_api *api, wfs_blkdev *devs[WFS_DEV_COUNT],
			 const char *options, wfs_fs **out)
{
	struct wfs_xfs *x = NULL;
	wfs_err_t err = wfs_xfs_mount(api, devs, options, &x);

	*out = err ? NULL : (wfs_fs *)x;
	return err;
}

static wfs_err_t x_unmount(wfs_fs *fs) { return wfs_xfs_unmount((struct wfs_xfs *)fs); }

static wfs_err_t x_statfs(wfs_fs *fs, wfs_statfs *st)
{
	return wfs_xfs_op_statfs((struct wfs_xfs *)fs, st);
}
static wfs_err_t x_getattr(wfs_fs *fs, wfs_ino_t ino, wfs_stat *st)
{
	return wfs_xfs_op_getattr((struct wfs_xfs *)fs, ino, st);
}
static wfs_err_t x_lookup(wfs_fs *fs, wfs_ino_t dir, const char *name, uint32_t len,
			  wfs_ino_t *out, wfs_stat *st)
{
	return wfs_xfs_op_lookup((struct wfs_xfs *)fs, dir, name, len, out, st);
}
static void x_forget(wfs_fs *fs, wfs_ino_t ino, uint64_t nlookup)
{
	wfs_xfs_op_forget((struct wfs_xfs *)fs, ino, nlookup);
}
static wfs_err_t x_readdir(wfs_fs *fs, wfs_ino_t dir, uint64_t cookie, wfs_dirent_cb cb, void *ctx)
{
	return wfs_xfs_op_readdir((struct wfs_xfs *)fs, dir, cookie, cb, ctx);
}

static wfs_err_t x_read(wfs_fs *fs, wfs_ino_t ino, uint64_t off, void *buf, size_t len, size_t *done)
{
	return wfs_xfs_op_read((struct wfs_xfs *)fs, ino, off, buf, len, done);
}
static wfs_err_t x_readlink(wfs_fs *fs, wfs_ino_t ino, char *buf, size_t cap, size_t *len)
{
	return wfs_xfs_op_readlink((struct wfs_xfs *)fs, ino, buf, cap, len);
}

#define X(fs) ((struct wfs_xfs *)(fs))
static wfs_err_t x_setattr(wfs_fs *fs, wfs_ino_t ino, const wfs_stat *st, uint32_t mask)
{
	return wfs_xfs_op_setattr(X(fs), ino, st, mask);
}
static wfs_err_t x_write(wfs_fs *fs, wfs_ino_t ino, uint64_t off, const void *buf, size_t len,
			 size_t *done)
{
	return wfs_xfs_op_write(X(fs), ino, off, buf, len, done);
}
static wfs_err_t x_create(wfs_fs *fs, wfs_ino_t dir, const char *name, uint32_t len, uint32_t ftype,
			  uint32_t mode, uint32_t uid, uint32_t gid, wfs_ino_t *out, wfs_stat *st)
{
	return wfs_xfs_op_create(X(fs), dir, name, len, ftype, mode, uid, gid, out, st);
}
static wfs_err_t x_unlink(wfs_fs *fs, wfs_ino_t dir, const char *name, uint32_t len)
{
	return wfs_xfs_op_unlink(X(fs), dir, name, len);
}
static wfs_err_t x_rmdir(wfs_fs *fs, wfs_ino_t dir, const char *name, uint32_t len)
{
	return wfs_xfs_op_rmdir(X(fs), dir, name, len);
}
static wfs_err_t x_rename(wfs_fs *fs, wfs_ino_t od, const char *on, uint32_t ol, wfs_ino_t nd,
			  const char *nn, uint32_t nl, uint32_t flags)
{
	return wfs_xfs_op_rename(X(fs), od, on, ol, nd, nn, nl, flags);
}
static wfs_err_t x_link(wfs_fs *fs, wfs_ino_t ino, wfs_ino_t nd, const char *name, uint32_t len)
{
	return wfs_xfs_op_link(X(fs), ino, nd, name, len);
}
static wfs_err_t x_symlink(wfs_fs *fs, wfs_ino_t dir, const char *name, uint32_t len,
			   const char *target, uint32_t tlen, uint32_t uid, uint32_t gid,
			   wfs_ino_t *out, wfs_stat *st)
{
	return wfs_xfs_op_symlink(X(fs), dir, name, len, target, tlen, uid, gid, out, st);
}
static wfs_err_t x_sync(wfs_fs *fs) { return wfs_xfs_op_sync(X(fs)); }
static wfs_err_t x_fsync(wfs_fs *fs, wfs_ino_t ino, int datasync)
{
	return wfs_xfs_op_fsync(X(fs), ino, datasync);
}

static const wfs_ops ops = {
	.probe = x_probe,
	.mount = x_mount,
	.unmount = x_unmount,
	.statfs = x_statfs,
	.getattr = x_getattr,
	.lookup = x_lookup,
	.forget = x_forget,
	.readdir = x_readdir,
	.read = x_read,
	.readlink = x_readlink,
	.setattr = x_setattr,
	.write = x_write,
	.create = x_create,
	.unlink = x_unlink,
	.rmdir = x_rmdir,
	.rename = x_rename,
	.link = x_link,
	.symlink = x_symlink,
	.sync = x_sync,
	.fsync = x_fsync,
};

static const wfs_backend_desc desc = {
	.abi_version = WFS_ABI_VERSION,
	.size = sizeof(wfs_backend_desc),
	.name = "xfs",
	.caps = WFS_CAP_HARDLINK | WFS_CAP_SYMLINK,
	.dev_allowed = (1u << WFS_DEV_DATA) | (1u << WFS_DEV_LOG) | (1u << WFS_DEV_RT),
	.dev_required = 1u << WFS_DEV_DATA,
	.ops = &ops,
};

WFS_EXPORT const wfs_backend_desc *wfs_backend_entry(void) { return &desc; }
