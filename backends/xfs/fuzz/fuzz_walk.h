/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Shared core of the fuzz harness (design 10.6): a sector image served to the
 * backend through a fake host API, and the "mount -> walk everything ->
 * unmount" run. Included (once per program) by fuzz_mount_walk.c (libFuzzer)
 * and mutate_runner.c (deterministic mutation runner).
 *
 * Image files
 *   .wsi  base image: sparse list of the 512-byte sectors the unmutated walk
 *         reads (everything else reads as zeros).  "WFSIMG01"
 *   .wsd  delta: name of a base .wsi + replacement sectors.  "WFSDLT01"
 *   Both are little-endian; written by `mutate_runner --pack` and
 *   tools/gen-fuzz-corpus.sh.
 *
 * Pass criteria of one run (struct fz_res): no crash/sanitizer report (the
 * process dies), no timeout, no allocation refused by the cap, no leak, no
 * ABI contract violation, every returned code 0 or a documented errno.
 */
#ifndef WFS_FUZZ_WALK_H
#define WFS_FUZZ_WALK_H

#include <dirent.h>	/* before the shim: its DT_* macros would clash with glibc's enum */
#include "wfs_xfs.h"
#include "icache.h"
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define FZ_SEC		512u
#define FZ_MAX_OVL	16
#define FZ_CAP_BYTES	((size_t)48 << 20)	/* allocation cap above the pre-mount total */
#define FZ_OPTIONS	"bufcache_bytes=2097152,inodecache_entries=64"

struct fz_sec { uint32_t role; uint64_t no; uint8_t d[FZ_SEC]; };

struct fz_img {
	uint64_t size[WFS_DEV_COUNT];
	struct fz_sec *sec;		/* sorted by (role, no) */
	size_t n;
	int raw_fd;			/* >= 0: read from this file instead (--pack) */
};

struct fz_ovl { uint32_t role; uint64_t no; uint8_t d[FZ_SEC]; };

struct fz_res {
	int mount_err;			/* 0 or -WFS_E* */
	unsigned nerr;			/* non-zero (documented) codes seen in the walk */
	unsigned nops;
	unsigned bad_code;		/* undocumented return code */
	unsigned contract;		/* ABI contract violations (names, cookies, ino 0) */
	unsigned leak;
	unsigned cap_hits;		/* allocations refused: over-allocation */
	int timeout;
	unsigned long nreads;
	char first[160];		/* first problem, for the report */
};

static inline int fz_fail(const struct fz_res *r)
{
	return r->bad_code || r->contract || r->leak || r->cap_hits || r->timeout;
}

/* ---- image I/O ---- */

static int fz_cmp_sec(const void *a, const void *b)
{
	const struct fz_sec *x = a, *y = b;

	if (x->role != y->role)
		return x->role < y->role ? -1 : 1;
	return x->no < y->no ? -1 : x->no > y->no;
}

static const struct fz_sec *fz_find(const struct fz_img *im, uint32_t role, uint64_t no)
{
	size_t lo = 0, hi = im->n;

	while (lo < hi) {
		size_t mid = lo + (hi - lo) / 2;
		const struct fz_sec *s = &im->sec[mid];

		if (s->role == role && s->no == no)
			return s;
		if (s->role < role || (s->role == role && s->no < no))
			lo = mid + 1;
		else
			hi = mid;
	}
	return NULL;
}

static int fz_load_wsi(struct fz_img *im, const char *path)
{
	FILE *f = fopen(path, "rb");
	char magic[8];
	uint32_t n, i;

	memset(im, 0, sizeof(*im));
	im->raw_fd = -1;
	if (!f)
		return -1;
	if (fread(magic, 8, 1, f) != 1 || memcmp(magic, "WFSIMG01", 8) ||
	    fread(im->size, sizeof(im->size), 1, f) != 1 || fread(&n, 4, 1, f) != 1) {
		fclose(f);
		return -1;
	}
	im->sec = calloc(n ? n : 1, sizeof(*im->sec));
	for (i = 0; im->sec && i < n; i++) {
		if (fread(&im->sec[i].role, 4, 1, f) != 1 || fread(&im->sec[i].no, 8, 1, f) != 1 ||
		    fread(im->sec[i].d, FZ_SEC, 1, f) != 1) {
			fclose(f);
			return -1;
		}
	}
	fclose(f);
	im->n = n;
	qsort(im->sec, n, sizeof(*im->sec), fz_cmp_sec);
	return im->sec ? 0 : -1;
}

static int fz_save_wsi(const struct fz_img *im, const char *path)
{
	FILE *f = fopen(path, "wb");
	uint32_t n = (uint32_t)im->n;
	size_t i;

	if (!f)
		return -1;
	fwrite("WFSIMG01", 8, 1, f);
	fwrite(im->size, sizeof(im->size), 1, f);
	fwrite(&n, 4, 1, f);
	for (i = 0; i < im->n; i++) {
		fwrite(&im->sec[i].role, 4, 1, f);
		fwrite(&im->sec[i].no, 8, 1, f);
		fwrite(im->sec[i].d, FZ_SEC, 1, f);
	}
	return fclose(f) ? -1 : 0;
}

/* Delta: loads its base (path relative to the delta's directory) into *im and
 * the replacement sectors into ovl[] (up to max); returns their count or -1. */
static int fz_load_wsd(struct fz_img *im, struct fz_ovl *ovl, int max, const char *path)
{
	FILE *f = fopen(path, "rb");
	char magic[8], base[96], full[1024];
	uint32_t n, i;
	const char *slash = strrchr(path, '/');

	if (!f)
		return -1;
	if (fread(magic, 8, 1, f) != 1 || memcmp(magic, "WFSDLT01", 8) ||
	    fread(base, sizeof(base), 1, f) != 1 || fread(&n, 4, 1, f) != 1 || n > (uint32_t)max) {
		fclose(f);
		return -1;
	}
	base[sizeof(base) - 1] = 0;
	snprintf(full, sizeof(full), "%.*s%s", slash ? (int)(slash - path + 1) : 0, path, base);
	for (i = 0; i < n; i++) {
		if (fread(&ovl[i].role, 4, 1, f) != 1 || fread(&ovl[i].no, 8, 1, f) != 1 ||
		    fread(ovl[i].d, FZ_SEC, 1, f) != 1) {
			fclose(f);
			return -1;
		}
	}
	fclose(f);
	return fz_load_wsi(im, full) ? -1 : (int)n;
}

static int fz_save_wsd(const char *path, const char *base, const struct fz_ovl *ovl, int n)
{
	FILE *f = fopen(path, "wb");
	char b[96] = { 0 };
	uint32_t cnt = (uint32_t)n;
	int i;

	if (!f)
		return -1;
	snprintf(b, sizeof(b), "%s", base);
	fwrite("WFSDLT01", 8, 1, f);
	fwrite(b, sizeof(b), 1, f);
	fwrite(&cnt, 4, 1, f);
	for (i = 0; i < n; i++) {
		fwrite(&ovl[i].role, 4, 1, f);
		fwrite(&ovl[i].no, 8, 1, f);
		fwrite(ovl[i].d, FZ_SEC, 1, f);
	}
	return fclose(f) ? -1 : 0;
}

/* ---- fake host ---- */

struct wfs_blkdev { uint32_t role; };

static struct {
	const struct fz_img *im;
	const struct fz_ovl *ovl;
	int novl;
	struct fz_sec *rec;		/* recording (--pack): grows, may hold duplicates */
	size_t nrec, caprec;
	unsigned long nreads;
	double deadline;		/* CLOCK_MONOTONIC seconds, 0 = none */
	int timed_out;
	int verbose;
} fz;

static double fz_now(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

static uint64_t h_size(wfs_blkdev *d) { return fz.im->size[d->role]; }
static uint32_t h_ss(wfs_blkdev *d) { (void)d; return FZ_SEC; }
static int h_ro(wfs_blkdev *d) { (void)d; return 1; }
static wfs_err_t h_write(wfs_blkdev *d, uint64_t o, const void *b, size_t l)
{ (void)d; (void)o; (void)b; (void)l; abort(); }	/* a read-only backend never writes */
static wfs_err_t h_flush(wfs_blkdev *d) { (void)d; abort(); }
static void h_log(int level, const char *msg)
{
	if (fz.verbose)
		fprintf(stderr, "[%d] %s\n", level, msg);
}

static void fz_record(uint32_t role, uint64_t no, const uint8_t *d)
{
	if (fz.nrec == fz.caprec) {
		fz.caprec = fz.caprec ? fz.caprec * 2 : 4096;
		fz.rec = realloc(fz.rec, fz.caprec * sizeof(*fz.rec));
		if (!fz.rec)
			abort();
	}
	fz.rec[fz.nrec].role = role;
	fz.rec[fz.nrec].no = no;
	memcpy(fz.rec[fz.nrec].d, d, FZ_SEC);
	fz.nrec++;
}

static wfs_err_t h_read(wfs_blkdev *d, uint64_t off, void *buf, size_t len)
{
	uint8_t *out = buf;
	uint64_t pos = off;

	if (off > fz.im->size[d->role] || len > fz.im->size[d->role] - off)
		return -WFS_EINVAL;
	fz.nreads++;
	if (fz.deadline && (fz.nreads & 63) == 0 && fz_now() > fz.deadline)
		fz.timed_out = 1;
	if (fz.timed_out)
		return -WFS_EIO;
	while (len) {
		uint64_t no = pos / FZ_SEC;
		size_t in = (size_t)(pos % FZ_SEC), n = FZ_SEC - in;
		uint8_t sec[FZ_SEC];
		const struct fz_sec *s;
		int i, hit = 0;

		if (n > len)
			n = len;
		if (fz.im->raw_fd >= 0) {
			memset(sec, 0, sizeof(sec));
			if (pread(fz.im->raw_fd, sec, FZ_SEC, (off_t)(no * FZ_SEC)) < 0)
				return -WFS_EIO;
			fz_record(d->role, no, sec);
		} else {
			for (i = 0; i < fz.novl && !hit; i++)
				if (fz.ovl[i].role == d->role && fz.ovl[i].no == no) {
					memcpy(sec, fz.ovl[i].d, FZ_SEC);
					hit = 1;
				}
			if (!hit) {
				s = fz_find(fz.im, d->role, no);
				if (s)
					memcpy(sec, s->d, FZ_SEC);
				else
					memset(sec, 0, FZ_SEC);
			}
		}
		memcpy(out, sec + in, n);
		out += n;
		pos += n;
		len -= n;
	}
	return 0;
}

static const wfs_host_api fz_api = {
	.abi_version = WFS_ABI_VERSION, .size = sizeof(wfs_host_api),
	.blk_size = h_size, .blk_sector_size = h_ss, .blk_read_only = h_ro,
	.blk_read = h_read, .blk_write = h_write, .blk_flush = h_flush, .log = h_log,
};

/* ---- the walk ---- */

static void fz_note(struct fz_res *r, const char *what, int err)
{
	if (!r->first[0])
		snprintf(r->first, sizeof(r->first), "%s: %d", what, err);
}

/* A walk operation's result: 0 or a documented errno, else bad_code. */
static int fz_code(struct fz_res *r, const char *op, int err)
{
	r->nops++;
	if (!err)
		return 0;
	switch (-err) {
	case WFS_EUCLEAN: case WFS_EIO: case WFS_EINVAL: case WFS_ENOENT: case WFS_ENOTDIR:
	case WFS_EISDIR: case WFS_ESTALE: case WFS_ENAMETOOLONG: case WFS_ENOMEM:
	case WFS_EOPNOTSUPP: case WFS_ENODEV: case WFS_ERANGE: case WFS_EFBIG:
		r->nerr++;
		return err;
	}
	r->bad_code++;
	fz_note(r, op, err);
	return err;
}

/* Probe whose failure is a legal answer on a clean image (garbage cookie,
 * missing name, over-long name, buffer too small): documented codes only, and
 * they do not count as "the image is bad". */
static int fz_tol(struct fz_res *r, const char *op, int err)
{
	unsigned n = r->nerr;
	int e = fz_code(r, op, err);

	r->nerr = n;
	return e;
}

static void fz_contract(struct fz_res *r, const char *what)
{
	r->contract++;
	fz_note(r, what, 0);
}

#define FZ_MAX_DIRENTS	200000u
#define FZ_MAX_DIRS	512u
#define FZ_MAX_INODES	4096u
#define FZ_READ_CAP	(256u << 10)	/* bytes read from the head of each file */
#define FZ_TOTAL_READ	((uint64_t)8 << 20)

struct fz_ent { wfs_ino_t ino; uint64_t next; uint32_t ftype, nlen; char name[WFS_NAME_MAX + 1]; };

struct fz_walk {
	struct fz_res *r;
	struct fz_ent *ents;
	size_t n, cap;
	int stop;
	uint64_t last_cookie, mode_cookie;
	uint64_t seen_cookies[64];
	size_t nseen;
	int check_cookies;
};

static int fz_dirent_cb(void *ctx, const wfs_dirent *e)
{
	struct fz_walk *w = ctx;
	size_t i;

	if (w->n >= FZ_MAX_DIRENTS) {
		w->r->timeout = 1;		/* runaway directory */
		fz_note(w->r, "readdir runaway", 0);
		w->stop = 1;
		return 1;
	}
	if (e->name_len < 1 || e->name_len > WFS_NAME_MAX || !e->name ||
	    memchr(e->name, 0, e->name_len) || memchr(e->name, '/', e->name_len))
		fz_contract(w->r, "readdir name");
	if (e->ino == 0)
		fz_contract(w->r, "readdir ino 0");
	if (e->next_cookie == 0 || e->next_cookie >= (1ull << 63) || e->next_cookie == w->last_cookie)
		fz_contract(w->r, "readdir cookie");
	else if (w->check_cookies) {
		/* uniqueness within the call: sample the first 64, plus adjacent */
		for (i = 0; i < w->nseen; i++)
			if (w->seen_cookies[i] == e->next_cookie)
				fz_contract(w->r, "readdir cookie reused");
		if (w->nseen < 64)
			w->seen_cookies[w->nseen++] = e->next_cookie;
	}
	w->last_cookie = e->next_cookie;
	if (w->n == w->cap) {
		w->cap = w->cap ? w->cap * 2 : 256;
		w->ents = realloc(w->ents, w->cap * sizeof(*w->ents));
		if (!w->ents)
			abort();
	}
	{
		struct fz_ent *d = &w->ents[w->n++];
		uint32_t l = e->name_len > WFS_NAME_MAX ? WFS_NAME_MAX : e->name_len;

		d->ino = e->ino;
		d->next = e->next_cookie;
		d->ftype = e->ftype;
		d->nlen = l;
		if (e->name)
			memcpy(d->name, e->name, l);
		d->name[l] = 0;
	}
	return 0;
}

static void fz_walk_file(struct wfs_xfs *x, struct fz_res *r, wfs_ino_t ino, const wfs_stat *st,
			 uint8_t *buf, uint64_t *total)
{
	static const uint64_t probe[] = { 0, 1, 511, 4095, 65537 };
	uint64_t off = 0;
	size_t i;

	/* head of the file in 64 KiB steps, then a few windows incl. EOF side */
	while (off < st->size && off < FZ_READ_CAP && *total < FZ_TOTAL_READ && !fz.timed_out) {
		size_t done = 0;
		int e = fz_code(r, "read", wfs_xfs_op_read(x, ino, off, buf, 65536, &done));

		if (e || done == 0)
			break;
		if (done > 65536)
			fz_contract(r, "read done > len");
		off += done;
		*total += done;
	}
	for (i = 0; i < sizeof(probe) / sizeof(probe[0]) && !fz.timed_out; i++) {
		size_t done = 0;

		fz_code(r, "read-probe", wfs_xfs_op_read(x, ino, probe[i], buf, 4096, &done));
		if (done > 4096)
			fz_contract(r, "read done > len");
	}
	if (st->size > 8192 && !fz.timed_out) {
		size_t done = 0;

		fz_code(r, "read-tail", wfs_xfs_op_read(x, ino, st->size - 4097, buf, 8192, &done));
		fz_code(r, "read-far", wfs_xfs_op_read(x, ino, st->size / 2 + 3, buf, 4096, &done));
	}
}

static void fz_walk_all(struct wfs_xfs *x, struct fz_res *r)
{
	wfs_ino_t *dirs = calloc(FZ_MAX_DIRS, sizeof(*dirs));
	wfs_ino_t *seen = calloc(FZ_MAX_INODES, sizeof(*seen));
	uint8_t *buf = malloc(65536);
	char link[4200];
	struct fz_walk w = { .r = r, .check_cookies = 1 };
	size_t qh = 0, qt = 0, nseen = 0;
	uint64_t total = 0;
	wfs_statfs sf;
	wfs_stat st;

	if (!dirs || !seen || !buf)
		abort();
	fz_code(r, "statfs", wfs_xfs_op_statfs(x, &sf));
	dirs[qt++] = WFS_ROOT_INO;
	seen[nseen++] = WFS_ROOT_INO;
	while (qh < qt && !fz.timed_out) {
		wfs_ino_t dir = dirs[qh++];
		size_t i;
		int e;

		fz_code(r, "getattr-dir", wfs_xfs_op_getattr(x, dir, &st));
		w.n = 0;
		w.stop = 0;
		w.last_cookie = 0;
		w.nseen = 0;
		e = fz_code(r, "readdir", wfs_xfs_op_readdir(x, dir, 0, fz_dirent_cb, &w));
		/* resume from the middle and from a cookie nobody handed out */
		if (w.n > 2 && !w.stop && !e) {
			struct fz_ent mid = w.ents[w.n / 2];
			struct fz_walk w2 = { .r = r };

			fz_code(r, "readdir-resume", wfs_xfs_op_readdir(x, dir, mid.next, fz_dirent_cb, &w2));
			free(w2.ents);
		}
		{
			struct fz_walk w3 = { .r = r };

			fz_tol(r, "readdir-garbage", wfs_xfs_op_readdir(x, dir, 0x123456789abcull, fz_dirent_cb, &w3));
			fz_tol(r, "readdir-garbage2", wfs_xfs_op_readdir(x, dir, (1ull << 63) - 1, fz_dirent_cb, &w3));
			free(w3.ents);
		}
		for (i = 0; i < w.n && !fz.timed_out; i++) {
			struct fz_ent *en = &w.ents[i];
			wfs_ino_t out = 0;
			wfs_stat lst, gst;
			int le, ge;

			le = fz_code(r, "lookup", wfs_xfs_op_lookup(x, dir, en->name, en->nlen, &out, &lst));
			if (!le) {
				if (out == 0)
					fz_contract(r, "lookup ino 0");
				wfs_xfs_op_forget(x, out, 1);
			}
			ge = fz_code(r, "getattr", wfs_xfs_op_getattr(x, en->ino, &gst));
			if (ge)
				continue;
			if (gst.ftype == WFS_FT_DIR) {
				size_t j;
				int dup = 0;

				for (j = 0; j < nseen && !dup; j++)
					dup = seen[j] == en->ino;
				if (!dup && qt < FZ_MAX_DIRS && nseen < FZ_MAX_INODES) {
					seen[nseen++] = en->ino;
					dirs[qt++] = en->ino;
				}
				/* a directory is not readable / readable as a link */
				fz_tol(r, "read-dir", wfs_xfs_op_read(x, en->ino, 0, buf, 16, &(size_t){ 0 }));
			} else if (gst.ftype == WFS_FT_REG) {
				fz_walk_file(x, r, en->ino, &gst, buf, &total);
			} else if (gst.ftype == WFS_FT_LNK) {
				size_t ln = 0;

				if (!fz_code(r, "readlink", wfs_xfs_op_readlink(x, en->ino, link, sizeof(link), &ln)) &&
				    ln > sizeof(link))
					fz_contract(r, "readlink len");
				fz_tol(r, "readlink-small", wfs_xfs_op_readlink(x, en->ino, link, 3, &ln));
			}
		}
		if (dir == WFS_ROOT_INO) {
			wfs_ino_t out;
			char longname[WFS_NAME_MAX + 1];

			memset(longname, 'n', sizeof(longname));
			fz_tol(r, "lookup-missing", wfs_xfs_op_lookup(x, dir, "no-such-name", 12, &out, &st));
			fz_tol(r, "lookup-255", wfs_xfs_op_lookup(x, dir, longname, WFS_NAME_MAX, &out, &st));
			fz_tol(r, "lookup-256", wfs_xfs_op_lookup(x, dir, longname, WFS_NAME_MAX + 1, &out, &st));
		}
	}
	free(w.ents);
	free(dirs);
	free(seen);
	free(buf);
}

/*
 * One run: mount -> walk -> unmount over `im` with `ovl` (n replacement
 * sectors) laid over it. alloc_cap_bytes: total live allocation above the
 * pre-mount level (0 = FZ_CAP_BYTES). limit_s: wall-clock limit.
 */
static void fz_run(const struct fz_img *im, const struct fz_ovl *ovl, int novl, double limit_s,
		   struct fz_res *r)
{
	wfs_blkdev dev = { .role = WFS_DEV_DATA }, logdev = { .role = WFS_DEV_LOG },
		   rtdev = { .role = WFS_DEV_RT };
	wfs_blkdev *devs[WFS_DEV_COUNT] = { &dev, im->size[1] ? &logdev : NULL,
					    im->size[2] ? &rtdev : NULL };
	size_t base = wfs_mem_used();
	size_t hits = wfs_mem_cap_hits();
	struct wfs_xfs *x = NULL;

	memset(r, 0, sizeof(*r));
	fz.im = im;
	fz.ovl = ovl;
	fz.novl = novl;
	fz.nreads = 0;
	fz.timed_out = 0;
	fz.deadline = limit_s > 0 ? fz_now() + limit_s : 0;
	wfs_mem_set_cap(base + FZ_CAP_BYTES);

	r->mount_err = fz_code(r, "mount", wfs_xfs_mount(&fz_api, devs, FZ_OPTIONS, &x));
	r->nerr = 0;	/* a failed mount is the expected result of a bad image, count below */
	if (r->mount_err)
		r->nerr = 1;
	if (!r->mount_err) {
		fz_walk_all(x, r);
		if (wfs_xfs_icache_refs(x) != 0) {
			r->leak++;
			fz_note(r, "icache refs after walk", (int)wfs_xfs_icache_refs(x));
		}
		fz_code(r, "unmount", wfs_xfs_unmount(x));
	}
	r->nreads = fz.nreads;
	r->timeout |= fz.timed_out;
	if (fz.timed_out)
		fz_note(r, "timeout", 0);
	r->cap_hits = (unsigned)(wfs_mem_cap_hits() - hits);
	if (r->cap_hits)
		fz_note(r, "allocation cap hit (over-allocation)", (int)r->cap_hits);
	wfs_mem_set_cap(0);
	if (wfs_mem_used() != base) {
		r->leak++;
		fz_note(r, "memory not released", (int)(wfs_mem_used() - base));
	}
}

#endif
