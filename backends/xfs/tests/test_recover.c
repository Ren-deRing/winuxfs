// SPDX-License-Identifier: GPL-2.0-only
/*
 * M3a T5 tests: dirty-log replay (a crash is a copy of the image taken after a log force).
 * usage: test_recover TMPDIR
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

static int wr(struct mnt *m, wfs_ino_t f, uint64_t off, const void *b, size_t n)
{
	size_t done = 0;
	int rc = wfs_xfs_op_write(m->x, f, off, b, n, &done);

	return rc ? rc : (done == n ? 0 : -9999);
}
static void fill(uint8_t *b, size_t n, uint32_t seed)
{
	size_t i;

	for (i = 0; i < n; i++) {
		seed = seed * 1664525u + 1013904223u;
		b[i] = (uint8_t)(seed >> 24);
	}
}
static const char *cpimg(const char *img, const char *name)
{
	static char paths[4][600];
	static int next;
	char *p = paths[next++ % 4];

	snprintf(p, sizeof(paths[0]), "%s/%s.img", tmp, name);
	if (sh("cp --sparse=always '%s' '%s'", img, p))
		return NULL;
	return p;
}
static bool log_is_dirty(const char *img)
{
	char out[256];

	sh_out(out, sizeof(out), "xfs_logprint -t '%s' 2>&1 | grep -c DIRTY", img);
	return atoi(out) > 0;
}
static void h_names(char *dst, size_t n, int k)
{
	snprintf(dst, n, "file-%03d-with-a-longer-name", k);
}

/* The workload: a directory with 60 files (data), some renamed/unlinked/changed. */
static void workload(struct mnt *m, wfs_ino_t *dir_out)
{
	uint8_t buf[6000];
	char n[64], n2[64];
	wfs_ino_t d, f;
	wfs_stat st;
	int k;

	mk(m, ROOT, "d", WFS_FT_DIR, 0755, &d, &st);
	*dir_out = d;
	for (k = 0; k < 60; k++) {
		h_names(n, sizeof(n), k);
		CHECK_RC(mk(m, d, n, WFS_FT_REG, 0644, &f, &st), 0);
		fill(buf, sizeof(buf), (uint32_t)k);
		CHECK_RC(wr(m, f, 0, buf, sizeof(buf)), 0);
		wfs_xfs_op_forget(m->x, f, 1);
	}
	for (k = 0; k < 60; k += 3) {			/* unlink every third */
		h_names(n, sizeof(n), k);
		CHECK_RC(wfs_xfs_op_unlink(m->x, d, n, (uint32_t)strlen(n)), 0);
	}
	for (k = 1; k < 60; k += 6) {			/* rename some */
		h_names(n, sizeof(n), k);
		snprintf(n2, sizeof(n2), "renamed-%03d", k);
		CHECK_RC(wfs_xfs_op_rename(m->x, d, n, (uint32_t)strlen(n), d, n2, (uint32_t)strlen(n2), 0), 0);
	}
	h_names(n, sizeof(n), 2);			/* truncate one */
	CHECK_RC(lk(m, d, n, &f, &st), 0);
	{
		wfs_stat s = { 0 };

		s.size = 100;
		CHECK_RC(wfs_xfs_op_setattr(m->x, f, &s, WFS_SET_SIZE), 0);
	}
	wfs_xfs_op_forget(m->x, f, 1);
}

static void check_workload(struct mnt *m, wfs_ino_t d)
{
	uint8_t want[6000], got[6000];
	char n[64];
	wfs_ino_t f;
	wfs_stat st;
	size_t g;
	int k;

	for (k = 0; k < 60; k++) {
		h_names(n, sizeof(n), k);
		if (k % 3 == 0) {
			CHECK_RC(lk(m, d, n, &f, &st), -WFS_ENOENT);
		} else if (k % 6 == 1) {
			CHECK_RC(lk(m, d, n, &f, &st), -WFS_ENOENT);
			snprintf(n, sizeof(n), "renamed-%03d", k);
			CHECK_RC(lk(m, d, n, &f, &st), 0);
		} else {
			CHECK_RC(lk(m, d, n, &f, &st), 0);
		}
		if (k % 3 == 0)
			continue;
		fill(want, sizeof(want), (uint32_t)k);
		CHECK(st.size == (k == 2 ? 100u : 6000u));
		CHECK_RC(wfs_xfs_op_read(m->x, f, 0, got, sizeof(got), &g), 0);
		CHECK(g == st.size && !memcmp(got, want, g));
		wfs_xfs_op_forget(m->x, f, 1);
	}
}

static void replay_after_crash(void)
{
	const char *img = mkimg("rc", PLAIN);
	const char *crashed;
	struct mnt m;
	wfs_ino_t d, d2;
	wfs_stat st;

	CHECK(img != NULL);
	if (!img)
		return;
	CHECK_RC(mnt_open(&m, img, "", false), 0);
	if (!m.x)
		return;
	workload(&m, &d);
	xfs_log_force(&m.x->mp, XFS_LOG_SYNC);	/* committed to the log, home blocks stale */
	crashed = cpimg(img, "rc_crash");
	CHECK(crashed != NULL);
	CHECK_RC(mnt_close(&m), 0);
	if (!crashed)
		return;
	CHECK(log_is_dirty(crashed));

	/* a read-only device cannot replay: refused, nothing written */
	CHECK_RC(mnt_open(&m, crashed, "", true), -WFS_EUCLEAN);

	CHECK_RC(mnt_open(&m, crashed, "", false), 0);	/* replays */
	if (!m.x)
		return;
	CHECK_RC(lk(&m, ROOT, "d", &d2, &st), 0);
	check_workload(&m, d2);
	CHECK_RC(mnt_close(&m), 0);
	CHECK(!log_is_dirty(crashed));
	CHECK(repair_clean(crashed));

	CHECK_RC(mnt_open(&m, crashed, "", false), 0);	/* and it is writable afterwards */
	if (m.x) {
		wfs_ino_t f;

		CHECK_RC(lk(&m, ROOT, "d", &d2, &st), 0);
		CHECK_RC(mk(&m, d2, "after", WFS_FT_REG, 0644, &f, &st), 0);
		CHECK_RC(mnt_close(&m), 0);
	}
	CHECK(repair_clean(crashed));
}

/* A log written by the real Linux kernel (tools/gen-root-fixtures.sh: 200 creates, a mkdir, then
 * "shutdown -f"): replay it with an rw mount and compare with what the kernel had done. */
static void kernel_written_log(void)
{
	static const char *fx = "../../tests/xfs/root-fixtures/dirty-log.img.zst";
	char img[700], name[32];
	struct mnt m;
	wfs_ino_t d, f;
	wfs_stat st;
	int i, miss = 0;

	if (access(fx, R_OK)) {
		printf("SKIP kernel_written_log (no %s)\n", fx);
		skipped++;
		return;
	}
	snprintf(img, sizeof(img), "%s/kern.img", tmp);
	CHECK(sh("zstd -dqf '%s' -o '%s'", fx, img) == 0);
	CHECK(log_is_dirty(img));
	CHECK_RC(mnt_open(&m, img, "", true), -WFS_EUCLEAN);
	CHECK_RC(mnt_open(&m, img, "", false), 0);
	if (!m.x)
		return;
	CHECK_RC(lk(&m, ROOT, "before.bin", &f, &st), 0);
	CHECK(st.size == 65536);
	CHECK_RC(lk(&m, ROOT, "new_dir", &d, &st), 0);
	CHECK(st.ftype == WFS_FT_DIR);
	for (i = 0; i < 200; i++) {
		snprintf(name, sizeof(name), "new_%d", i);
		if (lk(&m, ROOT, name, &f, &st) || st.size != 0)
			miss++;
	}
	CHECK(miss == 0);
	CHECK_RC(mnt_close(&m), 0);
	CHECK(!log_is_dirty(img));
	CHECK(repair_clean(img));
}

/* Replaying twice gives the same result (the first replay's own crash is T9). */
static void replay_is_idempotent(void)
{
	const char *img = mkimg("ri", PLAIN);
	const char *a, *b;
	struct mnt m;
	wfs_ino_t d, d2;
	wfs_stat st;

	CHECK(img != NULL);
	if (!img)
		return;
	CHECK_RC(mnt_open(&m, img, "", false), 0);
	if (!m.x)
		return;
	workload(&m, &d);
	xfs_log_force(&m.x->mp, XFS_LOG_SYNC);
	a = cpimg(img, "ri_a");
	b = cpimg(img, "ri_b");
	CHECK_RC(mnt_close(&m), 0);
	if (!a || !b)
		return;
	CHECK_RC(mnt_open(&m, a, "", false), 0);
	if (m.x)
		CHECK_RC(mnt_close(&m), 0);
	CHECK_RC(mnt_open(&m, a, "", false), 0);	/* second mount of the replayed image */
	if (m.x) {
		CHECK_RC(lk(&m, ROOT, "d", &d2, &st), 0);
		check_workload(&m, d2);
		CHECK_RC(mnt_close(&m), 0);
	}
	CHECK_RC(mnt_open(&m, b, "norecovery", true), 0);	/* stale view, but mounts */
	if (m.x)
		CHECK_RC(mnt_close(&m), 0);
	CHECK(repair_clean(a));
}

int main(int argc, char **argv)
{
	if (argc < 2) {
		fprintf(stderr, "usage: test_recover TMPDIR\n");
		return 2;
	}
	snprintf(tmp, sizeof(tmp), "%s/test_recover.%d", argv[1], (int)getpid());
	if (sh("mkdir -p '%s'", tmp))
		return 2;
	RUN(replay_after_crash);
	RUN(replay_is_idempotent);
	RUN(kernel_written_log);
	if (!failures)
		sh("rm -rf '%s'", tmp);
	else
		printf("images kept in %s\n", tmp);
	printf("test_recover: %d checks, %d failures, %d skipped\n", checks, failures, skipped);
	return failures ? 1 : 0;
}
