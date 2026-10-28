/* Deliberately broken plugins for loader tests. One source, three variants:
 *   -DBAD_ABI      descriptor declares abi_version 99
 *   -DBAD_NOOPS    mandatory op `lookup` is NULL
 *   -DBAD_NOENTRY  does not export wfs_backend_entry
 */
#include "winuxfs/backend.h"
#include "winuxfs/errno.h"

#ifdef _WIN32
#define WFS_EXPORT __declspec(dllexport)
#else
#define WFS_EXPORT __attribute__((visibility("default")))
#endif

static wfs_err_t b_mount(const wfs_host_api *h, wfs_blkdev *d[WFS_DEV_COUNT], const char *o, wfs_fs **out)
{ (void)h; (void)d; (void)o; *out = 0; return 0; }
static wfs_err_t b_unmount(wfs_fs *fs) { (void)fs; return 0; }
static wfs_err_t b_statfs(wfs_fs *fs, wfs_statfs *s) { (void)fs; (void)s; return -WFS_ENOSYS; }
static wfs_err_t b_getattr(wfs_fs *fs, wfs_ino_t i, wfs_stat *s) { (void)fs; (void)i; (void)s; return -WFS_ENOENT; }
static wfs_err_t b_lookup(wfs_fs *fs, wfs_ino_t d, const char *n, uint32_t l, wfs_ino_t *o, wfs_stat *s)
{ (void)fs; (void)d; (void)n; (void)l; (void)o; (void)s; return -WFS_ENOENT; }
static void b_forget(wfs_fs *fs, wfs_ino_t i, uint64_t n) { (void)fs; (void)i; (void)n; }
static wfs_err_t b_readdir(wfs_fs *fs, wfs_ino_t d, uint64_t c, wfs_dirent_cb cb, void *ctx)
{ (void)fs; (void)d; (void)c; (void)cb; (void)ctx; return 0; }

static const wfs_ops ops = {
    .mount = b_mount, .unmount = b_unmount, .statfs = b_statfs, .getattr = b_getattr,
#ifndef BAD_NOOPS
    .lookup = b_lookup,
#endif
    .forget = b_forget, .readdir = b_readdir,
};

static const wfs_backend_desc desc = {
#ifdef BAD_ABI
    .abi_version = 99,
#else
    .abi_version = WFS_ABI_VERSION,
#endif
    .size = sizeof(wfs_backend_desc), .name = "bad", .caps = 0,
    .dev_allowed = 0, .dev_required = 0, .ops = &ops,
};

#ifdef BAD_NOENTRY
WFS_EXPORT const wfs_backend_desc *not_the_entry(void) { return &desc; }
#else
WFS_EXPORT const wfs_backend_desc *wfs_backend_entry(void) { return &desc; }
#endif

#ifdef BAD_NOOPS
/* keep b_lookup referenced so -Wunused-function stays quiet */
void *bad_unused_lookup = (void *)&b_lookup;
#endif
