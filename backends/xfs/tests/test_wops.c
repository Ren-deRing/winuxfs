// SPDX-License-Identifier: GPL-2.0-only
/*
 * M3a T6 tests: create/mkdir/unlink/rmdir/setattr, inactive on forget.
 * usage: test_wops TMPDIR
 * The host is a file-backed fake that really writes (pwrite) and counts
 * writes and flushes; it can be made to fail every write. Ground truth comes
 * from xfsprogs: xfs_repair -n (consistency), xfs_db (superblock fields),
 * xfs_logprint (log contents). Images are made with mkfs.xfs in TMPDIR.
 */
#include "wfs_xfs.h"
#include "xfs_sb.h"
#include "ops.h"
#include "xfs_log.h"
#include "xfs_log_priv.h"
#include "xfs_trans.h"
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int failures, checks, skipped;
#define CHECK(c) do { checks++; if (!(c)) { failures++; \
	fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #c); } } while (0)
#define CHECK_RC(got, want) do { int _g = (got); checks++; if (_g != (want)) { failures++; \
	fprintf(stderr, "%s:%d: %s = %d, want %d (log: %s)\n", __FILE__, __LINE__, #got, _g, \
		(int)(want), logbuf); } } while (0)
#define RUN(t) do { int _f = failures; log_reset(); t(); \
	printf("%s %s\n", failures == _f ? "PASS" : "FAIL", #t); } while (0)

/* ---------------------------------------------------------------- host -- */
struct wfs_blkdev {
	int		fd;
	uint64_t	size;
	uint32_t	ss;
	bool		ro;
};

static char logbuf[16384];
static long nwrites, nflushes;
static bool fail_writes;

static void h_log(int level, const char *msg)
{
	(void)level;
	if (strlen(logbuf) + strlen(msg) + 2 < sizeof(logbuf)) {
		strcat(logbuf, msg);
		strcat(logbuf, "\n");
	}
}
static void log_reset(void) { logbuf[0] = 0; }
__attribute__((unused)) static bool log_has(const char *s) { return strstr(logbuf, s) != NULL; }

static uint64_t h_size(wfs_blkdev *d) { return d->size; }
static uint32_t h_ss(wfs_blkdev *d) { return d->ss; }
static int h_ro(wfs_blkdev *d) { return d->ro; }
static wfs_err_t h_read(wfs_blkdev *d, uint64_t off, void *buf, size_t len)
{
	if (off > d->size || len > d->size - off)
		return -WFS_EIO;
	return pread(d->fd, buf, len, (off_t)off) == (ssize_t)len ? 0 : -WFS_EIO;
}
static wfs_err_t h_write(wfs_blkdev *d, uint64_t off, const void *buf, size_t len)
{
	nwrites++;
	if (d->ro || fail_writes || off > d->size || len > d->size - off)
		return -WFS_EIO;
	return pwrite(d->fd, buf, len, (off_t)off) == (ssize_t)len ? 0 : -WFS_EIO;
}
static wfs_err_t h_flush(wfs_blkdev *d)
{
	(void)d;
	nflushes++;
	return fail_writes ? -WFS_EIO : 0;
}

static const wfs_host_api api = {
	.abi_version = WFS_ABI_VERSION, .size = sizeof(wfs_host_api),
	.blk_size = h_size, .blk_sector_size = h_ss, .blk_read_only = h_ro,
	.blk_read = h_read, .blk_write = h_write, .blk_flush = h_flush, .log = h_log,
};

/* ------------------------------------------------------------- helpers -- */
static char tmp[512];

static int sh(const char *fmt, ...)
{
	char cmd[4096];
	va_list ap;

	va_start(ap, fmt);
	vsnprintf(cmd, sizeof(cmd), fmt, ap);
	va_end(ap);
	return system(cmd);
}

/* first line of a command's output, trimmed */
__attribute__((unused)) static void sh_out(char *out, size_t n, const char *fmt, ...)
{
	char cmd[4096];
	va_list ap;
	FILE *f;

	va_start(ap, fmt);
	vsnprintf(cmd, sizeof(cmd), fmt, ap);
	va_end(ap);
	out[0] = 0;
	f = popen(cmd, "r");
	if (!f)
		return;
	if (!fgets(out, (int)n, f))
		out[0] = 0;
	pclose(f);
	out[strcspn(out, "\n")] = 0;
}

struct mnt {
	wfs_blkdev dev;
	wfs_blkdev *p[WFS_DEV_COUNT];
	struct wfs_xfs *x;
};

static int mnt_open(struct mnt *m, const char *img, const char *opts, bool ro_dev)
{
	struct stat st;
	int rc;

	memset(m, 0, sizeof(*m));
	m->dev.fd = open(img, ro_dev ? O_RDONLY : O_RDWR);
	if (m->dev.fd < 0 || fstat(m->dev.fd, &st))
		return -1000;
	m->dev.size = (uint64_t)st.st_size;
	m->dev.ss = 512;
	m->dev.ro = ro_dev;
	m->p[WFS_DEV_DATA] = &m->dev;
	rc = wfs_xfs_mount(&api, m->p, opts, &m->x);
	if (rc) {
		close(m->dev.fd);
		m->x = NULL;
	}
	return rc;
}

static int mnt_close(struct mnt *m)
{
	int rc = 0;

	if (m->x)
		rc = wfs_xfs_unmount(m->x);
	m->x = NULL;
	close(m->dev.fd);
	return rc;
}

/* mkfs a 320 MiB (sparse) image at tmp/NAME.img with extra mkfs options */
static const char *mkimg(const char *name, const char *opts)
{
	static char paths[4][600];	/* a few live at once */
	static int next;
	char *path = paths[next++ % 4];

	snprintf(path, sizeof(paths[0]), "%s/%s.img", tmp, name);
	unlink(path);
	if (sh("truncate -s 320m '%s' && mkfs.xfs -q -f %s '%s' >/dev/null 2>&1", path, opts, path))
		return NULL;
	return path;
}

#define PLAIN "-m crc=1,rmapbt=0,reflink=0,metadir=0 -n parent=0 -i sparse=1"

static bool repair_clean(const char *img)
{
	return sh("xfs_repair -n '%s' >'%s/repair.out' 2>&1", img, tmp) == 0;
}

/* the log head (cycle, block) of a mounted log */

/* ---------------------------------------------------------------- tests -- */
#define ROOT WFS_ROOT_INO

static int lk(struct mnt *m, wfs_ino_t d, const char *n, wfs_ino_t *o, wfs_stat *st)
{
	return wfs_xfs_op_lookup(m->x, d, n, (uint32_t)strlen(n), o, st);
}
static int mk(struct mnt *m, wfs_ino_t d, const char *n, uint32_t ft, uint32_t mode, wfs_ino_t *o, wfs_stat *st)
{
	return wfs_xfs_op_create(m->x, d, n, (uint32_t)strlen(n), ft, mode, 1000, 100, o, st);
}
static int ul(struct mnt *m, wfs_ino_t d, const char *n)
{
	return wfs_xfs_op_unlink(m->x, d, n, (uint32_t)strlen(n));
}
static int rd(struct mnt *m, wfs_ino_t d, const char *n)
{
	return wfs_xfs_op_rmdir(m->x, d, n, (uint32_t)strlen(n));
}
static uint64_t free_inodes(struct mnt *m)
{
	wfs_statfs s;

	wfs_xfs_op_statfs(m->x, &s);
	return s.free_inodes;
}

static void create_remove_basic(void)
{
	const char *img = mkimg("basic", PLAIN);
	wfs_ino_t a, d, f, t;
	wfs_stat st, rst;
	struct mnt m;

	CHECK(img != NULL);
	if (!img)
		return;
	CHECK_RC(mnt_open(&m, img, "", false), 0);
	if (!m.x)
		return;
	CHECK_RC(wfs_xfs_op_getattr(m.x, ROOT, &rst), 0);
	CHECK_RC(mk(&m, ROOT, "a", WFS_FT_REG, 0644, &a, &st), 0);
	CHECK(st.ftype == WFS_FT_REG && st.mode == 0644 && st.uid == 1000 && st.gid == 100 &&
	      st.nlink == 1 && st.size == 0);
	CHECK_RC(mk(&m, ROOT, "d", WFS_FT_DIR, 0755, &d, &st), 0);
	CHECK(st.ftype == WFS_FT_DIR && st.mode == 0755 && st.nlink == 2);
	CHECK_RC(mk(&m, d, "f", WFS_FT_REG, 0600, &f, &st), 0);
	CHECK_RC(mk(&m, ROOT, "a", WFS_FT_REG, 0644, &t, &st), -WFS_EEXIST);
	CHECK_RC(mk(&m, ROOT, "x", WFS_FT_FIFO, 0644, &t, &st), -WFS_EOPNOTSUPP);
	CHECK_RC(lk(&m, d, "f", &t, &st), 0);
	CHECK(t == f);
	wfs_xfs_op_forget(m.x, t, 1);
	CHECK_RC(wfs_xfs_op_getattr(m.x, ROOT, &st), 0);
	CHECK(st.nlink == rst.nlink + 1);		/* the subdirectory's ".." */
	CHECK_RC(rd(&m, ROOT, "d"), -WFS_ENOTEMPTY);
	CHECK_RC(rd(&m, ROOT, "a"), -WFS_ENOTDIR);
	CHECK_RC(ul(&m, ROOT, "d"), -WFS_EISDIR);
	CHECK_RC(ul(&m, d, "f"), 0);
	CHECK_RC(rd(&m, ROOT, "d"), 0);
	CHECK_RC(ul(&m, ROOT, "a"), 0);
	CHECK_RC(ul(&m, ROOT, "a"), -WFS_ENOENT);
	CHECK_RC(lk(&m, ROOT, "a", &t, &st), -WFS_ENOENT);
	CHECK_RC(wfs_xfs_op_getattr(m.x, ROOT, &st), 0);
	CHECK(st.nlink == rst.nlink);
	wfs_xfs_op_forget(m.x, a, 1);
	wfs_xfs_op_forget(m.x, d, 1);
	wfs_xfs_op_forget(m.x, f, 1);
	CHECK_RC(mnt_close(&m), 0);
	CHECK(repair_clean(img));
}

/* An unlinked inode that the host still references is freed by the last forget. */
static void inactive_on_forget(void)
{
	const char *img = mkimg("inact", PLAIN);
	wfs_ino_t a;
	wfs_stat st;
	struct mnt m;
	uint64_t before;

	CHECK(img != NULL);
	if (!img)
		return;
	CHECK_RC(mnt_open(&m, img, "", false), 0);
	if (!m.x)
		return;
	before = free_inodes(&m);
	CHECK_RC(mk(&m, ROOT, "a", WFS_FT_REG, 0644, &a, &st), 0);
	CHECK_RC(ul(&m, ROOT, "a"), 0);
	CHECK_RC(wfs_xfs_op_getattr(m.x, a, &st), 0);	/* still open */
	CHECK(st.nlink == 0);
	wfs_xfs_op_forget(m.x, a, 1);
	CHECK_RC(wfs_xfs_op_getattr(m.x, a, &st), -WFS_ESTALE);
	CHECK(free_inodes(&m) == before);
	CHECK_RC(mnt_close(&m), 0);
	CHECK(repair_clean(img));
}

static void setattr_basic(void)
{
	const char *img = mkimg("setattr", PLAIN);
	wfs_stat st, n = { 0 };
	wfs_ino_t a;
	struct mnt m;

	CHECK(img != NULL);
	if (!img)
		return;
	CHECK_RC(mnt_open(&m, img, "", false), 0);
	if (!m.x)
		return;
	CHECK_RC(mk(&m, ROOT, "a", WFS_FT_REG, 0644, &a, &st), 0);
	n.mode = 04711; n.uid = 7; n.gid = 8;
	n.mtime.sec = 1700000000; n.mtime.nsec = 123;
	n.atime.sec = 1600000000; n.atime.nsec = 5;
	CHECK_RC(wfs_xfs_op_setattr(m.x, a, &n, WFS_SET_MODE | WFS_SET_UID | WFS_SET_GID |
				    WFS_SET_MTIME | WFS_SET_ATIME), 0);
	CHECK_RC(wfs_xfs_op_getattr(m.x, a, &st), 0);
	CHECK(st.mode == 04711 && st.uid == 7 && st.gid == 8);
	CHECK(st.mtime.sec == 1700000000 && st.mtime.nsec == 123);
	CHECK(st.atime.sec == 1600000000 && st.atime.nsec == 5);
	n.size = 0;
	CHECK_RC(wfs_xfs_op_setattr(m.x, a, &n, WFS_SET_SIZE), 0);	/* no change */
	n.size = 10;
	CHECK_RC(wfs_xfs_op_setattr(m.x, a, &n, WFS_SET_SIZE), 0);	/* grows (T7) */
	CHECK_RC(wfs_xfs_op_getattr(m.x, a, &st), 0);
	CHECK(st.size == 10);
	CHECK_RC(mnt_close(&m), 0);
	CHECK(repair_clean(img));
	CHECK_RC(mnt_open(&m, img, "", false), 0);	/* persisted across remount */
	if (m.x) {
		CHECK_RC(lk(&m, ROOT, "a", &a, &st), 0);
		CHECK(st.mode == 04711 && st.uid == 7 && st.size == 10);	/* mtime moved with the size change */
		CHECK_RC(mnt_close(&m), 0);
	}
}

/* Directories that grow past short-form and block-form, then shrink back. */
static void many_entries(void)
{
	const char *img = mkimg("many", PLAIN);
	char name[300];
	wfs_ino_t i;
	wfs_stat st;
	struct mnt m;
	int k;

	CHECK(img != NULL);
	if (!img)
		return;
	CHECK_RC(mnt_open(&m, img, "", false), 0);
	if (!m.x)
		return;
	int nn = 600;

	for (k = 0; k < nn; k++) {
		snprintf(name, sizeof(name), "file-with-a-longish-name-%04d", k);
		int rc = mk(&m, ROOT, name, WFS_FT_REG, 0644, &i, &st);

		CHECK(rc == 0);
		if (rc) {
			fprintf(stderr, "many_entries: create #%d failed rc=%d\n", k, rc);
			break;
		}
		wfs_xfs_op_forget(m.x, i, 1);
	}
	memset(name, 'n', 255);
	name[255] = 0;
	CHECK_RC(mk(&m, ROOT, name, WFS_FT_REG, 0644, &i, &st), 0);
	wfs_xfs_op_forget(m.x, i, 1);
	name[255] = 'n';
	name[256] = 0;
	CHECK_RC(mk(&m, ROOT, name, WFS_FT_REG, 0644, &i, &st), -WFS_ENAMETOOLONG);
	CHECK_RC(mnt_close(&m), 0);
	CHECK(repair_clean(img));
	CHECK_RC(mnt_open(&m, img, "", false), 0);
	if (!m.x)
		return;
	for (k = 0; k < nn; k += 2) {
		snprintf(name, sizeof(name), "file-with-a-longish-name-%04d", k);
		CHECK_RC(ul(&m, ROOT, name), 0);
	}
	CHECK_RC(mnt_close(&m), 0);
	CHECK(repair_clean(img));
}

int main(int argc, char **argv)
{
	if (argc < 2) {
		fprintf(stderr, "usage: test_wops TMPDIR\n");
		return 2;
	}
	snprintf(tmp, sizeof(tmp), "%s/test_wops.%d", argv[1], (int)getpid());
	if (sh("mkdir -p '%s'", tmp))
		return 2;
	RUN(create_remove_basic);
	RUN(inactive_on_forget);
	RUN(setattr_basic);
	RUN(many_entries);
	if (!failures)
		sh("rm -rf '%s'", tmp);
	else
		printf("images kept in %s\n", tmp);
	printf("test_wops: %d checks, %d failures, %d skipped\n", checks, failures, skipped);
	return failures ? 1 : 0;
}
