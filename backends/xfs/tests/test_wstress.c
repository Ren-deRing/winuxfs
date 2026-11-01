// SPDX-License-Identifier: GPL-2.0-only
/*
 * M3a T11: parallel write stress (run under TSan/ASan): threads on one mount create,
 * write, truncate, rename and unlink in private and shared directories; every
 * survivor is verified against a per-thread oracle, before and after a remount,
 * and xfs_repair -n judges the image.
 * usage: test_wstress TMPDIR
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
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int failures, checks, skipped;
static pthread_mutex_t cklock = PTHREAD_MUTEX_INITIALIZER;
#define CHECK(c) do { pthread_mutex_lock(&cklock); checks++; if (!(c)) { failures++; \
	fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #c); } pthread_mutex_unlock(&cklock); } while (0)
#define CHECK_RC(got, want) do { int _g = (got); pthread_mutex_lock(&cklock); checks++; if (_g != (want)) { failures++; \
	fprintf(stderr, "%s:%d: %s = %d, want %d (log: %s)\n", __FILE__, __LINE__, #got, _g, \
		(int)(want), logbuf); } pthread_mutex_unlock(&cklock); } while (0)
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
static pthread_mutex_t loglock = PTHREAD_MUTEX_INITIALIZER;
static bool fail_writes;

static void h_log(int level, const char *msg)
{
	(void)level;
	pthread_mutex_lock(&loglock);
	if (strlen(logbuf) + strlen(msg) + 2 < sizeof(logbuf)) {
		strcat(logbuf, msg);
		strcat(logbuf, "\n");
	}
	pthread_mutex_unlock(&loglock);
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
	__atomic_add_fetch(&nwrites, 1, __ATOMIC_RELAXED);
	if (d->ro || fail_writes || off > d->size || len > d->size - off)
		return -WFS_EIO;
	return pwrite(d->fd, buf, len, (off_t)off) == (ssize_t)len ? 0 : -WFS_EIO;
}
static wfs_err_t h_flush(wfs_blkdev *d)
{
	(void)d;
	__atomic_add_fetch(&nflushes, 1, __ATOMIC_RELAXED);
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
__attribute__((unused)) static int open_new(struct mnt *m, const char *img, wfs_ino_t *f)
{
	wfs_stat st;
	int rc = mnt_open(m, img, "", false);

	return rc ? rc : mk(m, ROOT, "f", WFS_FT_REG, 0644, f, &st);
}


#define NT	4
#define NF	6		/* files per thread */
#define ITER	50
#define MAXLEN	70000

struct fent {
	char		name[40];
	wfs_ino_t	ino;
	uint8_t		*data;		/* oracle */
	size_t		len;
	bool		live;
};
struct tctx {
	struct mnt	*m;
	int		id;
	wfs_ino_t	dir, shared;
	struct fent	f[NF];
	uint32_t	rng;
};

static uint32_t trnd(struct tctx *t) { t->rng = t->rng * 1103515245u + 12345u; return t->rng >> 8; }

static void *worker(void *arg)
{
	struct tctx *t = arg;
	struct mnt *m = t->m;
	wfs_stat st;
	int it, k;

	for (it = 0; it < ITER; it++) {
		struct fent *e = &t->f[trnd(t) % NF];
		int op = (int)(trnd(t) % 10);

		if (!e->live) {
			snprintf(e->name, sizeof(e->name), "t%d-f%d-%d", t->id, (int)(e - t->f), it);
			CHECK_RC(mk(m, t->dir, e->name, WFS_FT_REG, 0644, &e->ino, &st), 0);
			e->live = true;
			e->len = 0;
			continue;
		}
		if (op < 5) {				/* write a random range */
			size_t off = trnd(t) % (e->len + 4096), n = 1 + trnd(t) % 12000;
			uint8_t *b = malloc(n);

			fill(b, n, trnd(t));
			CHECK_RC(wr(m, e->ino, off, b, n), 0);
			if (off + n > MAXLEN) {	/* keep the oracle bounded */
				CHECK_RC(trunc_to(m, e->ino, 0), 0);
				e->len = 0;
			} else {
				if (off + n > e->len) {
					e->data = realloc(e->data, off + n);
					if (off > e->len)
						memset(e->data + e->len, 0, off - e->len);
					e->len = off + n;
				}
				memcpy(e->data + off, b, n);
			}
			free(b);
		} else if (op == 5) {			/* truncate */
			size_t ns = e->len ? trnd(t) % (e->len + 1) : 0;

			CHECK_RC(trunc_to(m, e->ino, ns), 0);
			e->len = ns;
		} else if (op == 6) {			/* rename inside the private dir */
			char nn[40];

			snprintf(nn, sizeof(nn), "t%d-r%d-%d", t->id, (int)(e - t->f), it);
			CHECK_RC(wfs_xfs_op_rename(m->x, t->dir, e->name, (uint32_t)strlen(e->name), t->dir, nn,
						   (uint32_t)strlen(nn), 0), 0);
			memcpy(e->name, nn, sizeof(e->name));
		} else if (op == 7) {			/* churn in the shared directory */
			char sn[40];
			wfs_ino_t s;

			snprintf(sn, sizeof(sn), "s%d-%d", t->id, it);
			CHECK_RC(mk(m, t->shared, sn, WFS_FT_REG, 0600, &s, &st), 0);
			CHECK_RC(wr(m, s, 0, sn, strlen(sn)), 0);
			CHECK_RC(wfs_xfs_op_unlink(m->x, t->shared, sn, (uint32_t)strlen(sn)), 0);
			wfs_xfs_op_forget(m->x, s, 1);
		} else if (op == 8) {			/* unlink */
			CHECK_RC(wfs_xfs_op_unlink(m->x, t->dir, e->name, (uint32_t)strlen(e->name)), 0);
			wfs_xfs_op_forget(m->x, e->ino, 1);
			e->live = false;
			free(e->data);
			e->data = NULL;
			e->len = 0;
		} else {				/* read back and verify */
			CHECK(same(m, e->ino, e->data, e->len));
		}
		if (t->id == 0 && it % 10 == 0)
			wfs_xfs_op_sync(m->x);
	}
	for (k = 0; k < NF; k++)
		if (t->f[k].live)
			CHECK(same(m, t->f[k].ino, t->f[k].data, t->f[k].len));
	return NULL;
}

static void parallel_writers(void)
{
	struct mnt m;
	struct tctx t[NT];
	pthread_t th[NT];
	const char *img = mkimg("wstress", PLAIN);
	wfs_stat st;
	wfs_ino_t shared;
	int i, k;

	CHECK(img != NULL);
	if (!img)
		return;
	CHECK_RC(mnt_open(&m, img, "", false), 0);
	CHECK_RC(mk(&m, ROOT, "shared", WFS_FT_DIR, 0755, &shared, &st), 0);
	memset(t, 0, sizeof(t));
	for (i = 0; i < NT; i++) {
		char dn[16];

		snprintf(dn, sizeof(dn), "d%d", i);
		t[i].m = &m;
		t[i].id = i;
		t[i].rng = 7u + (uint32_t)i * 977u;
		t[i].shared = shared;
		CHECK_RC(mk(&m, ROOT, dn, WFS_FT_DIR, 0755, &t[i].dir, &st), 0);
	}
	for (i = 0; i < NT; i++)
		pthread_create(&th[i], NULL, worker, &t[i]);
	for (i = 0; i < NT; i++)
		pthread_join(th[i], NULL);
	CHECK_RC(mnt_close(&m), 0);
	CHECK(repair_clean(img));

	/* a fresh mount sees exactly the survivors, byte for byte */
	CHECK_RC(mnt_open(&m, img, "", false), 0);
	for (i = 0; i < NT; i++) {
		char dn[16];
		wfs_ino_t d;

		snprintf(dn, sizeof(dn), "d%d", i);
		CHECK_RC(lk(&m, ROOT, dn, &d, &st), 0);
		for (k = 0; k < NF; k++) {
			struct fent *e = &t[i].f[k];
			wfs_ino_t f;

			if (!e->live)
				continue;
			CHECK_RC(lk(&m, d, e->name, &f, &st), 0);
			CHECK(same(&m, f, e->data, e->len));
		}
	}
	CHECK_RC(lk(&m, ROOT, "shared", &shared, &st), 0);
	CHECK_RC(lk(&m, shared, "s0-7", &shared, &st), -WFS_ENOENT);
	CHECK_RC(mnt_close(&m), 0);
	for (i = 0; i < NT; i++)
		for (k = 0; k < NF; k++)
			free(t[i].f[k].data);
}


/* The Windows CopyFile pattern: the destination is first extended to its final size,
 * then written in large chunks while the source is read back concurrently. */
struct cp { struct mnt *m; wfs_ino_t src, dst; size_t len; int rc; };
static void *cp_reader(void *arg)
{
	struct cp *c = arg;
	uint8_t *b = malloc(65536);
	size_t off, got;

	for (off = 0; off < c->len; off += 65536) {
		got = 0;
		if (rdb(c->m, c->src, off, b, 65536, &got))
			c->rc = -1;
	}
	free(b);
	return NULL;
}
static void copy_pattern(void)
{
	struct mnt m;
	struct cp c = { .m = &m, .len = 3u << 20 };
	const char *img = mkimg("wcopy", PLAIN);
	wfs_stat st;
	uint8_t *data = malloc(c.len), *back = malloc(c.len);
	pthread_t th;
	size_t off;

	CHECK(img != NULL);
	if (!img)
		return;
	fill(data, c.len, 99);
	CHECK_RC(mnt_open(&m, img, "", false), 0);
	CHECK_RC(mk(&m, ROOT, "src", WFS_FT_REG, 0644, &c.src, &st), 0);
	CHECK_RC(wr(&m, c.src, 0, data, c.len), 0);
	CHECK_RC(mk(&m, ROOT, "dst", WFS_FT_REG, 0644, &c.dst, &st), 0);
	pthread_create(&th, NULL, cp_reader, &c);
	CHECK_RC(trunc_to(&m, c.dst, c.len), 0);		/* extend first */
	for (off = 0; off < c.len; off += 1u << 20)
		CHECK_RC(wr(&m, c.dst, off, data + off, 1u << 20), 0);
	pthread_join(th, NULL);
	CHECK(c.rc == 0);
	CHECK_RC(trunc_to(&m, c.dst, 1000), 0);
	CHECK(same(&m, c.dst, data, 1000));
	(void)back;
	CHECK_RC(mnt_close(&m), 0);
	CHECK(repair_clean(img));
	free(data);
	free(back);
}

int main(int argc, char **argv)
{
	if (argc < 2) {
		fprintf(stderr, "usage: %s TMPDIR\n", argv[0]);
		return 2;
	}
	snprintf(tmp, sizeof(tmp), "%s", argv[1]);
	if (sh("command -v mkfs.xfs >/dev/null && command -v xfs_repair >/dev/null")) {
		printf("SKIPPED: xfsprogs not installed\n");
		return 0;
	}
	RUN(copy_pattern);
	RUN(parallel_writers);
	printf("test_wstress: %d checks, %d failures, %d skipped\n", checks, failures, skipped);
	return failures ? 1 : 0;
}
