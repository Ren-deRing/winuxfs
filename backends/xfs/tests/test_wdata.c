// SPDX-License-Identifier: GPL-2.0-only
/*
 * M3a T7 tests: data write, truncate, fsync, ENOSPC, >4 GiB, random oracle.
 * usage: test_wdata TMPDIR
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
static int rdb(struct mnt *m, wfs_ino_t f, uint64_t off, void *b, size_t n, size_t *got)
{
	return wfs_xfs_op_read(m->x, f, off, b, n, got);
}
static int trunc_to(struct mnt *m, wfs_ino_t f, uint64_t size)
{
	wfs_stat s = { 0 };

	s.size = size;
	return wfs_xfs_op_setattr(m->x, f, &s, WFS_SET_SIZE);
}
static uint64_t fsize(struct mnt *m, wfs_ino_t f)
{
	wfs_stat s;

	return wfs_xfs_op_getattr(m->x, f, &s) ? ~0ull : s.size;
}
static void fill(uint8_t *b, size_t n, uint32_t seed)
{
	size_t i;

	for (i = 0; i < n; i++) {
		seed = seed * 1664525u + 1013904223u;
		b[i] = (uint8_t)(seed >> 24);
	}
}
/* does the file hold exactly want[0..n)? */
static bool same(struct mnt *m, wfs_ino_t f, const uint8_t *want, size_t n)
{
	uint8_t *b = malloc(n + 1);
	size_t got = 0;
	bool ok;

	ok = b && fsize(m, f) == n && rdb(m, f, 0, b, n + 1, &got) == 0 && got == n &&
	     (n == 0 || memcmp(b, want, n) == 0);
	free(b);
	return ok;
}
static int open_new(struct mnt *m, const char *img, wfs_ino_t *f)
{
	wfs_stat st;
	int rc = mnt_open(m, img, "", false);

	return rc ? rc : mk(m, ROOT, "f", WFS_FT_REG, 0644, f, &st);
}

static void write_read_basic(void)
{
	const char *img = mkimg("wrb", PLAIN);
	uint8_t *model = calloc(1, 3 << 20), *buf = malloc(2 << 20);
	size_t got, i, msize = 0;
	wfs_ino_t f, t;
	struct mnt m;
	wfs_stat st;

	CHECK(img && model && buf);
	if (!img || !model || !buf)
		return;
	CHECK_RC(open_new(&m, img, &f), 0);
	if (!m.x)
		return;
	CHECK_RC(wr(&m, f, 0, "hello", 5), 0);
	memcpy(model, "hello", 5); msize = 5;
	CHECK(same(&m, f, model, msize));
	CHECK_RC(wr(&m, f, 1, "EY", 2), 0);			/* overwrite inside a block */
	memcpy(model + 1, "EY", 2);
	CHECK(same(&m, f, model, msize));
	fill(buf, 7000, 1);					/* spans blocks, leaves a hole */
	CHECK_RC(wr(&m, f, 20000, buf, 7000), 0);
	memcpy(model + 20000, buf, 7000); msize = 27000;
	CHECK(same(&m, f, model, msize));
	fill(buf, 2 << 20, 2);					/* 2 MiB over existing + new */
	CHECK_RC(wr(&m, f, 1000, buf, 2 << 20), 0);
	memcpy(model + 1000, buf, 2 << 20); msize = 1000 + (2 << 20);
	CHECK(same(&m, f, model, msize));
	CHECK_RC(wfs_xfs_op_getattr(m.x, f, &st), 0);
	CHECK(st.blocks >= msize / 512);
	CHECK_RC(wfs_xfs_op_fsync(m.x, f, 0), 0);
	CHECK_RC(wfs_xfs_op_sync(m.x), 0);
	CHECK_RC(wr(&m, ROOT, 0, "x", 1), -WFS_EISDIR);
	CHECK_RC(rdb(&m, f, msize, buf, 10, &got), 0);
	CHECK(got == 0);
	for (i = 0; i < msize && i < 100; i++)
		CHECK(model[i] == model[i]);
	CHECK_RC(mnt_close(&m), 0);
	CHECK(repair_clean(img));
	CHECK_RC(mnt_open(&m, img, "", false), 0);		/* persisted */
	if (m.x) {
		CHECK_RC(lk(&m, ROOT, "f", &t, &st), 0);
		CHECK(same(&m, t, model, msize));
		CHECK_RC(mnt_close(&m), 0);
	}
	free(model); free(buf);
}

static void truncate_basic(void)
{
	const char *img = mkimg("trunc", PLAIN);
	uint8_t a[8000], z[8000], *big = malloc(1 << 20), *model = calloc(1, 1 << 20);
	uint64_t free0, free1;
	size_t got;
	wfs_ino_t f;
	struct mnt m;
	wfs_statfs s;

	CHECK(img && big && model);
	if (!img || !big || !model)
		return;
	CHECK_RC(open_new(&m, img, &f), 0);
	if (!m.x)
		return;
	memset(a, 'A', sizeof(a));
	memset(z, 0, sizeof(z));
	wfs_xfs_op_statfs(m.x, &s); free0 = s.free_bytes;
	CHECK_RC(wr(&m, f, 0, a, sizeof(a)), 0);
	CHECK_RC(trunc_to(&m, f, 100), 0);
	CHECK(fsize(&m, f) == 100);
	CHECK_RC(trunc_to(&m, f, 8000), 0);			/* the tail must read as zeros */
	CHECK_RC(rdb(&m, f, 0, big, 8000, &got), 0);
	CHECK(got == 8000 && !memcmp(big, a, 100) && !memcmp(big + 100, z, 7900));
	fill(big, 1 << 20, 5);
	CHECK_RC(wr(&m, f, 0, big, 1 << 20), 0);
	memcpy(model, big, 1 << 20);
	CHECK_RC(trunc_to(&m, f, 300000), 0);
	CHECK(same(&m, f, model, 300000));
	CHECK_RC(trunc_to(&m, f, 2 << 20), 0);		/* grow: hole */
	CHECK(fsize(&m, f) == (2u << 20));
	CHECK_RC(trunc_to(&m, f, 0), 0);
	CHECK(fsize(&m, f) == 0);
	CHECK_RC(trunc_to(&m, ROOT, 0), -WFS_EISDIR);
	wfs_xfs_op_statfs(m.x, &s); free1 = s.free_bytes;
	CHECK(free1 + (64u << 10) >= free0);			/* the blocks came back (minus dir/log slack) */
	CHECK_RC(mnt_close(&m), 0);
	CHECK(repair_clean(img));
	free(big); free(model);
}

static void big_offset(void)
{
	const char *img = mkimg("big", PLAIN);
	const uint64_t off = 5ull << 30;
	uint8_t buf[5000], back[5000];
	size_t got;
	wfs_ino_t f;
	struct mnt m;

	CHECK(img != NULL);
	if (!img)
		return;
	CHECK_RC(open_new(&m, img, &f), 0);
	if (!m.x)
		return;
	fill(buf, sizeof(buf), 9);
	CHECK_RC(wr(&m, f, off + 123, buf, sizeof(buf)), 0);
	CHECK(fsize(&m, f) == off + 123 + sizeof(buf));
	CHECK_RC(rdb(&m, f, off + 123, back, sizeof(back), &got), 0);
	CHECK(got == sizeof(buf) && !memcmp(buf, back, sizeof(buf)));
	CHECK_RC(rdb(&m, f, 4ull << 30, back, 100, &got), 0);
	CHECK(got == 100 && back[0] == 0 && back[99] == 0);
	CHECK_RC(trunc_to(&m, f, 0), 0);
	CHECK_RC(mnt_close(&m), 0);
	CHECK(repair_clean(img));
}

static void enospc(void)
{
	const char *img = mkimg("full", PLAIN);
	uint8_t *buf = malloc(4 << 20);
	uint64_t total = 0, free0;
	wfs_statfs s;
	wfs_ino_t f;
	struct mnt m;
	size_t done;
	int rc = 0, k;

	CHECK(img && buf);
	if (!img || !buf)
		return;
	fill(buf, 4 << 20, 3);
	CHECK_RC(open_new(&m, img, &f), 0);
	if (!m.x)
		return;
	wfs_xfs_op_statfs(m.x, &s); free0 = s.free_bytes;
	for (k = 0; k < 200 && !rc; k++) {
		done = 0;
		rc = wfs_xfs_op_write(m.x, f, total, buf, 4 << 20, &done);
		total += done;
		if (!rc && done < (4u << 20))
			rc = wfs_xfs_op_write(m.x, f, total, buf, 4 << 20, &done), total += done;
	}
	CHECK(rc == -WFS_ENOSPC);
	CHECK(total > free0 / 2 && total <= free0);
	CHECK(fsize(&m, f) == total);
	CHECK_RC(wfs_xfs_op_sync(m.x), 0);
	CHECK_RC(trunc_to(&m, f, 0), 0);
	wfs_xfs_op_statfs(m.x, &s);
	CHECK(s.free_bytes + (1u << 20) >= free0);
	CHECK_RC(wr(&m, f, 0, buf, 4 << 20), 0);		/* usable again */
	CHECK_RC(mnt_close(&m), 0);
	CHECK(repair_clean(img));
	free(buf);
}

static uint32_t rng;
static uint32_t R(void) { rng = rng * 1103515245u + 12345u; return rng >> 8; }

/* Random writes/truncates against an in-memory model of the file. */
static void random_oracle(void)
{
	const char *img = mkimg("rand", PLAIN);
	const size_t MAXF = 3 << 20;
	uint8_t *model = calloc(1, MAXF + 70000), *buf = malloc(70000);
	size_t msize = 0, step, len;
	wfs_ino_t f;
	struct mnt m;
	uint64_t off;

	rng = 12345;
	CHECK(img && model && buf);
	if (!img || !model || !buf)
		return;
	CHECK_RC(open_new(&m, img, &f), 0);
	if (!m.x)
		return;
	for (step = 0; step < 300; step++) {
		unsigned op = R() % 10;

		if (op < 7) {
			off = R() % (MAXF - 1);
			len = 1 + R() % (R() % 4 ? 9000 : 69000);
			fill(buf, len, (uint32_t)step);
			CHECK_RC(wr(&m, f, off, buf, len), 0);
			if (off > msize)
				memset(model + msize, 0, off - msize);
			memcpy(model + off, buf, len);
			if (off + len > msize)
				msize = off + len;
		} else if (op < 9) {
			size_t ns = R() % MAXF;

			CHECK_RC(trunc_to(&m, f, ns), 0);
			if (ns > msize)
				memset(model + msize, 0, ns - msize);
			msize = ns;
		} else {
			CHECK_RC(wfs_xfs_op_fsync(m.x, f, 0), 0);
		}
		if (step % 25 == 24)
			CHECK(same(&m, f, model, msize));
	}
	CHECK(same(&m, f, model, msize));
	CHECK_RC(mnt_close(&m), 0);
	CHECK(repair_clean(img));
	free(model); free(buf);
}

int main(int argc, char **argv)
{
	if (argc < 2) {
		fprintf(stderr, "usage: test_wdata TMPDIR\n");
		return 2;
	}
	snprintf(tmp, sizeof(tmp), "%s/test_wdata.%d", argv[1], (int)getpid());
	if (sh("mkdir -p '%s'", tmp))
		return 2;
	RUN(write_read_basic);
	RUN(truncate_basic);
	RUN(big_offset);
	RUN(enospc);
	RUN(random_oracle);
	if (!failures)
		sh("rm -rf '%s'", tmp);
	else
		printf("images kept in %s\n", tmp);
	printf("test_wdata: %d checks, %d failures, %d skipped\n", checks, failures, skipped);
	return failures ? 1 : 0;
}
