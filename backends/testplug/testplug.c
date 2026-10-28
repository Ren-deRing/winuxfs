/* Tiny test backend for the core's Mount plumbing (not a real filesystem).
 *
 * Tree: root dir ino 1 containing one regular file "hello" (ino 2).
 * File content is "hello\n", or bytes 4..10 of the DATA device when one is given.
 * probe: 0 if DATA starts with "TPLG", else -EINVAL.
 * Only the mandatory ops plus `read` are implemented; the rest are NULL (-> ENOSYS).
 *
 * Test hooks:
 *   getattr(3)    sleeps ~2 ms and counts overlapping calls (testplug_overlaps())
 *   getattr(1000) returns +7 (invalid positive result)
 *   getattr(1001) returns -1234 (unknown errno)
 *   getattr(2000+e) returns -e (errno passthrough / range boundaries)
 *   read(4)       reports done = len + 1 (bogus)
 *   lookup(1, "badft") succeeds with out ino 7 but ftype 99 (bogus)
 *   lookup(1, "zero")  succeeds with out ino 0 (bogus)
 *   readdir(5)    emits 1000 entries, ignoring the callback's stop signal
 *   readdir(6)    emits good, bad (ftype 99), good, good, ignoring stop
 *   readdir(9..12) emit one entry with next_cookie u64::MAX-1, u64::MAX, 2^63,
 *                 2^63-1 (the last is valid); readdir(1, cookie 99) emits u64::MAX
 *   testplug_unmount_count() counts unmount calls.
 *   testplug_bad_forgets() sums forget(n) calls on the bogus inodes 7 and 8.
 *   testplug_accepted_after_bad() counts callbacks after the bad readdir(6)
 *     entry that returned 0 (i.e. asked for more).
 * Variant -DTESTPLUG_ST: caps = WFS_CAP_SINGLE_THREADED, dev_required = DATA,
 *   plus `create`: "badft" -> ino 8 with ftype 99, "zero" -> ino 0,
 *   anything else -EEXIST.
 */
#define _POSIX_C_SOURCE 200809L
#include "winuxfs/backend.h"
#include <errno.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#define WFS_EXPORT __declspec(dllexport)
static void nap(void) { Sleep(2); }
#else
#include <time.h>
#define WFS_EXPORT __attribute__((visibility("default")))
static void nap(void) { struct timespec t = {0, 2000000}; nanosleep(&t, 0); }
#endif

struct wfs_fs { char content[6]; };

static atomic_int unmounts, inflight, overlaps, bad_forgets, accepted_after_bad;

WFS_EXPORT int testplug_unmount_count(void) { return atomic_load(&unmounts); }
WFS_EXPORT int testplug_overlaps(void) { return atomic_load(&overlaps); }
WFS_EXPORT int testplug_bad_forgets(void) { return atomic_load(&bad_forgets); }
WFS_EXPORT int testplug_accepted_after_bad(void) { return atomic_load(&accepted_after_bad); }

static int read_magic(const wfs_host_api *h, wfs_blkdev *d, char out[10])
{
    return d && h->blk_read(d, 0, out, 10) == 0 && memcmp(out, "TPLG", 4) == 0;
}

static wfs_err_t t_probe(const wfs_host_api *h, wfs_blkdev *d[WFS_DEV_COUNT])
{
    char b[10];
    return read_magic(h, d[WFS_DEV_DATA], b) ? 0 : -EINVAL;
}

static wfs_err_t t_mount(const wfs_host_api *h, wfs_blkdev *d[WFS_DEV_COUNT], const char *o, wfs_fs **out)
{
    (void)o;
    wfs_fs *fs = malloc(sizeof *fs);
    if (!fs) return -ENOMEM;
    char b[10];
    if (read_magic(h, d[WFS_DEV_DATA], b)) memcpy(fs->content, b + 4, 6);
    else memcpy(fs->content, "hello\n", 6);
    *out = fs;
    return 0;
}

static wfs_err_t t_unmount(wfs_fs *fs) { free(fs); atomic_fetch_add(&unmounts, 1); return 0; }

static wfs_err_t t_statfs(wfs_fs *fs, wfs_statfs *s)
{
    (void)fs;
    memset(s, 0, sizeof *s);
    s->total_bytes = 4096; s->free_bytes = s->avail_bytes = 0;
    s->total_inodes = 2; s->block_size = 512; s->name_max = 255;
    return 0;
}

static void fill(wfs_ino_t ino, wfs_stat *s)
{
    memset(s, 0, sizeof *s);
    s->ino = ino;
    s->ftype = ino == 1 ? WFS_FT_DIR : WFS_FT_REG;
    s->mode = ino == 1 ? 0755 : 0644;
    s->nlink = ino == 1 ? 2 : 1;
    s->size = ino == 1 ? 0 : 6;
}

static wfs_err_t t_getattr(wfs_fs *fs, wfs_ino_t ino, wfs_stat *s)
{
    (void)fs;
    if (ino == 3) {
        if (atomic_fetch_add(&inflight, 1) > 0) atomic_fetch_add(&overlaps, 1);
        nap();
        atomic_fetch_sub(&inflight, 1);
        return -ENOENT;
    }
    if (ino == 1000) return 7;
    if (ino == 1001) return -1234;
    if (ino >= 2000 && ino < 3000) return -(wfs_err_t)(ino - 2000);
    if (ino != 1 && ino != 2) return -ENOENT;
    fill(ino, s);
    return 0;
}

static wfs_err_t t_lookup(wfs_fs *fs, wfs_ino_t dir, const char *n, uint32_t l, wfs_ino_t *out, wfs_stat *s)
{
    (void)fs;
    if (dir == 2) return -ENOTDIR;
    if (dir == 1 && l == 5 && memcmp(n, "badft", 5) == 0) {
        fill(2, s); s->ino = 7; s->ftype = 99; *out = 7;
        return 0;
    }
    if (dir == 1 && l == 4 && memcmp(n, "zero", 4) == 0) {
        fill(2, s); *out = 0;
        return 0;
    }
    if (dir != 1 || l != 5 || memcmp(n, "hello", 5) != 0) return -ENOENT;
    *out = 2;
    fill(2, s);
    return 0;
}

static void t_forget(wfs_fs *fs, wfs_ino_t i, uint64_t n)
{
    (void)fs;
    if (i == 7 || i == 8) atomic_fetch_add(&bad_forgets, (int)n);
}

#ifdef TESTPLUG_ST
static wfs_err_t t_create(wfs_fs *fs, wfs_ino_t dir, const char *n, uint32_t l, uint32_t ftype,
                          uint32_t mode, uint32_t uid, uint32_t gid, wfs_ino_t *out, wfs_stat *s)
{
    (void)fs; (void)dir; (void)ftype; (void)mode; (void)uid; (void)gid;
    if (l == 5 && memcmp(n, "badft", 5) == 0) {
        fill(2, s); s->ino = 8; s->ftype = 99; *out = 8;
        return 0;
    }
    if (l == 4 && memcmp(n, "zero", 4) == 0) {
        fill(2, s); *out = 0;
        return 0;
    }
    return -EEXIST;
}
#endif

static wfs_err_t t_readdir(wfs_fs *fs, wfs_ino_t dir, uint64_t cookie, wfs_dirent_cb cb, void *ctx)
{
    (void)fs;
    if (dir == 5) {
        for (uint64_t i = 0; i < 1000; i++) {
            wfs_dirent e = { .next_cookie = i + 1, .ino = 2, .ftype = WFS_FT_REG, .name_len = 1, .name = "x" };
            (void)cb(ctx, &e);
        }
        return 0;
    }
    if (dir == 6) {
        wfs_dirent good = { .next_cookie = 1, .ino = 2, .ftype = WFS_FT_REG, .name_len = 1, .name = "g" };
        wfs_dirent bad = { .next_cookie = 2, .ino = 2, .ftype = 99, .name_len = 1, .name = "b" };
        (void)cb(ctx, &good);
        (void)cb(ctx, &bad);
        for (int i = 0; i < 2; i++)
            if (cb(ctx, &good) == 0) atomic_fetch_add(&accepted_after_bad, 1);
        return 0;
    }
    if (dir >= 9 && dir <= 12) {
        static const uint64_t ck[] = { UINT64_MAX - 1, UINT64_MAX, (uint64_t)1 << 63, ((uint64_t)1 << 63) - 1 };
        wfs_dirent e = { .next_cookie = ck[dir - 9], .ino = 2, .ftype = WFS_FT_REG, .name_len = 1, .name = "c" };
        (void)cb(ctx, &e);
        return 0;
    }
    if (dir == 1 && cookie == 99) {
        wfs_dirent e = { .next_cookie = UINT64_MAX, .ino = 2, .ftype = WFS_FT_REG, .name_len = 1, .name = "c" };
        (void)cb(ctx, &e);
        return 0;
    }
    if (dir != 1) return dir == 2 ? -ENOTDIR : -ENOENT;
    if (cookie == 0) {
        wfs_dirent e = { .next_cookie = 1, .ino = 2, .ftype = WFS_FT_REG, .name_len = 5, .name = "hello" };
        cb(ctx, &e);
    }
    return 0;
}

static wfs_err_t t_read(wfs_fs *fs, wfs_ino_t ino, uint64_t off, void *buf, size_t len, size_t *done)
{
    if (ino == 4) { *done = len + 1; return 0; }
    if (ino == 1) return -EISDIR;
    if (ino != 2) return -ENOENT;
    size_t n = off >= 6 ? 0 : 6 - (size_t)off;
    if (n > len) n = len;
    if (n) memcpy(buf, fs->content + off, n);
    *done = n;
    return 0;
}

static const wfs_ops ops = {
    .probe = t_probe, .mount = t_mount, .unmount = t_unmount, .statfs = t_statfs,
    .getattr = t_getattr, .lookup = t_lookup, .forget = t_forget, .readdir = t_readdir,
    .read = t_read,
#ifdef TESTPLUG_ST
    .create = t_create,
#endif
};

static const wfs_backend_desc desc = {
    .abi_version = WFS_ABI_VERSION, .size = sizeof(wfs_backend_desc),
#ifdef TESTPLUG_ST
    .name = "testplug_st", .caps = WFS_CAP_SINGLE_THREADED,
    .dev_allowed = 1 << WFS_DEV_DATA, .dev_required = 1 << WFS_DEV_DATA,
#else
    .name = "testplug", .caps = 0,
    .dev_allowed = 1 << WFS_DEV_DATA, .dev_required = 0,
#endif
    .ops = &ops,
};

WFS_EXPORT const wfs_backend_desc *wfs_backend_entry(void) { return &desc; }
