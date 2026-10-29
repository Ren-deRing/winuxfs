// SPDX-License-Identifier: GPL-2.0-only
/*
 * Task 14 tests: several threads on one mount (run under TSan/ASan), and
 * the caches squeezed to their minimum so eviction races constantly.
 *
 *   test_concurrent FIXDIR
 *     FIXDIR   generated matrix (tools/gen-fixtures.sh), as for test_read
 *
 * A walk fingerprints every entry (path, type, mode, nlink, size, content
 * hash, link target) with the backend ops only. The single-threaded
 * fingerprint is the baseline (test_read checks that tree against the
 * manifest hashes); every concurrent walk must reproduce it exactly.
 * Skips visibly when the fixtures are missing.
 */
#include <dirent.h>	/* before the shim: its DT_* macros would clash with glibc's enum */
#include "wfs_xfs.h"
#include "icache.h"
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int failures, checks;
static pthread_mutex_t cklock = PTHREAD_MUTEX_INITIALIZER;
#define CHECK(c) do { pthread_mutex_lock(&cklock); checks++; if (!(c)) { failures++; \
	fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #c); } \
	pthread_mutex_unlock(&cklock); } while (0)
#define CHECK_EQ(a, b) do { long long _a = (a), _b = (b); pthread_mutex_lock(&cklock); checks++; \
	if (_a != _b) { failures++; fprintf(stderr, "%s:%d: %s = %lld, want %lld\n", \
		__FILE__, __LINE__, #a, _a, _b); } pthread_mutex_unlock(&cklock); } while (0)

/* The host side is called from many threads: no unsynchronised state. */
struct wfs_blkdev { int fd; uint64_t size; };
static long nwrites;
static void h_log(int level, const char *msg) { if (getenv("RD_VERBOSE")) fprintf(stderr, "[%d] %s\n", level, msg); }
static uint64_t h_size(wfs_blkdev *d) { return d->size; }
static uint32_t h_ss(wfs_blkdev *d) { (void)d; return 512; }
static int h_ro(wfs_blkdev *d) { (void)d; return 1; }
static wfs_err_t h_read(wfs_blkdev *d, uint64_t off, void *buf, size_t len)
{
	if (off > d->size || len > d->size - off)
		return -WFS_EINVAL;
	return pread(d->fd, buf, len, (off_t)off) == (ssize_t)len ? 0 : -WFS_EIO;
}
static wfs_err_t h_write(wfs_blkdev *d, uint64_t o, const void *b, size_t l)
{ (void)d; (void)o; (void)b; (void)l; __atomic_add_fetch(&nwrites, 1, __ATOMIC_SEQ_CST); return -WFS_EROFS; }
static wfs_err_t h_flush(wfs_blkdev *d) { (void)d; __atomic_add_fetch(&nwrites, 1, __ATOMIC_SEQ_CST); return 0; }
static const wfs_host_api api = {
	.abi_version = WFS_ABI_VERSION, .size = sizeof(wfs_host_api),
	.blk_size = h_size, .blk_sector_size = h_ss, .blk_read_only = h_ro,
	.blk_read = h_read, .blk_write = h_write, .blk_flush = h_flush, .log = h_log,
};

static size_t mem_base;
static int live;
#define NTHREADS	8
#define TINY_OPTS	"bufcache_bytes=8388608,inodecache_entries=64"

struct mnt {
	wfs_blkdev dev[WFS_DEV_COUNT];
	wfs_blkdev *p[WFS_DEV_COUNT];
	struct wfs_xfs *x;
};

static int exists(const char *p) { struct stat st; return stat(p, &st) == 0; }

static int mnt_open(struct mnt *m, const char *base, const char *opts)
{
	const char *suffix[WFS_DEV_COUNT] = { ".img", ".log.img", ".rt.img" };
	int i;

	memset(m, 0, sizeof(*m));
	for (i = 0; i < WFS_DEV_COUNT; i++) {
		char path[800];
		struct stat st;

		m->dev[i].fd = -1;
		snprintf(path, sizeof(path), "%s%s", base, suffix[i]);
		if (!exists(path))
			continue;
		m->dev[i].fd = open(path, O_RDONLY);
		if (m->dev[i].fd < 0 || fstat(m->dev[i].fd, &st))
			return 12345;
		m->dev[i].size = (uint64_t)st.st_size;
		m->p[i] = &m->dev[i];
	}
	live++;
	return wfs_xfs_mount(&api, m->p, opts, &m->x);
}

/* Unmount; every reference must be back (design 7.2) and no memory leaked. */
static void mnt_close(struct mnt *m)
{
	int i;

	CHECK_EQ((long long)wfs_xfs_icache_refs(m->x), 0);
	CHECK(wfs_xfs_unmount(m->x) == 0);
	for (i = 0; i < WFS_DEV_COUNT; i++)
		if (m->dev[i].fd >= 0)
			close(m->dev[i].fd);
	if (--live == 0)
		CHECK(wfs_mem_used() == mem_base);
}

/* ---- the walk ---- */

static uint64_t mix(uint64_t h, const void *p, size_t n)	/* FNV-1a */
{
	const uint8_t *b = p;

	while (n--)
		h = (h ^ *b++) * 0x100000001b3ull;
	return h;
}

static uint64_t fin(uint64_t h)	/* make the per-entry hashes summable */
{
	h ^= h >> 33; h *= 0xff51afd7ed558ccdull; h ^= h >> 33;
	h *= 0xc4ceb9fe1a85ec53ull; return h ^ (h >> 33);
}

struct de { char name[256]; uint32_t len; wfs_ino_t ino; uint32_t ftype; };
struct dlist { struct de *v; size_t n, cap; int oom; };

static int collect(void *ctx, const wfs_dirent *e)
{
	struct dlist *l = ctx;
	struct de *d;

	if (l->n == l->cap) {
		struct de *nv = realloc(l->v, (l->cap ? l->cap * 2 : 64) * sizeof(*nv));

		if (!nv) { l->oom = 1; return 1; }
		l->v = nv; l->cap = l->cap ? l->cap * 2 : 64;
	}
	d = &l->v[l->n++];
	d->len = e->name_len < 255 ? e->name_len : 255;
	memcpy(d->name, e->name, d->len);
	d->name[d->len] = 0;
	d->ino = e->ino; d->ftype = e->ftype;
	return 0;
}

/* Whole directory: the callback never stops the iteration. */
static int list_dir(struct wfs_xfs *x, wfs_ino_t dir, struct dlist *l)
{
	int err = wfs_xfs_op_readdir(x, dir, 0, collect, l);

	return err ? err : (l->oom ? -WFS_ENOMEM : 0);
}

#define FILE_HEAD ((uint64_t)4 << 20)	/* a 4 GiB sparse file is not read in full */
#define FILE_TAIL ((uint64_t)64 << 10)
#define CHUNK ((size_t)65536 + 13)

struct walker {
	struct wfs_xfs *x;
	uint64_t fp;		/* sum of per-entry hashes */
	uint64_t entries;
	int err;
};

static int hash_range(struct wfs_xfs *x, wfs_ino_t w, uint64_t off, uint64_t end, uint64_t *h)
{
	uint8_t *buf = malloc(CHUNK);

	if (!buf)
		return -WFS_ENOMEM;
	while (off < end) {
		size_t want = end - off < CHUNK ? (size_t)(end - off) : CHUNK, done = 12345;
		int err = wfs_xfs_op_read(x, w, off, buf, want, &done);

		if (err || done == 0 || done > want) {
			free(buf);
			return err ? err : -WFS_EIO;
		}
		*h = mix(*h, buf, done);
		off += done;
	}
	free(buf);
	return 0;
}

static void walk(struct walker *wk, wfs_ino_t dir, const char *path, int depth)
{
	struct dlist l = { 0 };
	size_t i;
	int err = list_dir(wk->x, dir, &l);

	if (err) { wk->err = err; return; }
	for (i = 0; i < l.n && !wk->err; i++) {
		struct de *d = &l.v[i];
		char p[4096], tgt[4100];
		wfs_stat st;
		wfs_ino_t w;
		uint64_t h = 1469598103934665603ull;
		size_t tl;

		snprintf(p, sizeof(p), "%s/%s", path, d->name);
		err = wfs_xfs_op_lookup(wk->x, dir, d->name, d->len, &w, &st);
		if (err) { wk->err = err; break; }
		if (w != d->ino) { wk->err = -9998; wfs_xfs_op_forget(wk->x, w, 1); break; }
		h = mix(h, p, strlen(p));
		h = mix(h, &st.mode, sizeof(st.mode));
		h = mix(h, &st.nlink, sizeof(st.nlink));
		if (S_ISREG(st.mode)) {
			h = mix(h, &st.size, sizeof(st.size));
			err = hash_range(wk->x, w, 0, st.size < FILE_HEAD ? st.size : FILE_HEAD, &h);
			if (!err && st.size > FILE_HEAD)
				err = hash_range(wk->x, w, st.size > FILE_TAIL ? st.size - FILE_TAIL : 0, st.size, &h);
		} else if (S_ISLNK(st.mode)) {
			tl = 0;
			err = wfs_xfs_op_readlink(wk->x, w, tgt, sizeof(tgt), &tl);
			if (!err)
				h = mix(h, tgt, tl);
		} else if (S_ISDIR(st.mode) && depth < 64) {
			walk(wk, w, p, depth + 1);
		}
		if (err) wk->err = err;
		wk->fp += fin(h);
		wk->entries++;
		wfs_xfs_op_forget(wk->x, w, 1);
	}
	free(l.v);
}

static void *walk_thread(void *arg)
{
	struct walker *wk = arg;
	wfs_stat st;

	/* start somewhere different per thread so they meet in the middle */
	CHECK_EQ(wfs_xfs_op_getattr(wk->x, 1, &st), 0);
	walk(wk, 1, "", 0);
	return NULL;
}

static void run_walkers(struct wfs_xfs *x, int n, struct walker *out)
{
	pthread_t t[NTHREADS];
	int i;

	for (i = 0; i < n; i++) {
		out[i] = (struct walker){ .x = x };
		CHECK(pthread_create(&t[i], NULL, walk_thread, &out[i]) == 0);
	}
	for (i = 0; i < n; i++)
		pthread_join(t[i], NULL);
}

static void check_quiesced(struct mnt *m)
{
	CHECK_EQ((long long)wfs_xfs_icache_refs(m->x), 0);
	CHECK_EQ((long long)wfs_xfs_icache_nlookup(m->x), 0);
}

/* Baseline: one thread, default caches. Returns 0 and fills fp and entries. */
static int baseline(const char *base, uint64_t *fp, uint64_t *entries)
{
	struct mnt m;
	struct walker w;
	int rc = mnt_open(&m, base, NULL);

	if (rc)
		return rc;
	w = (struct walker){ .x = m.x };
	walk_thread(&w);
	CHECK_EQ(w.err, 0);
	check_quiesced(&m);
	*fp = w.fp; *entries = w.entries;
	mnt_close(&m);
	return w.err;
}

/* Images that mount (v4 etc. are refused by design and skipped). */
static int names(const char *fixdir, char out[][700], int max)
{
	char cmd[900], line[800];
	FILE *f;
	int n = 0;

	snprintf(cmd, sizeof(cmd), "ls '%s'/*.manifest.json 2>/dev/null", fixdir);
	f = popen(cmd, "r");
	while (f && n < max && fgets(line, sizeof(line), f)) {
		char *e = strstr(line, ".manifest.json");

		if (e) { *e = 0; snprintf(out[n++], 700, "%s", line); }
	}
	if (f)
		pclose(f);
	return n;
}

static char list[64][700];
static int nlist;

/* ---- tests ---- */

static void parallel_walk_8_threads_same_manifest(void)
{
	int i, j, done = 0;

	for (i = 0; i < nlist; i++) {
		struct walker w[NTHREADS];
		uint64_t fp, ent;
		struct mnt m;

		if (baseline(list[i], &fp, &ent)) {
			fprintf(stderr, "note: %s not walked\n", list[i]);
			continue;
		}
		CHECK(ent > 0);
		CHECK(mnt_open(&m, list[i], NULL) == 0);
		run_walkers(m.x, NTHREADS, w);
		for (j = 0; j < NTHREADS; j++) {
			CHECK_EQ(w[j].err, 0);
			CHECK_EQ((long long)w[j].entries, (long long)ent);
			CHECK(w[j].fp == fp);
		}
		check_quiesced(&m);
		mnt_close(&m);
		done++;
	}
	CHECK(done >= 10);
}

static void tiny_caches_still_correct(void)
{
	int i, j, done = 0;

	for (i = 0; i < nlist; i++) {
		struct walker w[NTHREADS];
		uint64_t fp, ent;
		struct mnt m;

		if (baseline(list[i], &fp, &ent))
			continue;
		CHECK(mnt_open(&m, list[i], TINY_OPTS) == 0);
		CHECK_EQ((long long)wfs_xfs_bcache_limit(m.x->bcache), 8 << 20);
		run_walkers(m.x, NTHREADS, w);
		for (j = 0; j < NTHREADS; j++) {
			CHECK_EQ(w[j].err, 0);
			CHECK_EQ((long long)w[j].entries, (long long)ent);
			CHECK(w[j].fp == fp);
		}
		check_quiesced(&m);
		/* the limits hold again once nothing is referenced (soft while busy) */
		CHECK(wfs_xfs_bcache_usage(m.x->bcache) <= wfs_xfs_bcache_limit(m.x->bcache));
		CHECK(wfs_xfs_icache_count(m.x) <= 2 * WFS_IC_SHARDS + 1 + WFS_XFS_INODECACHE_MIN);
		mnt_close(&m);
		done++;
	}
	CHECK(done >= 10);
}

/* ---- lookup/forget balance ---- */

struct lf {
	struct wfs_xfs *x;
	struct de *v;
	size_t n;
	unsigned seed;
	uint64_t looked, forgot;
	int err;
};

static unsigned xr(unsigned *s) { *s = *s * 1664525u + 1013904223u; return *s >> 8; }

static void *lf_thread(void *arg)
{
	struct lf *t = arg;
	enum { HELD = 40, ROUNDS = 3000 };
	wfs_ino_t held[HELD];
	size_t nheld = 0;
	int r;

	for (r = 0; r < ROUNDS && !t->err; r++) {
		/* lookup 1..3 random entries, keeping some, then forget a random batch */
		unsigned k = 1 + xr(&t->seed) % 3;

		while (k--) {
			struct de *d = &t->v[xr(&t->seed) % t->n];
			wfs_stat st;
			wfs_ino_t w;
			int err = wfs_xfs_op_lookup(t->x, 1, d->name, d->len, &w, &st);

			if (err) { t->err = err; break; }
			t->looked++;
			if (xr(&t->seed) % 2 && nheld < HELD)
				held[nheld++] = w;
			else {
				/* a second reference to the same inode, forgotten in one go */
				if (xr(&t->seed) % 4 == 0) {
					err = wfs_xfs_op_lookup(t->x, 1, d->name, d->len, &w, &st);
					if (err) { t->err = err; break; }
					t->looked++;
					wfs_xfs_op_forget(t->x, w, 2);
					t->forgot += 2;
				} else {
					wfs_xfs_op_forget(t->x, w, 1);
					t->forgot++;
				}
			}
		}
		while (nheld && (nheld == HELD || xr(&t->seed) % 3 == 0)) {
			wfs_xfs_op_forget(t->x, held[--nheld], 1);
			t->forgot++;
		}
	}
	while (nheld) {
		wfs_xfs_op_forget(t->x, held[--nheld], 1);
		t->forgot++;
	}
	return NULL;
}

static void concurrent_lookup_forget_balance(void)
{
	const char *opts[] = { NULL, TINY_OPTS };
	int o, i, mounted = 0;

	for (o = 0; o < 2; o++) {
		for (i = 0; i < nlist; i++) {
			struct mnt m;
			struct dlist l = { 0 };
			struct lf t[NTHREADS];
			pthread_t th[NTHREADS];
			uint64_t looked = 0, forgot = 0;
			int j;

			if (mnt_open(&m, list[i], opts[o]))
				continue;
			/* the root directory has the busiest names; use all of them */
			if (list_dir(m.x, 1, &l) || l.n == 0) {
				free(l.v);
				mnt_close(&m);
				continue;
			}
			for (j = 0; j < NTHREADS; j++) {
				t[j] = (struct lf){ .x = m.x, .v = l.v, .n = l.n, .seed = 977u * (j + 1) + i };
				CHECK(pthread_create(&th[j], NULL, lf_thread, &t[j]) == 0);
			}
			for (j = 0; j < NTHREADS; j++) {
				pthread_join(th[j], NULL);
				CHECK_EQ(t[j].err, 0);
				looked += t[j].looked; forgot += t[j].forgot;
			}
			CHECK_EQ((long long)looked, (long long)forgot);
			check_quiesced(&m);
			/* forgetting an inode nobody holds is ignored, not underflowed */
			wfs_xfs_op_forget(m.x, l.v[0].ino, 5);
			check_quiesced(&m);
			free(l.v);
			mnt_close(&m);
			mounted++;
		}
	}
	CHECK(mounted >= 20);
}

int main(int argc, char **argv)
{
	const char *fixdir = argc > 1 ? argv[1] : "../../target/xfs-fixtures";
	char probe[800];

	snprintf(probe, sizeof(probe), "%s/.done", fixdir);
	if (!exists(probe)) {
		printf("SKIPPED test_concurrent: no fixture cache at %s (run make test-mount)\n", fixdir);
		return 0;
	}
	nlist = names(fixdir, list, 64);
	if (nlist == 0) {
		printf("SKIPPED test_concurrent: no manifests in %s\n", fixdir);
		return 0;
	}
	mem_base = wfs_mem_used();
#define RUN(t) do { int f0 = failures; t(); \
	printf("%s %s\n", failures == f0 ? "PASS" : "FAIL", #t); fflush(stdout); } while (0)
	RUN(parallel_walk_8_threads_same_manifest);
	RUN(tiny_caches_still_correct);
	RUN(concurrent_lookup_forget_balance);
	CHECK(nwrites == 0);
	printf("test_concurrent: %d checks, %d failures\n", checks, failures);
	return failures ? 1 : 0;
}
