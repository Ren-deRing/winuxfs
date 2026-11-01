// SPDX-License-Identifier: GPL-2.0-only
/*
 * M3a T9 crash-consistency harness: every block write/flush of a workload is recorded; crash images are rebuilt from prefixes (+ subsets of the unflushed epoch), replayed, and checked.
 * usage: test_crash TMPDIR
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
/* Recording of every device write and flush, in order (T9). */
struct ev { uint64_t off; uint32_t len; uint8_t *data; };	/* data == NULL: flush */
static struct ev *evs;
static size_t nevs, capevs;
static bool recording;
static void rec(uint64_t off, const void *buf, size_t len, bool flush)
{
	if (!recording)
		return;
	if (nevs == capevs) {
		capevs = capevs ? capevs * 2 : 4096;
		evs = realloc(evs, capevs * sizeof(*evs));
	}
	evs[nevs].off = off;
	evs[nevs].len = (uint32_t)len;
	evs[nevs].data = NULL;
	if (!flush) {
		evs[nevs].data = malloc(len);
		memcpy(evs[nevs].data, buf, len);
	}
	nevs++;
}
static wfs_err_t h_write(wfs_blkdev *d, uint64_t off, const void *buf, size_t len)
{
	nwrites++;
	rec(off, buf, len, false);
	if (d->ro || fail_writes || off > d->size || len > d->size - off)
		return -WFS_EIO;
	return pwrite(d->fd, buf, len, (off_t)off) == (ssize_t)len ? 0 : -WFS_EIO;
}
static wfs_err_t h_flush(wfs_blkdev *d)
{
	(void)d;
	nflushes++;
	rec(0, NULL, 0, true);
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

__attribute__((unused)) static int lk(struct mnt *m, wfs_ino_t d, const char *n, wfs_ino_t *o, wfs_stat *st)
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

static int trunc_size(struct mnt *m, wfs_ino_t f, uint64_t size)
{
	wfs_stat s = { 0 };

	s.size = size;
	return wfs_xfs_op_setattr(m->x, f, &s, WFS_SET_SIZE);
}

/* ---------------------------------------------------------------- digest -- */
static uint64_t mix(uint64_t h, const void *p, size_t n)
{
	const uint8_t *b = p;
	size_t i;

	for (i = 0; i < n; i++)
		h = (h ^ b[i]) * 0x100000001b3ull;
	return h;
}

/*
 * One file (tol_ino) is excluded from the exact digest: an overwrite in place is not
 * atomic (the device may persist any subset of its sectors), so its bytes may be a
 * mix of the states of the operations in flight. Every byte of it must still come
 * from one of those states (snapshots below); nothing else (no stale blocks).
 */
static wfs_ino_t tol_ino;
static uint8_t *snap[201];
static uint64_t snaplen[201];
static bool snapok[201];
static char *cap_buf; static size_t cap_len, cap_cap; static bool cap_on;
static void cap_add(const char *line)
{
	size_t n = strlen(line);

	if (!cap_on)
		return;
	if (cap_len + n + 2 > cap_cap) {
		cap_cap = (cap_len + n + 2) * 2;
		cap_buf = realloc(cap_buf, cap_cap);
	}
	memcpy(cap_buf + cap_len, line, n);
	cap_len += n;
	cap_buf[cap_len++] = '\n';
	cap_buf[cap_len] = 0;
}

struct dg {
	struct mnt *m;
	uint64_t acc;		/* XOR of per-entry hashes: order independent */
	char path[1024];
	int err;
};
static void dg_dir(struct dg *g, wfs_ino_t dir);

struct rdctx {
	struct dg *g;
	wfs_ino_t dir;
	struct { char name[260]; wfs_ino_t ino; uint32_t ftype; } ent[1024];
	int n;
};
static int rd_cb(void *ctx, const wfs_dirent *e)
{
	struct rdctx *c = ctx;

	if (e->name_len == 1 && e->name[0] == '.')
		return 0;
	if (e->name_len == 2 && e->name[0] == '.' && e->name[1] == '.')
		return 0;
	if (c->n >= 1024)
		return 1;
	memcpy(c->ent[c->n].name, e->name, e->name_len);
	c->ent[c->n].name[e->name_len] = 0;
	c->ent[c->n].ino = e->ino;
	c->ent[c->n].ftype = e->ftype;
	c->n++;
	return 0;
}

static void dg_dir(struct dg *g, wfs_ino_t dir)
{
	struct rdctx *c = calloc(1, sizeof(*c));
	size_t plen = strlen(g->path);
	int i;

	c->g = g;
	if (wfs_xfs_op_readdir(g->m->x, dir, 0, rd_cb, c)) {
		g->err = 1;
		free(c);
		return;
	}
	for (i = 0; i < c->n && !g->err; i++) {
		wfs_stat st;
		uint64_t h = 0xcbf29ce484222325ull;
		uint8_t buf[8192];
		size_t got;
		uint64_t off;

		if (wfs_xfs_op_getattr(g->m->x, c->ent[i].ino, &st)) {
			g->err = 1;
			break;
		}
		snprintf(g->path + plen, sizeof(g->path) - plen, "/%s", c->ent[i].name);
		h = mix(h, g->path, strlen(g->path));
		h = mix(h, &st.ftype, sizeof(st.ftype));
		h = mix(h, &st.mode, sizeof(st.mode));
		h = mix(h, &st.nlink, sizeof(st.nlink));
		h = mix(h, &st.size, sizeof(st.size));
		if (st.ftype == WFS_FT_REG && c->ent[i].ino != tol_ino) {
			for (off = 0; off < st.size; off += got) {
				if (wfs_xfs_op_read(g->m->x, c->ent[i].ino, off, buf, sizeof(buf), &got) || !got) {
					g->err = 1;
					break;
				}
				h = mix(h, buf, got);
			}
		} else if (st.ftype == WFS_FT_LNK) {
			char t[1100];

			if (wfs_xfs_op_readlink(g->m->x, c->ent[i].ino, t, sizeof(t), &got)) {
				g->err = 1;
				break;
			}
			h = mix(h, t, got);
		}
		{
			char ln[1400];

			snprintf(ln, sizeof(ln), "%s ft=%u mode=%o nl=%u size=%llu h=%016llx", g->path, st.ftype, st.mode, st.nlink, (unsigned long long)st.size, (unsigned long long)h);
			cap_add(ln);
		}
		g->acc ^= h;
		if (st.ftype == WFS_FT_DIR)
			dg_dir(g, c->ent[i].ino);
		g->path[plen] = 0;
	}
	free(c);
}

/* Digest of the whole tree (0 + err on failure). */
static bool digest(struct mnt *m, uint64_t *out)
{
	struct dg g = { .m = m };


	dg_dir(&g, ROOT);
	*out = g.acc;
	return !g.err;
}

static void take_snap(struct mnt *m, int i)
{
	wfs_stat st;
	size_t got = 0;

	free(snap[i]);
	snap[i] = NULL;
	snapok[i] = false;
	if (!tol_ino || wfs_xfs_op_getattr(m->x, tol_ino, &st) || st.nlink == 0)
		return;
	snaplen[i] = st.size;
	snap[i] = malloc(st.size ? st.size : 1);
	if (st.size && (wfs_xfs_op_read(m->x, tol_ino, 0, snap[i], st.size, &got) || got != st.size))
		return;
	snapok[i] = true;
}

/* The tolerant file of a recovered tree: every byte comes from a state in [L, U]. */
static bool tol_content_ok(struct mnt *m, size_t L, size_t U)
{
	wfs_stat st;
	uint8_t *b;
	size_t got = 0, j, k;
	bool ok = true;

	if (!tol_ino || wfs_xfs_op_getattr(m->x, tol_ino, &st) || st.nlink == 0)
		return true;			/* absent: the structure digest decided */
	b = malloc(st.size ? st.size : 1);
	if (st.size && (wfs_xfs_op_read(m->x, tol_ino, 0, b, st.size, &got) || got != st.size)) {
		free(b);
		return false;
	}
	for (k = 0; k < st.size && ok; k++) {
		bool seen = false;

		for (j = L; j <= U && !seen; j++) {
			if (snapok[j] && k < snaplen[j] && snap[j][k] == b[k])
				seen = true;
			/* a shrinking truncate zeroes the tail of the new last block before its
			 * transaction commits (as the kernel does): zeros past an in-flight EOF */
			if (b[k] == 0 && j > L && snapok[j] && snapok[j - 1] && snaplen[j] < snaplen[j - 1] &&
			    k >= snaplen[j] && k < ((snaplen[j] + 4095) & ~(uint64_t)4095))
				seen = true;
		}
		if (!seen && k < st.size)
			ok = false;
	}
	free(b);
	return ok;
}

/* -------------------------------------------------------------- workload -- */
#define MAXOPS 200
static char *listing[MAXOPS + 1];
static uint64_t dig[MAXOPS + 1];		/* dig[i]: tree after op i (dig[0]: empty) */
static size_t ev_start[MAXOPS + 1], ev_done[MAXOPS + 1];
static int nops, synced[MAXOPS + 1];	/* synced[i]: a sync returned after op i */
static size_t sync_ev[MAXOPS + 1];

static void begin_op(struct mnt *m, int i)
{
	(void)m;
	ev_start[i] = nevs;
}
static void end_op(struct mnt *m, int i, bool do_sync)
{
	ev_done[i] = nevs;
	if (do_sync) {
		CHECK_RC(wfs_xfs_op_sync(m->x), 0);
		synced[i] = 1;
		sync_ev[i] = nevs;
	}
	cap_on = true; cap_len = 0; if (cap_buf) cap_buf[0] = 0;
	CHECK(digest(m, &dig[i]));
	cap_on = false;
	take_snap(m, i);
	free(listing[i]);
	listing[i] = cap_buf ? strdup(cap_buf) : strdup("");
	nops = i;
}

static void wl_write(struct mnt *m, wfs_ino_t f, uint64_t off, size_t n, uint32_t seed)
{
	uint8_t *b = malloc(n);
	size_t d = 0, i;
	uint32_t s = seed;

	for (i = 0; i < n; i++) {
		s = s * 1664525u + 1013904223u;
		b[i] = (uint8_t)(s >> 24);
	}
	CHECK_RC(wfs_xfs_op_write(m->x, f, off, b, n, &d), 0);
	free(b);
}

/* ~70 operations covering every T6-T8 operation; sync after every 3rd (or all). */
static void workload(struct mnt *m, bool sync_each)
{
	wfs_ino_t a, f1, f2, sub, l, t, x;
	wfs_stat st;
	char n[64];
	int i = 0, k;
#define OP(code) do { begin_op(m, ++i); code; end_op(m, i, sync_each || i % 3 == 0); } while (0)

	cap_on = true; cap_len = 0; if (cap_buf) cap_buf[0] = 0;
	digest(m, &dig[0]);
	cap_on = false;
	free(listing[0]);
	listing[0] = cap_buf ? strdup(cap_buf) : strdup("");
	OP(CHECK_RC(mk(m, ROOT, "a", WFS_FT_DIR, 0755, &a, &st), 0));
	OP(CHECK_RC(mk(m, a, "f1", WFS_FT_REG, 0644, &f1, &st), 0); tol_ino = f1);
	OP(wl_write(m, f1, 0, 9000, 1));
	OP(CHECK_RC(mk(m, a, "f2", WFS_FT_REG, 0600, &f2, &st), 0));
	OP(wl_write(m, f2, 0, 100, 2));
	OP(wl_write(m, f1, 5000, 3000, 3));			/* overwrite */
	OP(CHECK_RC(mk(m, a, "sub", WFS_FT_DIR, 0755, &sub, &st), 0));
	OP(CHECK_RC(wfs_xfs_op_symlink(m->x, a, "lnk", 3, "f1", 2, 1, 1, &l, &st), 0));
	OP(CHECK_RC(wfs_xfs_op_link(m->x, f2, a, "f2hard", 6), 0));
	OP(CHECK_RC(wfs_xfs_op_rename(m->x, a, "f2", 2, sub, "f2x", 3, 0), 0));
	OP(CHECK_RC(trunc_size(m, f1, 2000), 0));
	OP(wl_write(m, f1, 20000, 5000, 4));			/* extend with a hole */
	OP(CHECK_RC(wfs_xfs_op_unlink(m->x, a, "f2hard", 6), 0));
	for (k = 0; k < 30; k++)				/* directory growth */
		OP(snprintf(n, sizeof(n), "grow-file-with-long-name-%03d", k);
		   CHECK_RC(mk(m, a, n, WFS_FT_REG, 0644, &x, &st), 0); wfs_xfs_op_forget(m->x, x, 1));
	OP(wl_write(m, f1, 0, 200000, 5));			/* multi-extent */
	OP(CHECK_RC(wfs_xfs_op_rename(m->x, ROOT, "a", 1, ROOT, "a2", 2, 0), 0));
	for (k = 0; k < 30; k += 2)
		OP(snprintf(n, sizeof(n), "grow-file-with-long-name-%03d", k);
		   CHECK_RC(wfs_xfs_op_unlink(m->x, a, n, (uint32_t)strlen(n)), 0));
	OP(CHECK_RC(trunc_size(m, f1, 0), 0));
	OP(CHECK_RC(wfs_xfs_op_unlink(m->x, a, "lnk", 3), 0));
	OP(CHECK_RC(wfs_xfs_op_unlink(m->x, sub, "f2x", 3), 0));
	OP(CHECK_RC(wfs_xfs_op_rmdir(m->x, a, "sub", 3), 0));
	(void)t;
	wfs_xfs_op_forget(m->x, f1, 1); wfs_xfs_op_forget(m->x, f2, 1);
	wfs_xfs_op_forget(m->x, a, 1); wfs_xfs_op_forget(m->x, sub, 1); wfs_xfs_op_forget(m->x, l, 1);
#undef OP
}

/* ------------------------------------------------------------ crash points -- */
static uint32_t rng = 1;
static uint32_t rnd(void) { rng = rng * 1103515245u + 12345u; return rng >> 8; }

/* Build the image a crash at event position p leaves: all writes before the last
 * flush at or before p, plus (mode 0: none, 1: all, 2: a random half) of the
 * writes after it. */
static bool build_image(const char *base, const char *out, size_t p, int mode)
{
	size_t f = 0, i;
	int fd;

	for (i = 0; i < p && i < nevs; i++)
		if (!evs[i].data)
			f = i + 1;		/* writes before index f are durable */
	if (sh("cp --sparse=always '%s' '%s'", base, out))
		return false;
	fd = open(out, O_RDWR);
	if (fd < 0)
		return false;
	for (i = 0; i < p && i < nevs; i++) {
		if (!evs[i].data)
			continue;
		if (i >= f && (mode == 0 || (mode == 2 && (rnd() & 1))))
			continue;
		if (pwrite(fd, evs[i].data, evs[i].len, (off_t)evs[i].off) != (ssize_t)evs[i].len) {
			close(fd);
			return false;
		}
	}
	close(fd);
	return true;
}

static void crash_run(bool sync_each, const char *name)
{
	char base[600], work[600], crash[600];
	size_t maxpts = getenv("WCRASH_POINTS") ? (size_t)atol(getenv("WCRASH_POINTS")) : 120;
	size_t p, stride, npts = 0, bad = 0;
	const char *img = mkimg(name, PLAIN);
	struct mnt m;
	uint64_t d;
	int i;

	CHECK(img != NULL);
	if (!img)
		return;
	snprintf(base, sizeof(base), "%s/%s.base", tmp, name);
	snprintf(work, sizeof(work), "%s/%s.work", tmp, name);
	snprintf(crash, sizeof(crash), "%s/%s.crash", tmp, name);
	CHECK(sh("cp --sparse=always '%s' '%s' && cp --sparse=always '%s' '%s'", img, base, img, work) == 0);
	nevs = 0;
	tol_ino = 0;
	memset(synced, 0, sizeof(synced));
	recording = true;
	CHECK_RC(mnt_open(&m, work, "", false), 0);
	if (!m.x) {
		recording = false;
		return;
	}
	workload(&m, sync_each);
	CHECK_RC(mnt_close(&m), 0);
	recording = false;
	printf("  %s: %d ops, %zu device events\n", name, nops, nevs);
	/* the digest must be stable: remounting the cleanly unmounted image gives the last one */
	CHECK_RC(mnt_open(&m, work, "", false), 0);
	if (m.x) {
		CHECK(digest(&m, &d) && d == dig[nops]);
		if (d != dig[nops])
			fprintf(stderr, "  clean remount digest %016llx != last live %016llx\n",
				(unsigned long long)d, (unsigned long long)dig[nops]);
		CHECK_RC(mnt_close(&m), 0);
	}

	stride = nevs / maxpts ? nevs / maxpts : 1;
	for (p = 0; p <= nevs; p++) {
		bool special = false;
		int modes[3], nm = 1, k;
		size_t L = 0, U = 0, j;

		for (i = 1; i <= nops; i++)
			if (synced[i] && (sync_ev[i] == p || sync_ev[i] == p + 1))
				special = true;
		if (!special && p % stride)
			continue;
		if (special || (p / stride) % 4 == 0) {
			modes[0] = 0; modes[1] = 1; modes[2] = 2; nm = 3;
		} else {
			modes[0] = (int)(p % 3);
		}
		for (i = 1; i <= nops; i++) {
			if (synced[i] && sync_ev[i] <= p)
				L = (size_t)i;
			if (ev_start[i] <= p)
				U = (size_t)i;
		}
		for (k = 0; k < nm; k++) {
			bool ok = false, valid = false;

			npts++;
			rng = (uint32_t)(p * 2654435761u + (uint32_t)k);
			if (!build_image(base, crash, p, modes[k])) {
				CHECK(!"build_image");
				continue;
			}
			if (mnt_open(&m, crash, "", false) != 0) {
				fprintf(stderr, "%s: crash @%zu mode %d: mount failed\n", name, p, modes[k]);
				bad++;
				CHECK(!"mount after crash");
				continue;
			}
			ok = digest(&m, &d);
			for (j = L; j <= U && ok; j++)
				if (dig[j] == d)
					valid = true;
			if (valid && !tol_content_ok(&m, L, U))
				valid = false;
			if (!ok || !valid) {
				fprintf(stderr, "%s: crash @%zu mode %d: tree matches no state in ops %zu..%zu%s\n",
					name, p, modes[k], L, U, ok ? "" : " (unreadable)");
				if (getenv("WCRASH_DEBUG") && bad < 3) {
					char f1[700], f2[700];
					FILE *fp;

					sh("cp '%s' '%s/tree-%s-%zu-%d.img'", crash, tmp, name, p, modes[k]);
					cap_on = true; cap_len = 0; if (cap_buf) cap_buf[0] = 0;
					digest(&m, &d);
					cap_on = false;
					snprintf(f1, sizeof(f1), "%s/got.txt", tmp);
					fp = fopen(f1, "w");
					if (fp) { fputs(cap_buf ? cap_buf : "", fp); fclose(fp); }
					for (j = L; j <= U; j++) {
						snprintf(f2, sizeof(f2), "%s/op%zu.txt", tmp, j);
						fp = fopen(f2, "w");
						if (fp) { fputs(listing[j], fp); fclose(fp); }
						fprintf(stderr, "  --- diff of the recovered tree vs state after op %zu (sorted):\n", j);
						sh("sort '%s' >'%s.s'; sort '%s' >'%s.s'; diff '%s.s' '%s.s' | head -8 >&2", f2, f2, f1, f1, f2, f1);
					}
				}
				bad++;
				CHECK(!"tree state");
			}
			if (mnt_close(&m) != 0) {
				fprintf(stderr, "%s: crash @%zu mode %d: unmount failed\n", name, p, modes[k]);
				bad++;
				CHECK(!"unmount after replay");
			}
			if (!repair_clean(crash)) {
				fprintf(stderr, "%s: crash @%zu mode %d: xfs_repair -n failed\n", name, p, modes[k]);
				if (bad < 3)
					sh("cp '%s' '%s/fail-%s-%zu-%d.img'", crash, tmp, name, p, modes[k]);
				bad++;
				CHECK(!"repair after replay");
			}
		}
	}
	printf("  %s: %zu crash images, %zu bad\n", name, npts, bad);
	for (p = 0; p < nevs; p++)
		free(evs[p].data);
	nevs = 0;
}

static void crash_sync_each_op(void) { crash_run(true, "cs"); }
static void crash_sync_every_third(void) { crash_run(false, "c3"); }

int main(int argc, char **argv)
{
	if (argc < 2) {
		fprintf(stderr, "usage: test_crash TMPDIR\n");
		return 2;
	}
	snprintf(tmp, sizeof(tmp), "%s/test_crash.%d", argv[1], (int)getpid());
	if (sh("mkdir -p '%s'", tmp))
		return 2;
	RUN(crash_sync_each_op);
	RUN(crash_sync_every_third);
	if (!failures)
		sh("rm -rf '%s'", tmp);
	else
		printf("images kept in %s\n", tmp);
	printf("test_crash: %d checks, %d failures, %d skipped\n", checks, failures, skipped);
	return failures ? 1 : 0;
}
