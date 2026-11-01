// SPDX-License-Identifier: GPL-2.0-only
/*
 * M3a T11: fuzzing after writes. A workload (create/write/rename/link/symlink/
 * truncate/unlink) builds two base images: one cleanly unmounted and one whose
 * log is dirty (the unmount writes are dropped). Each iteration mutates a few
 * metadata sectors in a copy-on-write overlay, mounts READ-WRITE (replaying a
 * dirty log), then walks the tree and pokes at what it finds with reads and
 * writes: a mutated image may fail with a documented error, but must never
 * crash, hang, leak or return an undocumented code (ASan/UBSan build).
 *
 * usage: test_wfuzz TMPDIR
 *   WFUZZ_ITERS (default 150)  WFUZZ_SEED  WFUZZ_MUT (max mutations, default 3)
 *   WFUZZ_ONLY=n (rerun one iteration, verbose)
 * Built with WFS_XFS_FUZZ_SKIP_CRC (make test-wfuzz-nocrc) checksums always verify,
 * so mutations reach the structural checks instead of dying at the CRC.
 */
#include "wfs_xfs.h"
#include "xfs_sb.h"
#include "ops.h"
#include "xfs_log.h"
#include "xfs_log_priv.h"
#include "xfs_trans.h"
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
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
struct page { uint64_t no; struct page *next; uint8_t d[4096]; };
struct wfs_blkdev {
	int		fd;
	uint64_t	size;
	uint32_t	ss;
	bool		ro;
	struct page	*tab[1024];		/* copy-on-write overlay (iterations) */
	bool		use_ovl, drop;		/* drop: writes are discarded (dirty-log base) */
	uint8_t		*wmap;			/* sectors written, while recording */
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
static struct page *pg_find(wfs_blkdev *d, uint64_t no, bool create)
{
	struct page *p;

	for (p = d->tab[no % 1024]; p; p = p->next)
		if (p->no == no)
			return p;
	if (!create)
		return NULL;
	p = calloc(1, sizeof(*p));
	p->no = no;
	if (no * 4096 < d->size && pread(d->fd, p->d, 4096, (off_t)(no * 4096)) < 0)
		memset(p->d, 0, 4096);
	p->next = d->tab[no % 1024];
	d->tab[no % 1024] = p;
	return p;
}
static void ovl_reset(wfs_blkdev *d)
{
	size_t i;

	for (i = 0; i < 1024; i++)
		while (d->tab[i]) {
			struct page *p = d->tab[i];

			d->tab[i] = p->next;
			free(p);
		}
}
static wfs_err_t h_read(wfs_blkdev *d, uint64_t off, void *buf, size_t len)
{
	uint64_t p, first, last;

	if (off > d->size || len > d->size - off)
		return -WFS_EIO;
	if (len && pread(d->fd, buf, len, (off_t)off) != (ssize_t)len)
		return -WFS_EIO;
	if (d->use_ovl && len) {
		first = off / 4096;
		last = (off + len - 1) / 4096;
		for (p = first; p <= last; p++) {
			struct page *pg = pg_find(d, p, false);
			uint64_t lo = p * 4096 > off ? p * 4096 : off, hi = (p + 1) * 4096 < off + len ? (p + 1) * 4096 : off + len;

			if (pg)
				memcpy((uint8_t *)buf + (lo - off), pg->d + (lo - p * 4096), hi - lo);
		}
	}
	return 0;
}
static wfs_err_t h_write(wfs_blkdev *d, uint64_t off, const void *buf, size_t len)
{
	__atomic_add_fetch(&nwrites, 1, __ATOMIC_RELAXED);
	if (d->ro || fail_writes || off > d->size || len > d->size - off)
		return -WFS_EIO;
	if (d->drop)
		return 0;
	if (d->wmap) {
		uint64_t s;

		for (s = off / 512; s * 512 < off + len; s++)
			d->wmap[s] = 1;
	}
	if (d->use_ovl) {
		uint64_t p;

		for (p = off / 4096; len && p * 4096 < off + len; p++) {
			struct page *pg = pg_find(d, p, true);
			uint64_t lo = p * 4096 > off ? p * 4096 : off, hi = (p + 1) * 4096 < off + len ? (p + 1) * 4096 : off + len;

			memcpy(pg->d + (lo - p * 4096), (const uint8_t *)buf + (lo - off), hi - lo);
		}
		return 0;
	}
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
__attribute__((unused)) static bool same(struct mnt *m, wfs_ino_t f, const uint8_t *want, size_t n)
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



/* ------------------------------------------------------------ the fuzzer -- */
static int bad_codes, ops_done, errs_seen;
static char first_bad[200];
static unsigned cur_iter;
static uint32_t cur_seed;
static bool verbose;

/* a result of 0 or a documented error; anything else is a bug */
static int code(const char *op, int rc)
{
	ops_done++;
	if (!rc)
		return 0;
	if (getenv("WFUZZ_STRICT") && -rc == WFS_EUCLEAN)	/* self-test of the reporting path */
		goto bad;
	switch (-rc) {
	case WFS_EUCLEAN: case WFS_EIO: case WFS_EINVAL: case WFS_ENOENT: case WFS_ENOTDIR:
	case WFS_EISDIR: case WFS_ESTALE: case WFS_ENAMETOOLONG: case WFS_ENOMEM:
	case WFS_EOPNOTSUPP: case WFS_ENODEV: case WFS_ERANGE: case WFS_EFBIG:
	case WFS_EEXIST: case WFS_ENOTEMPTY: case WFS_ENOSPC: case WFS_EROFS: case WFS_EMLINK:
	case WFS_EPERM: case WFS_EBUSY: case WFS_ELOOP: case WFS_EACCES: case WFS_EXDEV:
		errs_seen++;
		return rc;
	}
bad:
	bad_codes++;
	if (!first_bad[0])
		snprintf(first_bad, sizeof(first_bad), "%s returned %d", op, rc);
	return rc;
}

static uint32_t rng;
static uint32_t rnd(void) { rng = rng * 1103515245u + 12345u; return rng >> 8; }

/* a few operations of every kind: the workload that builds the base images */
static void base_workload(struct mnt *m)
{
	wfs_stat st;
	wfs_ino_t a, b, f1, f2, x, l;
	uint8_t *buf = malloc(120000);
	char n[64];
	int k;

	CHECK_RC(mk(m, ROOT, "a", WFS_FT_DIR, 0755, &a, &st), 0);
	CHECK_RC(mk(m, ROOT, "b", WFS_FT_DIR, 0700, &b, &st), 0);
	CHECK_RC(mk(m, a, "f1", WFS_FT_REG, 0644, &f1, &st), 0);
	fill(buf, 120000, 1);
	CHECK_RC(wr(m, f1, 0, buf, 90000), 0);
	CHECK_RC(mk(m, a, "f2", WFS_FT_REG, 0600, &f2, &st), 0);
	CHECK_RC(wr(m, f2, 5000, buf, 3000), 0);
	CHECK_RC(wfs_xfs_op_symlink(m->x, a, "lnk", 3, "f1", 2, 1, 1, &l, &st), 0);
	CHECK_RC(wfs_xfs_op_link(m->x, f2, b, "f2hard", 6), 0);
	CHECK_RC(wfs_xfs_op_rename(m->x, a, "f2", 2, b, "f2x", 3, 0), 0);
	CHECK_RC(trunc_to(m, f1, 40000), 0);
	CHECK_RC(wr(m, f1, 100000, buf, 5000), 0);		/* extend with a hole */
	for (k = 0; k < 120; k++) {				/* a block-form/leaf directory */
		snprintf(n, sizeof(n), "entry-with-a-longish-name-%04d", k);
		CHECK_RC(mk(m, b, n, WFS_FT_REG, 0644, &x, &st), 0);
		if (k % 7 == 0)
			CHECK_RC(wr(m, x, 0, buf, 1 + (size_t)k * 40), 0);
		wfs_xfs_op_forget(m->x, x, 1);
	}
	for (k = 0; k < 120; k += 3) {
		snprintf(n, sizeof(n), "entry-with-a-longish-name-%04d", k);
		CHECK_RC(ul(m, b, n), 0);
	}
	CHECK_RC(mk(m, ROOT, "sub", WFS_FT_DIR, 0755, &x, &st), 0);
	CHECK_RC(rd(m, ROOT, "sub"), 0);
	free(buf);
}

struct ent { char name[256]; wfs_ino_t ino; uint32_t ft; };
struct lst { struct ent e[40]; int n; };
static int lcb(void *ctx, const wfs_dirent *d)
{
	struct lst *l = ctx;

	if (l->n < 40 && d->name_len < 255 && strcmp(d->name, ".") && strcmp(d->name, "..")) {
		memcpy(l->e[l->n].name, d->name, d->name_len);
		l->e[l->n].name[d->name_len] = 0;
		l->e[l->n].ino = d->ino;
		l->e[l->n].ft = d->ftype;
		l->n++;
	}
	return 0;
}

static void poke(struct mnt *m, wfs_ino_t dir, int depth)
{
	struct lst *l = calloc(1, sizeof(*l));
	uint8_t *buf = malloc(70000);
	int i;

	code("readdir", wfs_xfs_op_readdir(m->x, dir, 0, lcb, l));
	for (i = 0; i < l->n; i++) {
		struct ent *e = &l->e[i];
		wfs_ino_t ino;
		wfs_stat st = { 0 };
		size_t got = 0;
		char nn[300], tgt[512];
		size_t tl;

		if (code("lookup", wfs_xfs_op_lookup(m->x, dir, e->name, (uint32_t)strlen(e->name), &ino, &st)))
			continue;
		code("getattr", wfs_xfs_op_getattr(m->x, ino, &st));
		if (S_ISDIR(st.mode) && depth < 3) {
			poke(m, ino, depth + 1);
		} else if (S_ISLNK(st.mode)) {
			code("readlink", wfs_xfs_op_readlink(m->x, ino, tgt, sizeof(tgt), &tl));
		} else if (S_ISREG(st.mode)) {
			code("read", wfs_xfs_op_read(m->x, ino, 0, buf, 65536, &got));
			code("read-tail", wfs_xfs_op_read(m->x, ino, st.size > 100 ? st.size - 100 : 0, buf, 200, &got));
			fill(buf, 70000, rnd());
			code("write", wfs_xfs_op_write(m->x, ino, rnd() % (st.size + 1), buf, 1 + rnd() % 20000, &got));
			code("write-end", wfs_xfs_op_write(m->x, ino, st.size, buf, 1 + rnd() % 70000, &got));
			if (rnd() & 1) {
				wfs_stat t = { 0 };

				t.size = rnd() % (st.size + 1);
				code("truncate", wfs_xfs_op_setattr(m->x, ino, &t, WFS_SET_SIZE));
			}
		}
		if (rnd() % 4 == 0) {
			wfs_stat t = { 0 };

			t.mode = 0640;
			code("chmod", wfs_xfs_op_setattr(m->x, ino, &t, WFS_SET_MODE));
		}
		switch (rnd() % 4) {
		case 0:
			snprintf(nn, sizeof(nn), "renamed-%u", rnd() % 1000);
			code("rename", wfs_xfs_op_rename(m->x, dir, e->name, (uint32_t)strlen(e->name), dir, nn,
							(uint32_t)strlen(nn), 0));
			break;
		case 1:
			snprintf(nn, sizeof(nn), "hard-%u", rnd() % 1000);
			code("link", wfs_xfs_op_link(m->x, ino, dir, nn, (uint32_t)strlen(nn)));
			break;
		case 2:
			code(S_ISDIR(st.mode) ? "rmdir" : "unlink",
			     S_ISDIR(st.mode) ? wfs_xfs_op_rmdir(m->x, dir, e->name, (uint32_t)strlen(e->name))
					      : wfs_xfs_op_unlink(m->x, dir, e->name, (uint32_t)strlen(e->name)));
			break;
		default:
			break;
		}
		wfs_xfs_op_forget(m->x, ino, 1);
	}
	free(buf);
	free(l);
}

static void new_stuff(struct mnt *m)
{
	wfs_ino_t d, f;
	wfs_stat st;
	uint8_t *buf = malloc(300000);
	size_t got;
	int k;

	fill(buf, 300000, rnd());
	if (code("mkdir", mk(m, ROOT, "fuzz-new", WFS_FT_DIR, 0755, &d, &st)))
		goto out;
	for (k = 0; k < 8; k++) {
		char n[32];

		snprintf(n, sizeof(n), "n%d", k);
		if (code("create", mk(m, d, n, WFS_FT_REG, 0644, &f, &st)))
			break;
		code("write-new", wfs_xfs_op_write(m->x, f, 0, buf, 1 + rnd() % 300000, &got));
		wfs_xfs_op_forget(m->x, f, 1);
	}
	code("sync", wfs_xfs_op_sync(m->x));
out:
	free(buf);
}

/* the metadata-looking sectors of a base: written by the workload, with enough zeros to not be file data */
struct base {
	char path[600];
	uint32_t *sec;
	size_t n;
};

static void pick_sectors(struct base *b, const uint8_t *wmap, uint64_t size)
{
	int fd = open(b->path, O_RDONLY);
	uint8_t s[512];
	uint64_t i;

	b->sec = malloc(size / 512 * sizeof(uint32_t));
	b->n = 0;
	for (i = 0; i < size / 512; i++) {
		int z = 0, k;

		if (!wmap[i] || pread(fd, s, 512, (off_t)(i * 512)) != 512)
			continue;
		for (k = 0; k < 512; k++)
			z += s[k] == 0;
		if (z >= 40)
			b->sec[b->n++] = (uint32_t)i;
	}
	close(fd);
}

static void mutate(struct mnt *m, const struct base *b)
{
	int maxm = getenv("WFUZZ_MUT") ? atoi(getenv("WFUZZ_MUT")) : 3;
	int n = 1 + (int)(rnd() % (unsigned)maxm), i;

	for (i = 0; i < n; i++) {
		uint32_t sec = b->sec[rnd() % b->n];
		uint8_t s[512];
		uint64_t off = (uint64_t)sec * 512, p;
		uint32_t kind = rnd() % 6;
		unsigned o = rnd() % 512, k;
		struct page *pg;

		h_read(&m->dev, off, s, 512);
		switch (kind) {
		case 0: for (k = 1 + rnd() % 8; k; k--) s[rnd() % 512] ^= (uint8_t)(1u << (rnd() % 8)); break;
		case 1: { unsigned n1 = 8 + rnd() % 56; memset(s + o, 0, 512 - o < n1 ? 512 - o : n1); } break;
		case 2: if (o + 4 > 512) o = 508; memset(s + o, 0xff, 4); break;
		case 3: if (o + 4 > 512) o = 508; { uint32_t v = rnd(); memcpy(s + o, &v, 4); } break;
		case 4: h_read(&m->dev, (uint64_t)b->sec[rnd() % b->n] * 512, s, 512); break;
		default: memset(s, 0, 512); break;
		}
		if (verbose)
			fprintf(stderr, "  mutate sector %u kind %u off %u\n", sec, kind, o);
		p = off / 4096;
		pg = pg_find(&m->dev, p, true);
		memcpy(pg->d + (off - p * 4096), s, 512);
	}
}

static void on_alarm(int sig)
{
	static const char msg[] = "wfuzz: TIMEOUT (a hang)\n";

	(void)sig;
	if (write(2, msg, sizeof(msg) - 1)) { }
	fprintf(stderr, "  iteration %u seed %u: rerun with WFUZZ_ONLY=%u WFUZZ_SEED=<seed>\n", cur_iter, cur_seed,
		cur_iter);
	_exit(3);
}

static void build_bases(struct base *bs)
{
	const char *names[2] = { "wfz-clean", "wfz-dirty" };
	int k;

	for (k = 0; k < 2; k++) {
		struct mnt m;
		const char *img = mkimg(names[k], PLAIN);
		uint8_t *wmap;
		wfs_stat st;
		wfs_ino_t r;

		CHECK(img != NULL);
		if (!img)
			exit(1);
		snprintf(bs[k].path, sizeof(bs[k].path), "%s", img);
		CHECK_RC(mnt_open(&m, img, "", false), 0);
		if (!m.x)
			exit(1);
		wmap = calloc(1, m.dev.size / 512 + 1);
		m.dev.wmap = wmap;
		base_workload(&m);
		if (k == 1) {
			/* force the log (committed transactions reach it), then the rest of the writes,
			 * including the unmount record, vanish: a dirty log is left behind */
			wfs_ino_t b;

			CHECK_RC(lk(&m, ROOT, "b", &b, &st), 0);
			CHECK_RC(lk(&m, b, "entry-with-a-longish-name-0119", &r, &st), 0);
			CHECK_RC(wfs_xfs_op_fsync(m.x, r, 0), 0);
			m.dev.drop = true;
		}
		CHECK_RC(mnt_close(&m), 0);
		if (k == 0)
			CHECK(repair_clean(img));
		else {
			char o[64];

			sh_out(o, sizeof(o), "xfs_logprint -t '%s' 2>/dev/null | grep -o 'state: <[A-Z]*>'", img);
			printf("dirty base log: %s\n", o);
		}
		pick_sectors(&bs[k], wmap, (uint64_t)320 << 20);
		printf("base %s: %zu metadata sectors to mutate\n", names[k], bs[k].n);
		free(wmap);
	}
}

static void fuzz_main(void)
{
	struct base bs[2];
	unsigned iters = getenv("WFUZZ_ITERS") ? (unsigned)atoi(getenv("WFUZZ_ITERS")) : 150;
	uint32_t seed = getenv("WFUZZ_SEED") ? (uint32_t)strtoul(getenv("WFUZZ_SEED"), NULL, 0) : 0x57465A31u;
	long only = getenv("WFUZZ_ONLY") ? atol(getenv("WFUZZ_ONLY")) : -1;
	unsigned it, mounted = 0, refused = 0, lowest = 0;

	build_bases(bs);
	signal(SIGALRM, on_alarm);
	for (it = 0; it < iters; it++) {
		struct mnt m;
		const struct base *b = &bs[it & 1];
		int rc;

		if (only >= 0 && (long)it != only)
			continue;
		verbose = only >= 0;
		cur_iter = it;
		cur_seed = seed;
		rng = seed + it * 2654435761u;
		memset(&m, 0, sizeof(m));
		m.dev.fd = open(b->path, O_RDONLY);	/* the base is never written: the overlay takes it */
		m.dev.size = (uint64_t)320 << 20;
		m.dev.ss = 512;
		m.dev.use_ovl = true;
		m.p[WFS_DEV_DATA] = &m.dev;
		if (verbose)
			fprintf(stderr, "iteration %u (base %s)\n", it, b->path);
		fprintf(stderr, "wfuzz: iteration %u\r", it);
		mutate(&m, b);
		alarm(40);
		rc = wfs_xfs_mount(&api, m.p, "bufcache_bytes=2097152,inodecache_entries=64", &m.x);
		if (rc) {
			code("mount", rc);
			refused++;
		} else {
			mounted++;
			poke(&m, ROOT, 0);
			new_stuff(&m);
			code("unmount", wfs_xfs_unmount(m.x));
			m.x = NULL;
		}
		alarm(0);
		close(m.dev.fd);
		ovl_reset(&m.dev);
		if (bad_codes) {
			fprintf(stderr, "\nwfuzz: iteration %u (seed %u): %s\n  rerun: WFUZZ_ONLY=%u WFUZZ_SEED=%u\n", it, seed,
				first_bad, it, seed);
			failures++;
			break;
		}
		(void)lowest;
	}
	fprintf(stderr, "\n");
	printf("wfuzz: %u iterations, %u mounted, %u refused, %d ops, %d documented errors, %d undocumented\n",
	       it, mounted, refused, ops_done, errs_seen, bad_codes);
	CHECK(bad_codes == 0);
	free(bs[0].sec);
	free(bs[1].sec);
}

int main(int argc, char **argv)
{
	if (argc < 2) {
		fprintf(stderr, "usage: %s TMPDIR\n", argv[0]);
		return 2;
	}
	setvbuf(stdout, NULL, _IOLBF, 0);
	/* a private directory per process: san and nocrc builds may run in parallel */
	snprintf(tmp, sizeof(tmp), "%s/test_wfuzz.%d", argv[1], (int)getpid());
	if (sh("mkdir -p '%s'", tmp))
		return 2;
	if (sh("command -v mkfs.xfs >/dev/null && command -v xfs_repair >/dev/null && command -v xfs_logprint >/dev/null")) {
		printf("SKIPPED: xfsprogs not installed\n");
		return 0;
	}
	RUN(fuzz_main);
	if (!failures)
		sh("rm -rf '%s'", tmp);
	else
		printf("images kept in %s\n", tmp);
	printf("test_wfuzz: %d checks, %d failures, %d skipped\n", checks, failures, skipped);
	return failures ? 1 : 0;
}
