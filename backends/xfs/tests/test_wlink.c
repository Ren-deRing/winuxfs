// SPDX-License-Identifier: GPL-2.0-only
/*
 * M3a T8 tests: link, symlink, rename.
 * usage: test_wlink TMPDIR
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

#define PLAIN_STR "-m crc=1,rmapbt=0,reflink=0,metadir=0 -n parent=0 -i sparse=1"
/* WFEAT overrides the mkfs features (make test-wrmap runs the write suites on rmapbt+reflink) */
#define PLAIN (getenv("WFEAT") ? getenv("WFEAT") : PLAIN_STR)

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
__attribute__((unused)) static int ul(struct mnt *m, wfs_ino_t d, const char *n)
{
	return wfs_xfs_op_unlink(m->x, d, n, (uint32_t)strlen(n));
}
__attribute__((unused)) static int rd(struct mnt *m, wfs_ino_t d, const char *n)
{
	return wfs_xfs_op_rmdir(m->x, d, n, (uint32_t)strlen(n));
}
__attribute__((unused)) static uint64_t free_inodes(struct mnt *m)
{
	wfs_statfs s;

	wfs_xfs_op_statfs(m->x, &s);
	return s.free_inodes;
}

static const char *tmp_img(const char *name)
{
	static char p[700];

	snprintf(p, sizeof(p), "%s/%s.img", tmp, name);
	return p;
}
static int rn(struct mnt *m, wfs_ino_t od, const char *o, wfs_ino_t nd, const char *n, uint32_t fl)
{
	return wfs_xfs_op_rename(m->x, od, o, (uint32_t)strlen(o), nd, n, (uint32_t)strlen(n), fl);
}
static int ln(struct mnt *m, wfs_ino_t ino, wfs_ino_t nd, const char *n)
{
	return wfs_xfs_op_link(m->x, ino, nd, n, (uint32_t)strlen(n));
}
static int sl(struct mnt *m, wfs_ino_t d, const char *n, const char *t, wfs_ino_t *o, wfs_stat *st)
{
	return wfs_xfs_op_symlink(m->x, d, n, (uint32_t)strlen(n), t, (uint32_t)strlen(t), 1000, 100, o, st);
}
static uint32_t nlink_of(struct mnt *m, wfs_ino_t i)
{
	wfs_stat s;

	return wfs_xfs_op_getattr(m->x, i, &s) ? 9999 : s.nlink;
}
static int new_mnt(struct mnt *m, const char *name)
{
	const char *img = mkimg(name, PLAIN);

	return img ? mnt_open(m, img, "", false) : -1;
}
static bool done_clean(struct mnt *m, const char *name)
{
	char path[700];

	snprintf(path, sizeof(path), "%s/%s.img", tmp, name);
	return mnt_close(m) == 0 && repair_clean(path);
}

static void link_basic(void)
{
	wfs_ino_t f, g, d, t;
	wfs_stat st;
	struct mnt m;
	size_t got;
	char buf[16];

	CHECK_RC(new_mnt(&m, "lnk"), 0);
	if (!m.x)
		return;
	CHECK_RC(mk(&m, ROOT, "f", WFS_FT_REG, 0644, &f, &st), 0);
	CHECK_RC(wfs_xfs_op_write(m.x, f, 0, "data", 4, &got), 0);
	CHECK_RC(ln(&m, f, ROOT, "g"), 0);
	CHECK_RC(lk(&m, ROOT, "g", &g, &st), 0);
	CHECK(g == f && st.nlink == 2);
	CHECK_RC(ln(&m, f, ROOT, "g"), -WFS_EEXIST);
	CHECK_RC(mk(&m, ROOT, "d", WFS_FT_DIR, 0755, &d, &st), 0);
	CHECK_RC(ln(&m, d, ROOT, "dl"), -WFS_EPERM);
	CHECK_RC(ln(&m, f, f, "x"), -WFS_ENOTDIR);
	CHECK_RC(ul(&m, ROOT, "f"), 0);
	CHECK(nlink_of(&m, f) == 1);
	CHECK_RC(lk(&m, ROOT, "g", &t, &st), 0);
	CHECK_RC(wfs_xfs_op_read(m.x, t, 0, buf, sizeof(buf), &got), 0);
	CHECK(got == 4 && !memcmp(buf, "data", 4));
	wfs_xfs_op_forget(m.x, t, 1);
	CHECK_RC(ul(&m, ROOT, "g"), 0);
	CHECK(nlink_of(&m, f) == 0);
	CHECK_RC(ln(&m, f, d, "dead"), -WFS_ENOENT);
	wfs_xfs_op_forget(m.x, f, 2);		/* creation + first lookup */
	wfs_xfs_op_forget(m.x, d, 1);
	CHECK(done_clean(&m, "lnk"));
}

static void symlink_basic(void)
{
	char t300[301], t1024[1024], back[1100];
	wfs_ino_t s1, s2, s3, t;
	wfs_stat st;
	struct mnt m;
	size_t len;

	memset(t300, 'a', 300); t300[300] = 0;
	memset(t1024, 'b', 1023); t1024[1023] = 0;
	CHECK_RC(new_mnt(&m, "sym"), 0);
	if (!m.x)
		return;
	CHECK_RC(sl(&m, ROOT, "s1", "short/target", &s1, &st), 0);
	CHECK(st.ftype == WFS_FT_LNK && st.size == 12 && st.nlink == 1 && st.uid == 1000);
	CHECK_RC(sl(&m, ROOT, "s2", t300, &s2, &st), 0);
	CHECK(st.size == 300);
	CHECK_RC(sl(&m, ROOT, "s3", t1024, &s3, &st), 0);
	CHECK(st.size == 1023);
	CHECK_RC(sl(&m, ROOT, "s4", "x", &t, &st), 0);
	wfs_xfs_op_forget(m.x, t, 1);
	CHECK_RC(sl(&m, ROOT, "s1", "x", &t, &st), -WFS_EEXIST);
	CHECK_RC(sl(&m, ROOT, "e", "", &t, &st), -WFS_EINVAL);
	{
		char big[1100];

		memset(big, 'c', sizeof(big)); big[1024] = 0;
		CHECK_RC(sl(&m, ROOT, "big", big, &t, &st), -WFS_ENAMETOOLONG);
	}
	CHECK_RC(wfs_xfs_op_readlink(m.x, s1, back, sizeof(back), &len), 0);
	CHECK(len == 12 && !memcmp(back, "short/target", 12));
	CHECK_RC(wfs_xfs_op_readlink(m.x, s2, back, sizeof(back), &len), 0);
	CHECK(len == 300 && !memcmp(back, t300, 300));
	CHECK_RC(wfs_xfs_op_readlink(m.x, s3, back, sizeof(back), &len), 0);
	CHECK(len == 1023 && !memcmp(back, t1024, 1023));
	CHECK_RC(mnt_close(&m), 0);
	CHECK_RC(mnt_open(&m, tmp_img("sym"), "", false), 0);	/* remount: persisted */
	if (m.x) {
		CHECK_RC(lk(&m, ROOT, "s3", &t, &st), 0);
		CHECK_RC(wfs_xfs_op_readlink(m.x, t, back, sizeof(back), &len), 0);
		CHECK(len == 1023 && !memcmp(back, t1024, 1023));
		CHECK_RC(ul(&m, ROOT, "s3"), 0);		/* frees remote blocks via inactive */
		CHECK_RC(ul(&m, ROOT, "s2"), 0);
		CHECK_RC(ul(&m, ROOT, "s1"), 0);
		wfs_xfs_op_forget(m.x, t, 1);
	}
	CHECK(done_clean(&m, "sym"));
}

static void rename_basic(void)
{
	wfs_ino_t a, b, d1, d2, d3, x, y;
	wfs_stat st, r0;
	struct mnt m;

	CHECK_RC(new_mnt(&m, "ren"), 0);
	if (!m.x)
		return;
	wfs_xfs_op_getattr(m.x, ROOT, &r0);
	CHECK_RC(mk(&m, ROOT, "a", WFS_FT_REG, 0644, &a, &st), 0);
	CHECK_RC(mk(&m, ROOT, "b", WFS_FT_REG, 0644, &b, &st), 0);
	CHECK_RC(mk(&m, ROOT, "d1", WFS_FT_DIR, 0755, &d1, &st), 0);
	CHECK_RC(mk(&m, ROOT, "d2", WFS_FT_DIR, 0755, &d2, &st), 0);
	CHECK_RC(mk(&m, d2, "inner", WFS_FT_DIR, 0755, &d3, &st), 0);
	CHECK_RC(rn(&m, ROOT, "a", ROOT, "a2", 0), 0);		/* plain */
	CHECK_RC(lk(&m, ROOT, "a", &x, &st), -WFS_ENOENT);
	CHECK_RC(lk(&m, ROOT, "a2", &x, &st), 0);
	CHECK(x == a);
	wfs_xfs_op_forget(m.x, x, 1);
	CHECK_RC(rn(&m, ROOT, "a2", ROOT, "b", WFS_RENAME_NOREPLACE), -WFS_EEXIST);
	CHECK_RC(rn(&m, ROOT, "a2", ROOT, "b", WFS_RENAME_EXCHANGE), -WFS_EINVAL);
	CHECK_RC(rn(&m, ROOT, "a2", ROOT, "a2", 0), 0);		/* same entry */
	CHECK_RC(rn(&m, ROOT, "nope", ROOT, "z", 0), -WFS_ENOENT);
	CHECK_RC(rn(&m, ROOT, "a2", ROOT, "b", 0), 0);		/* over a file: old b is freed */
	CHECK_RC(lk(&m, ROOT, "b", &x, &st), 0);
	CHECK(x == a);
	wfs_xfs_op_forget(m.x, x, 1);
	CHECK_RC(wfs_xfs_op_getattr(m.x, b, &st), 0);		/* b still held by the host */
	CHECK(st.nlink == 0);
	wfs_xfs_op_forget(m.x, b, 1);			/* last reference: inactive */
	CHECK_RC(rn(&m, ROOT, "b", d1, "moved", 0), 0);		/* across dirs */
	CHECK_RC(lk(&m, d1, "moved", &x, &st), 0);
	wfs_xfs_op_forget(m.x, x, 1);
	CHECK_RC(rn(&m, ROOT, "d1", ROOT, "d1b", 0), 0);		/* directory */
	CHECK_RC(rn(&m, ROOT, "d2", d2, "self", 0), -WFS_EINVAL);	/* into itself */
	CHECK_RC(rn(&m, ROOT, "d2", d3, "deep", 0), -WFS_EINVAL);	/* into its own subtree */
	CHECK_RC(rn(&m, d2, "inner", ROOT, "inner_up", 0), 0);	/* dir to another parent */
	CHECK_RC(lk(&m, d3, "..", &x, &st), 0);
	CHECK(x == ROOT);
	CHECK_RC(rn(&m, ROOT, "d1b", d2, "d1c", 0), 0);
	CHECK_RC(mk(&m, ROOT, "ne", WFS_FT_DIR, 0755, &x, &st), 0);
	CHECK_RC(mk(&m, x, "f", WFS_FT_REG, 0644, &y, &st), 0);
	CHECK_RC(rn(&m, ROOT, "d2", ROOT, "ne", 0), -WFS_ENOTEMPTY);
	CHECK_RC(mk(&m, ROOT, "emptyd", WFS_FT_DIR, 0755, &y, &st), 0);
	CHECK_RC(rn(&m, ROOT, "emptyd", ROOT, "inner_up", 0), 0);	/* dir over empty dir */
	CHECK_RC(rn(&m, ROOT, "b", ROOT, "inner_up", 0), -WFS_ENOENT);
	CHECK_RC(mk(&m, ROOT, "file", WFS_FT_REG, 0644, &x, &st), 0);
	CHECK_RC(rn(&m, ROOT, "file", ROOT, "inner_up", 0), -WFS_EISDIR);
	CHECK_RC(rn(&m, ROOT, "inner_up", ROOT, "file", 0), -WFS_ENOTDIR);
	wfs_xfs_op_forget(m.x, x, 1);
	wfs_xfs_op_forget(m.x, y, 1);
	CHECK(done_clean(&m, "ren"));
}

static void rename_many(void)
{
	char n[64];
	wfs_ino_t d1, d2, i;
	wfs_stat st;
	struct mnt m;
	int k;

	CHECK_RC(new_mnt(&m, "renm"), 0);
	if (!m.x)
		return;
	CHECK_RC(mk(&m, ROOT, "d1", WFS_FT_DIR, 0755, &d1, &st), 0);
	CHECK_RC(mk(&m, ROOT, "d2", WFS_FT_DIR, 0755, &d2, &st), 0);
	for (k = 0; k < 400; k++) {
		snprintf(n, sizeof(n), "a-fairly-long-file-name-%04d", k);
		CHECK_RC(mk(&m, d1, n, WFS_FT_REG, 0644, &i, &st), 0);
		wfs_xfs_op_forget(m.x, i, 1);
	}
	for (k = 0; k < 400; k++) {
		snprintf(n, sizeof(n), "a-fairly-long-file-name-%04d", k);
		CHECK_RC(rn(&m, d1, n, d2, n, 0), 0);
	}
	for (k = 0; k < 400; k += 2) {
		snprintf(n, sizeof(n), "a-fairly-long-file-name-%04d", k);
		CHECK_RC(rn(&m, d2, n, d1, n, 0), 0);
	}
	CHECK_RC(wfs_xfs_op_sync(m.x), 0);
	CHECK(done_clean(&m, "renm"));
}

int main(int argc, char **argv)
{
	if (argc < 2) {
		fprintf(stderr, "usage: test_wlink TMPDIR\n");
		return 2;
	}
	snprintf(tmp, sizeof(tmp), "%s/test_wlink.%d", argv[1], (int)getpid());
	if (sh("mkdir -p '%s'", tmp))
		return 2;
	RUN(link_basic);
	RUN(symlink_basic);
	RUN(rename_basic);
	RUN(rename_many);
	if (!failures)
		sh("rm -rf '%s'", tmp);
	else
		printf("images kept in %s\n", tmp);
	printf("test_wlink: %d checks, %d failures, %d skipped\n", checks, failures, skipped);
	return failures ? 1 : 0;
}
