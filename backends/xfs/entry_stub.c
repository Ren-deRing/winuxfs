// SPDX-License-Identifier: GPL-2.0-only
/*
 * Skeleton backend descriptor "xfs" (Task 4). probe says "not XFS" and mount
 * is -ENOSYS until Task 6 replaces these with real code; the descriptor and
 * the single exported symbol stay.
 */
#include "winuxfs/backend.h"
#include "winuxfs/errno.h"

#ifdef _WIN32
#define WFS_EXPORT __declspec(dllexport)
#else
#define WFS_EXPORT __attribute__((visibility("default")))
#endif

/* PENDING(T6): probe/mount/unmount are replaced by mount.c. */
static wfs_err_t x_probe(const wfs_host_api *api, wfs_blkdev *devs[WFS_DEV_COUNT])
{
	(void)api; (void)devs;
	return -WFS_EINVAL;
}

static wfs_err_t x_mount(const wfs_host_api *api, wfs_blkdev *devs[WFS_DEV_COUNT],
			 const char *options, wfs_fs **out)
{
	(void)api; (void)devs; (void)options;
	*out = NULL;
	return -WFS_ENOSYS;
}

/* The loader requires these ops even though mount never succeeds yet.
 * PENDING(T6, T8, T9): replaced by the real implementations. */
static wfs_err_t x_unmount(wfs_fs *fs) { (void)fs; return -WFS_ENOSYS; }
static wfs_err_t x_statfs(wfs_fs *fs, wfs_statfs *st) { (void)fs; (void)st; return -WFS_ENOSYS; }
static wfs_err_t x_getattr(wfs_fs *fs, wfs_ino_t ino, wfs_stat *st)
{
	(void)fs; (void)ino; (void)st;
	return -WFS_ENOSYS;
}
static wfs_err_t x_lookup(wfs_fs *fs, wfs_ino_t dir, const char *name, uint32_t len,
			  wfs_ino_t *out, wfs_stat *st)
{
	(void)fs; (void)dir; (void)name; (void)len; (void)out; (void)st;
	return -WFS_ENOSYS;
}
static void x_forget(wfs_fs *fs, wfs_ino_t ino, uint64_t nlookup)
{
	(void)fs; (void)ino; (void)nlookup;
}
static wfs_err_t x_readdir(wfs_fs *fs, wfs_ino_t dir, uint64_t cookie, wfs_dirent_cb cb, void *ctx)
{
	(void)fs; (void)dir; (void)cookie; (void)cb; (void)ctx;
	return -WFS_ENOSYS;
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
};

static const wfs_backend_desc desc = {
	.abi_version = WFS_ABI_VERSION,
	.size = sizeof(wfs_backend_desc),
	.name = "xfs",
	.caps = WFS_CAP_READONLY | WFS_CAP_HARDLINK | WFS_CAP_SYMLINK,
	.dev_allowed = 0,
	.dev_required = 0,
	.ops = &ops,
};

WFS_EXPORT const wfs_backend_desc *wfs_backend_entry(void) { return &desc; }
