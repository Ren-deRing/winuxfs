// SPDX-License-Identifier: GPL-2.0-only
/*
 * Backend descriptor "xfs". probe/mount/unmount are real (Task 6, mount.c);
 * statfs/lookup/readdir stay -ENOSYS until Tasks 9/11 (getattr/forget: Task 8), the rest of the
 * required-op skeleton likewise. The single exported symbol stays.
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

/* The loader requires these ops. PENDING(T9, T11): statfs, lookup, readdir. */
static wfs_err_t x_statfs(wfs_fs *fs, wfs_statfs *st) { (void)fs; (void)st; return -WFS_ENOSYS; }
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
	.dev_allowed = (1u << WFS_DEV_DATA) | (1u << WFS_DEV_LOG) | (1u << WFS_DEV_RT),
	.dev_required = 1u << WFS_DEV_DATA,
	.ops = &ops,
};

WFS_EXPORT const wfs_backend_desc *wfs_backend_entry(void) { return &desc; }
