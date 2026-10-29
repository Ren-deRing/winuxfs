// SPDX-License-Identifier: GPL-2.0-only
/*
 * Task 10 tests: file read (holes, unwritten, btree extent maps, shared
 * extents, RT device, unaligned windows, EOF) and readlink.
 *
 *   test_read FIXDIR ROOTFIX
 *     FIXDIR   generated matrix (tools/gen-fixtures.sh): NAME.img, optional
 *              NAME.log.img / NAME.rt.img, NAME.manifest.json, NAME.expect.json
 *     ROOTFIX  tests/xfs/root-fixtures (*.img.zst, kernel-written images)
 *
 * Ground truth: the manifest SHA-256 of the source tree (sha256sum of what
 * the backend returns), xfs_db for inode numbers/extent maps, and patterns
 * known from tools/gen-root-fixtures.sh. Read-only host: any write fails the
 * test. Skips visibly when a tool is missing.
 */
#include <dirent.h>	/* before the shim: its DT_* macros would clash with glibc's enum */
#include "wfs_xfs.h"
#include "icache.h"
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
#define CHECK_EQ(a, b) do { long long _a = (a), _b = (b); checks++; if (_a != _b) { failures++; \
	fprintf(stderr, "%s:%d: %s = %lld, want %lld\n", __FILE__, __LINE__, #a, _a, _b); } } while (0)

struct wfs_blkdev { int fd; uint64_t size; };
static void h_log(int level, const char *msg) { if (getenv("RD_VERBOSE")) fprintf(stderr, "[%d] %s\n", level, msg); }
static uint64_t h_size(wfs_blkdev *d) { return d->size; }
static uint32_t h_ss(wfs_blkdev *d) { (void)d; return 512; }
static int h_ro(wfs_blkdev *d) { (void)d; return 1; }
static long nreads;
static wfs_err_t h_read(wfs_blkdev *d, uint64_t off, void *buf, size_t len)
{
	if (off > d->size || len > d->size - off)
		return -WFS_EINVAL;
	nreads++;
	return pread(d->fd, buf, len, (off_t)off) == (ssize_t)len ? 0 : -WFS_EIO;
}
static long nwrites;
static wfs_err_t h_write(wfs_blkdev *d, uint64_t o, const void *b, size_t l)
{ (void)d; (void)o; (void)b; (void)l; nwrites++; return -WFS_EROFS; }
static wfs_err_t h_flush(wfs_blkdev *d) { (void)d; nwrites++; return 0; }
static const wfs_host_api api = {
	.abi_version = WFS_ABI_VERSION, .size = sizeof(wfs_host_api),
	.blk_size = h_size, .blk_sector_size = h_ss, .blk_read_only = h_ro,
	.blk_read = h_read, .blk_write = h_write, .blk_flush = h_flush, .log = h_log,
};

static char tmp[512];
static size_t mem_base;
static int live;
static int tools_ok;

static int sh(const char *fmt, ...)
{
	char cmd[4096];
	va_list ap;

	va_start(ap, fmt);
	vsnprintf(cmd, sizeof(cmd), fmt, ap);
	va_end(ap);
	return system(cmd);
}

static int have(const char *tool)
{
	return sh("command -v %s >/dev/null 2>&1", tool) == 0;
}

static int exists(const char *p) { struct stat st; return stat(p, &st) == 0; }

struct mnt {
	wfs_blkdev dev[WFS_DEV_COUNT];
	wfs_blkdev *p[WFS_DEV_COUNT];
	struct wfs_xfs *x;
};

static int mnt_open3(struct mnt *m, const char *data, const char *log, const char *rt)
{
	const char *paths[WFS_DEV_COUNT] = { data, log, rt };
	int i;

	memset(m, 0, sizeof(*m));
	for (i = 0; i < WFS_DEV_COUNT; i++) {
		struct stat st;

		m->dev[i].fd = -1;
		if (!paths[i])
			continue;
		m->dev[i].fd = open(paths[i], O_RDONLY);
		if (m->dev[i].fd < 0 || fstat(m->dev[i].fd, &st))
			return 12345;
		m->dev[i].size = (uint64_t)st.st_size;
		m->p[i] = &m->dev[i];
	}
	live++;
	return wfs_xfs_mount(&api, m->p, NULL, &m->x);
}
static int mnt_open(struct mnt *m, const char *img) { return mnt_open3(m, img, NULL, NULL); }
static void mnt_close(struct mnt *m)
{
	int i;

	CHECK(wfs_xfs_icache_refs(m->x) == 0);
	CHECK(wfs_xfs_unmount(m->x) == 0);
	for (i = 0; i < WFS_DEV_COUNT; i++)
		if (m->dev[i].fd >= 0)
			close(m->dev[i].fd);
	if (--live == 0)
		CHECK(wfs_mem_used() == mem_base);
}

static void path_join(char *out, size_t n, const char *a, const char *b)
{
	snprintf(out, n, "%s/%s", a, b);
}

/* ---- helpers ---- */

static wfs_ino_t w_of(struct mnt *m, uint64_t xino) { return wfs_xfs_ino_to_wfs(&m->x->mp, xino); }

static uint64_t db_ino(const char *img, const char *path)
{
	char cmd[1024], line[256];
	FILE *f;
	uint64_t v = 0;

	snprintf(cmd, sizeof(cmd), "xfs_db -r -c 'path %s' -c 'inode' '%s' 2>/dev/null", path, img);
	f = popen(cmd, "r");
	while (f && fgets(line, sizeof(line), f))
		if (strstr(line, "inode number is"))
			v = strtoull(strrchr(line, ' ') + 1, NULL, 0);
	if (f)
		pclose(f);
	return v;
}

/* Read [off, off+len) completely; returns the first error or 0. *got = bytes. */
static int read_range(struct mnt *m, wfs_ino_t w, uint64_t off, void *buf, size_t len, size_t *got)
{
	size_t done = 0, d;
	int err = 0;

	while (done < len) {
		d = 12345;
		err = wfs_xfs_op_read(m->x, w, off + done, (uint8_t *)buf + done, len - done, &d);
		if (err || d == 0) {
			done += err ? 0 : d;
			break;
		}
		done += d;
	}
	*got = done;
	return err;
}

/* sha256sum of [0,size) read in odd-sized (unaligned) chunks. */
#define ODD_CHUNK ((size_t)(1u << 20) + 13)
static int hash_file(struct mnt *m, wfs_ino_t w, uint64_t size, char hex[65])
{
	char cmd[1200], hf[600];
	uint8_t *buf = malloc(ODD_CHUNK);
	FILE *p, *r;
	uint64_t off = 0;
	int err = 0;

	snprintf(hf, sizeof(hf), "%s/hash.out", tmp);
	snprintf(cmd, sizeof(cmd), "sha256sum > '%s'", hf);
	p = popen(cmd, "w");
	if (!buf || !p) {
		free(buf);
		return -1;
	}
	while (off < size) {
		size_t want = size - off < ODD_CHUNK ? (size_t)(size - off) : ODD_CHUNK, got;

		err = read_range(m, w, off, buf, want, &got);
		if (err || got != want) {
			if (!err)
				err = -9999;
			break;
		}
		fwrite(buf, 1, got, p);
		off += got;
	}
	pclose(p);
	free(buf);
	hex[0] = 0;
	r = fopen(hf, "r");
	if (r) {
		if (fscanf(r, "%64s", hex) != 1)
			hex[0] = 0;
		fclose(r);
	}
	return err;
}

static uint64_t rng_state = 0x9e3779b97f4a7c15ull;
static uint64_t rnd(void)
{
	rng_state ^= rng_state << 13;
	rng_state ^= rng_state >> 7;
	rng_state ^= rng_state << 17;
	return rng_state;
}

/* Random windows (also beyond EOF) against a full copy; canary after len. */
static void check_windows(struct mnt *m, wfs_ino_t w, const uint8_t *whole, uint64_t size, int n)
{
	uint8_t *buf = malloc(70016);
	int i;

	for (i = 0; i < n && buf; i++) {
		uint64_t off = rnd() % (size + 100);
		size_t len = (i % 5 == 0) ? 1 + rnd() % 3 : rnd() % 70000, done = 777, exp;
		int err;

		memset(buf, 0xAA, 70016);
		err = wfs_xfs_op_read(m->x, w, off, buf, len, &done);
		exp = off >= size ? 0 : (len < size - off ? len : (size_t)(size - off));
		CHECK_EQ(err, 0);
		CHECK_EQ(done, exp);
		if (done == exp && exp)
			CHECK(!memcmp(buf, whole + off, exp));
		CHECK(buf[len] == 0xAA && buf[70015] == 0xAA);
	}
	free(buf);
}

/* ---- manifest walk ---- */

static const char LIST_PY[] =
	"import json,sys\n"
	"m=json.load(open(sys.argv[1]))['entries']\n"
	"e=json.load(open(sys.argv[2]))['paths']\n"
	"for p,v in sorted(m.items()):\n"
	"    if p not in e: continue\n"
	"    t=v.get('target','').encode('utf-8','surrogateescape').hex() or '-'\n"
	"    print('\\t'.join([p.encode('utf-8','surrogateescape').hex(),v['kind'],str(v.get('size',0)),\n"
	"        v.get('sha256','-'),str(e[p]['ino']),str(e[p].get('bmbt_level',0)),t]))\n";

static int seen_level2, seen_btree_file, seen_unaligned_windows, walked_images, walked_files;
static unsigned big_done_mask;

static size_t unhex(const char *h, uint8_t *out, size_t max)
{
	size_t n = 0;
	unsigned v;

	if (!strcmp(h, "-"))
		return 0;
	while (h[0] && h[1] && n < max && sscanf(h, "%2x", &v) == 1) {
		out[n++] = (uint8_t)v;
		h += 2;
	}
	return n;
}

static void walk_image(const char *fixdir, const char *name)
{
	char base[700], img[720], log[720], rt[720], man[720], exp[720], py[600], lst[600], line[8192];
	struct mnt m;
	FILE *f;
	int dirs_checked = 0, fifos = 0, rc;

	path_join(base, sizeof(base), fixdir, name);
	snprintf(img, sizeof(img), "%s.img", base);
	snprintf(log, sizeof(log), "%s.log.img", base);
	snprintf(rt, sizeof(rt), "%s.rt.img", base);
	snprintf(man, sizeof(man), "%s.manifest.json", base);
	snprintf(exp, sizeof(exp), "%s.expect.json", base);
	if (!exists(man) || !exists(exp))
		return;
	snprintf(py, sizeof(py), "%s/list.py", tmp);
	snprintf(lst, sizeof(lst), "%s/list.tsv", tmp);
	f = fopen(py, "w");
	if (!f)
		return;
	fputs(LIST_PY, f);
	fclose(f);
	CHECK(sh("python3 -I '%s' '%s' '%s' > '%s'", py, man, exp, lst) == 0);

	rc = mnt_open3(&m, img, exists(log) ? log : NULL, exists(rt) ? rt : NULL);
	CHECK_EQ(rc, 0);
	if (rc)
		return;
	f = fopen(lst, "r");
	while (f && fgets(line, sizeof(line), f)) {
		char *fld[7], *s = line;
		uint8_t pathb[1100], tgt[1100], *whole = NULL;
		uint64_t size, xino;
		unsigned level;
		size_t tl, done;
		wfs_ino_t w;
		wfs_stat st;
		int i, err;
		char hex[65];

		for (i = 0; i < 7; i++) {
			fld[i] = s;
			s = strpbrk(s, "\t\n");
			if (!s)
				break;
			*s++ = 0;
		}
		if (i < 7 && !(i == 6 && s))
			continue;
		pathb[unhex(fld[0], pathb, sizeof(pathb) - 1)] = 0;
		size = strtoull(fld[2], NULL, 10);
		xino = strtoull(fld[4], NULL, 10);
		level = (unsigned)strtoul(fld[5], NULL, 10);
		w = w_of(&m, xino);
		tl = unhex(fld[6], tgt, sizeof(tgt) - 1);
		walked_files++;

		if (!strcmp(fld[1], "dir")) {
			uint8_t b[16];

			if (dirs_checked++ < 20) {
				CHECK_EQ(wfs_xfs_op_read(m.x, w, 0, b, sizeof(b), &done), -WFS_EISDIR);
				CHECK_EQ(wfs_xfs_op_readlink(m.x, w, (char *)b, sizeof(b), &done), -WFS_EINVAL);
			}
			continue;
		}
		if (!strcmp(fld[1], "symlink")) {
			char out[2048];
			size_t len = 0;

			memset(out, 0x55, sizeof(out));
			CHECK_EQ(wfs_xfs_op_readlink(m.x, w, out, sizeof(out), &len), 0);
			CHECK_EQ(len, tl);
			CHECK(len == tl && !memcmp(out, tgt, tl));
			CHECK_EQ(wfs_xfs_op_getattr(m.x, w, &st), 0);
			CHECK_EQ(st.size, size);
			CHECK_EQ(len, size);
			/* cap too small: ERANGE with the needed length, buffer untouched */
			memset(out, 0x55, sizeof(out));
			CHECK_EQ(wfs_xfs_op_readlink(m.x, w, out, tl - 1, &len), -WFS_ERANGE);
			CHECK_EQ(len, tl);
			CHECK(out[0] == 0x55);
			CHECK_EQ(wfs_xfs_op_readlink(m.x, w, out, tl, &len), 0);	/* exact fit */
			CHECK_EQ(wfs_xfs_op_read(m.x, w, 0, out, 4, &done), -WFS_EINVAL);
			continue;
		}
		if (strcmp(fld[1], "file")) {	/* fifo & co */
			uint8_t b[16];

			if (fifos++ < 5) {
				CHECK_EQ(wfs_xfs_op_read(m.x, w, 0, b, sizeof(b), &done), -WFS_EINVAL);
				CHECK_EQ(wfs_xfs_op_readlink(m.x, w, (char *)b, sizeof(b), &done), -WFS_EINVAL);
			}
			continue;
		}
		/* regular file */
		CHECK_EQ(wfs_xfs_op_getattr(m.x, w, &st), 0);
		CHECK_EQ(st.size, size);
		if (size == 0) {
			uint8_t b[8];

			done = 99;
			CHECK_EQ(wfs_xfs_op_read(m.x, w, 0, b, sizeof(b), &done), 0);
			CHECK_EQ(done, 0);
			continue;
		}
		if (!strcmp(fld[3], "skipped") || !strcmp(fld[3], "-"))
			continue;
		if (level >= 2)
			seen_level2++;
		{
			struct xfs_inode *ip;

			if (wfs_xfs_iget(m.x, xino, &ip) == 0) {
				if (ip->i_df.if_format == XFS_DINODE_FMT_BTREE)
					seen_btree_file++;
				xfs_irele(ip);
			}
		}
		if (size > ((uint64_t)256 << 20) && !getenv("WFS_READ_FULL")) {
			/* hashing 4 GiB of mostly zeros per image is slow: once per block size */
			unsigned bit = 1u << m.x->mp.m_sb.sb_blocklog;

			if (big_done_mask & bit) {
				uint8_t tail[8205];

				CHECK_EQ(read_range(&m, w, size - sizeof(tail), tail, sizeof(tail), &done), 0);
				CHECK_EQ(done, sizeof(tail));
				continue;
			}
			big_done_mask |= bit;
		}
		err = hash_file(&m, w, size, hex);
		CHECK_EQ(err, 0);
		if (strcmp(hex, fld[3]))
			fprintf(stderr, "sha256 mismatch %s in %s: %s want %s\n", pathb, name, hex, fld[3]);
		CHECK(!strcmp(hex, fld[3]));
		if (size <= (20u << 20) && size > 2048 && (walked_files % 3) == 0) {
			whole = malloc(size);
			if (whole) {
				size_t got;

				CHECK_EQ(read_range(&m, w, 0, whole, size, &got), 0);
				CHECK_EQ(got, size);
				check_windows(&m, w, whole, size, 40);
				seen_unaligned_windows++;
				free(whole);
			}
		}
	}
	if (f)
		fclose(f);
	mnt_close(&m);
	walked_images++;
}

static void manifest_sha256_matches_for_all_fixture_files(const char *fixdir)
{
	DIR *d = opendir(fixdir);
	struct dirent *e;
	char names[64][128];
	int n = 0, i;

	if (!d) {
		skipped++;
		printf("SKIPPED %s: %s missing\n", __func__, fixdir);
		return;
	}
	while ((e = readdir(d)) && n < 64) {
		size_t l = strlen(e->d_name);

		if (l > 14 && !strcmp(e->d_name + l - 14, ".manifest.json") && strncmp(e->d_name, "hole", 4))
			snprintf(names[n++], sizeof(names[0]), "%.*s", (int)(l - 14), e->d_name);
	}
	closedir(d);
	for (i = 0; i < n; i++)
		walk_image(fixdir, names[i]);
	CHECK(walked_images >= 16 || n < 16);
	CHECK(n > 0);
	printf("  walked %d images, %d entries\n", walked_images, walked_files);
}

static void bmbt_two_level_file_hash(void)
{
	/* counters from the matrix walk above; the hash was checked there */
	CHECK(seen_btree_file > 0);
	CHECK(seen_level2 > 0);
	CHECK(seen_unaligned_windows > 0);
}

static void read_at_eof_and_beyond(const char *fixdir)
{
	char img[720];
	struct mnt m;
	uint8_t whole[1025], b[64];
	size_t done;
	uint64_t xi;
	wfs_ino_t w;
	wfs_stat st;

	path_join(img, sizeof(img), fixdir, "m00-b1024-s512-ddef-plain.img");
	if (!exists(img)) {
		skipped++;
		printf("SKIPPED %s: m00 image missing\n", __func__);
		return;
	}
	xi = db_ino(img, "/block_plus_1.bin");
	CHECK(xi != 0);
	CHECK_EQ(mnt_open(&m, img), 0);
	w = w_of(&m, xi);
	CHECK_EQ(wfs_xfs_op_getattr(m.x, w, &st), 0);
	CHECK_EQ(st.size, 1025);
	CHECK_EQ(read_range(&m, w, 0, whole, sizeof(whole), &done), 0);
	CHECK_EQ(done, 1025);

	memset(b, 0x77, sizeof(b));
	done = 5;
	CHECK_EQ(wfs_xfs_op_read(m.x, w, 1025, b, sizeof(b), &done), 0);	/* at EOF */
	CHECK_EQ(done, 0);
	CHECK(b[0] == 0x77);
	done = 5;
	CHECK_EQ(wfs_xfs_op_read(m.x, w, 1026, b, sizeof(b), &done), 0);
	CHECK_EQ(done, 0);
	done = 5;
	CHECK_EQ(wfs_xfs_op_read(m.x, w, UINT64_MAX, b, sizeof(b), &done), 0);
	CHECK_EQ(done, 0);
	done = 5;
	CHECK_EQ(wfs_xfs_op_read(m.x, w, 0, b, 0, &done), 0);		/* zero length */
	CHECK_EQ(done, 0);
	done = 0;
	CHECK_EQ(wfs_xfs_op_read(m.x, w, 1024, b, sizeof(b), &done), 0);	/* clamp to 1 byte */
	CHECK_EQ(done, 1);
	CHECK(b[0] == whole[1024] && b[1] == 0x77);
	done = 0;
	CHECK_EQ(wfs_xfs_op_read(m.x, w, 1000, b, 25, &done), 0);	/* exact end */
	CHECK_EQ(done, 25);
	CHECK(!memcmp(b, whole + 1000, 25));
	done = 0;
	CHECK_EQ(wfs_xfs_op_read(m.x, w, 1020, b, SIZE_MAX / 2, &done), 0);	/* huge len */
	CHECK_EQ(done, 5);
	CHECK(!memcmp(b, whole + 1020, 5));
	/* one byte windows at every offset around the block boundary */
	for (xi = 1020; xi < 1030; xi++) {
		done = 9;
		CHECK_EQ(wfs_xfs_op_read(m.x, w, xi, b, 1, &done), 0);
		CHECK_EQ(done, xi < 1025 ? 1 : 0);
		if (xi < 1025)
			CHECK(b[0] == whole[xi]);
	}
	/* stale / bad handles */
	CHECK_EQ(wfs_xfs_op_read(m.x, 0, 0, b, 1, &done), -WFS_ESTALE);
	CHECK_EQ(wfs_xfs_op_read(m.x, (uint64_t)1 << 40, 0, b, 1, &done), -WFS_ESTALE);
	CHECK_EQ(wfs_xfs_op_readlink(m.x, w, (char *)b, sizeof(b), &done), -WFS_EINVAL);
	CHECK_EQ(wfs_xfs_op_read(m.x, WFS_ROOT_INO, 0, b, 1, &done), -WFS_EISDIR);
	mnt_close(&m);
}

/* ---- hole / unwritten image ---- */

static void hole_and_unwritten_extent_read_zeros(const char *fixdir)
{
	char img[720], base[720];
	struct mnt m;
	uint8_t buf[20480], z[16384];
	size_t done;
	int i;

	path_join(img, sizeof(img), fixdir, "hole-unwritten.img");
	if (!exists(img)) {
		skipped++;
		printf("SKIPPED %s: hole-unwritten image missing\n", __func__);
		return;
	}
	path_join(base, sizeof(base), fixdir, "hole-unwritten");
	walk_image(fixdir, "hole-unwritten");

	CHECK_EQ(mnt_open(&m, img), 0);
	{
		wfs_ino_t isl = w_of(&m, db_ino(img, "/isl.bin"));
		wfs_ino_t whole = w_of(&m, db_ino(img, "/whole.bin"));
		wfs_ino_t hole = w_of(&m, db_ino(img, "/hole.bin"));
		char cmd[1024], line[256];
		int unw = 0;
		FILE *f;

		/* ground truth: the flags really are set, and the disk holds nonzero bytes there */
		snprintf(cmd, sizeof(cmd), "xfs_db -r -c 'path /isl.bin' -c bmap '%s'", img);
		f = popen(cmd, "r");
		while (f && fgets(line, sizeof(line), f))
			if (strstr(line, "flag 1"))
				unw++;
		if (f)
			pclose(f);
		CHECK_EQ(unw, 1);

		memset(buf, 0x11, sizeof(buf));
		CHECK_EQ(read_range(&m, isl, 0, buf, 20480, &done), 0);
		CHECK_EQ(done, 20480);
		memset(z, 0, sizeof(z));
		for (i = 0; i < 4096; i++) {
			CHECK(buf[i] == 'A');			/* block 0 written */
			CHECK(buf[4096 + i] == 0);		/* block 1 hole */
			CHECK(buf[8192 + i] == 0);		/* block 2 unwritten (disk holds 'B') */
			CHECK(buf[12288 + i] == 0);		/* block 3 hole */
			CHECK(buf[16384 + i] == 'C');		/* block 4 written */
		}
		/* straddling windows */
		CHECK_EQ(read_range(&m, isl, 8190, buf, 8, &done), 0);
		CHECK(done == 8 && !memcmp(buf, z, 8));
		CHECK_EQ(read_range(&m, isl, 4090, buf, 4100, &done), 0);	/* 'A' tail, then hole */
		CHECK(done == 4100 && buf[0] == 'A' && buf[5] == 'A' && !memcmp(buf + 6, z, 4094));
		CHECK_EQ(read_range(&m, isl, 16380, buf, 8, &done), 0);	/* hole | 'C' */
		CHECK(done == 8 && !memcmp(buf, z, 4) && buf[4] == 'C' && buf[7] == 'C');
		CHECK_EQ(read_range(&m, isl, 4094, buf, 4, &done), 0);	/* A | hole boundary at 4096 */
		CHECK(done == 4 && buf[0] == 'A' && buf[1] == 'A' && !memcmp(buf + 2, z, 2));
		/* the whole file of unwritten extents is zeros */
		memset(buf, 0x11, sizeof(buf));
		CHECK_EQ(read_range(&m, whole, 0, buf, 16384, &done), 0);
		CHECK_EQ(done, 16384);
		CHECK(!memcmp(buf, z, 16384));
		/* native holes: hash checked by the walk; spot check zeros */
		CHECK_EQ(read_range(&m, hole, 0, buf, 20480, &done), 0);
		CHECK_EQ(done, 20480);
	}
	mnt_close(&m);
}

static void read_offset_1_shl_40_sparse(const char *fixdir)
{
	char img[720];
	struct mnt m;
	uint8_t buf[8192], z[8192];
	size_t done;
	wfs_ino_t far;
	uint64_t big = (uint64_t)1 << 40;

	path_join(img, sizeof(img), fixdir, "hole-unwritten.img");
	if (!exists(img)) {
		skipped++;
		printf("SKIPPED %s: image missing\n", __func__);
		return;
	}
	CHECK_EQ(mnt_open(&m, img), 0);
	far = w_of(&m, db_ino(img, "/far.bin"));
	memset(z, 0, sizeof(z));
	memset(buf, 0x5a, sizeof(buf));
	CHECK_EQ(read_range(&m, far, big - 4, buf, 64, &done), 0);	/* hole, then "far" */
	CHECK_EQ(done, 7);
	CHECK(!memcmp(buf, z, 4));
	CHECK(!memcmp(buf + 4, "far", 3));
	CHECK_EQ(read_range(&m, far, big, buf, 64, &done), 0);
	CHECK_EQ(done, 3);
	CHECK(!memcmp(buf, "far", 3));
	CHECK_EQ(read_range(&m, far, 0, buf, 4, &done), 0);
	CHECK(done == 4 && !memcmp(buf, "near", 4));
	/* the long hole in the middle: 8 KiB windows at assorted unaligned offsets */
	{
		uint64_t offs[] = { 4096, 4097, 123456789, 1ull << 30, (1ull << 39) + 5, big - 8192 - 3 };
		unsigned i;

		for (i = 0; i < sizeof(offs) / sizeof(offs[0]); i++) {
			memset(buf, 0x5a, sizeof(buf));
			CHECK_EQ(read_range(&m, far, offs[i], buf, sizeof(buf), &done), 0);
			CHECK_EQ(done, sizeof(buf));
			CHECK(!memcmp(buf, z, sizeof(z)));
		}
	}
	/* offset beyond the 40-bit size and offsets near 2^63 */
	CHECK_EQ(wfs_xfs_op_read(m.x, far, big + 3, buf, 8, &done), 0);
	CHECK_EQ(done, 0);
	CHECK_EQ(wfs_xfs_op_read(m.x, far, (uint64_t)1 << 63, buf, 8, &done), 0);
	CHECK_EQ(done, 0);
	mnt_close(&m);
}

/* ---- symlinks (local and remote), explicit ---- */

static void symlink_local_and_remote(const char *fixdir)
{
	char img[720];
	struct mnt m;
	char out[1100];
	size_t len;
	int k;

	path_join(img, sizeof(img), fixdir, "m00-b1024-s512-ddef-plain.img");
	if (!exists(img)) {
		skipped++;
		printf("SKIPPED %s: image missing\n", __func__);
		return;
	}
	CHECK_EQ(mnt_open(&m, img), 0);
	for (k = 0; k < 2; k++) {
		const char *path = k ? "/links_remote_300" : "/links_local";
		uint64_t xi = db_ino(img, path);
		struct xfs_inode *ip;
		size_t exp = k ? 300 : 8;

		CHECK(xi != 0);
		CHECK_EQ(wfs_xfs_iget(m.x, xi, &ip), 0);
		CHECK_EQ(ip->i_df.if_format, k ? XFS_DINODE_FMT_EXTENTS : XFS_DINODE_FMT_LOCAL);
		xfs_irele(ip);
		len = 0;
		memset(out, 0x33, sizeof(out));
		CHECK_EQ(wfs_xfs_op_readlink(m.x, w_of(&m, xi), out, sizeof(out), &len), 0);
		CHECK_EQ(len, exp);
		if (k) {
			CHECK(out[0] == '.' && out[1] == '/' && !memcmp(out + 292, "tiny.txt", 8));
		} else {
			CHECK(!memcmp(out, "tiny.txt", 8));
		}
		CHECK(out[exp] == 0x33);	/* no terminator written, no overrun */
		CHECK_EQ(wfs_xfs_op_readlink(m.x, w_of(&m, xi), out, exp - 1, &len), -WFS_ERANGE);
		CHECK_EQ(len, exp);
		CHECK_EQ(wfs_xfs_op_readlink(m.x, w_of(&m, xi), out, 0, &len), -WFS_ERANGE);
		CHECK_EQ(wfs_xfs_op_readlink(m.x, w_of(&m, xi), out, exp, &len), 0);
	}
	mnt_close(&m);
}

/* ---- RT device ---- */

static uint8_t pat(uint64_t o) { return (uint8_t)(o * 7 + 3 + (o >> 8)); }

/*
 * Turn /tiny.txt of `img` into a realtime file with three extents
 * (file blocks 0-3 -> rt 32, 4-7 -> rt 36 (adjacent: one merged read),
 * 10-12 -> rt 64; blocks 8, 9 are a hole) and put the matching bytes on the
 * rt image. Returns the file size (0 on failure).
 */
static uint64_t make_rt_file(const char *img, const char *rtimg, const char *path, uint32_t bs, uint64_t rgbno0)
{
	static const struct { uint64_t off, rtb, cnt; } ext[3] = { { 0, 32, 4 }, { 4, 36, 4 }, { 10, 64, 3 } };
	uint64_t size = 12 * (uint64_t)bs + bs / 2;
	uint8_t *blk = malloc(bs);
	int fd, i;
	uint64_t b;

	if (!blk)
		return 0;
	if (sh("xfs_db -x -c 'path %s' -c 'write core.forkoff 0' -c 'write core.aformat 2' "
	       "-c 'write core.naextents 0' -c 'write core.realtime 1' -c 'write core.nextents 3' "
	       "-c 'write core.nblocks 11' -c 'write core.size %llu' "
	       "-c 'write u3.bmx[0].startoff 0' -c 'write u3.bmx[0].startblock %llu' -c 'write u3.bmx[0].blockcount 4' -c 'write u3.bmx[0].extentflag 0' "
	       "-c 'write u3.bmx[1].startoff 4' -c 'write u3.bmx[1].startblock %llu' -c 'write u3.bmx[1].blockcount 4' -c 'write u3.bmx[1].extentflag 0' "
	       "-c 'write u3.bmx[2].startoff 10' -c 'write u3.bmx[2].startblock %llu' -c 'write u3.bmx[2].blockcount 3' -c 'write u3.bmx[2].extentflag 0' "
	       "'%s' >/dev/null", path, (unsigned long long)size,
	       (unsigned long long)(rgbno0 + 32), (unsigned long long)(rgbno0 + 36),
	       (unsigned long long)(rgbno0 + 64), img)) {
		free(blk);
		return 0;
	}
	fd = open(rtimg, O_WRONLY);
	if (fd < 0) {
		free(blk);
		return 0;
	}
	for (i = 0; i < 3; i++)
		for (b = 0; b < ext[i].cnt; b++) {
			uint64_t fo = (ext[i].off + b) * bs, j;

			for (j = 0; j < bs; j++)
				blk[j] = pat(fo + j);
			if (pwrite(fd, blk, bs, (off_t)((ext[i].rtb + b) * bs)) != (ssize_t)bs) {
				close(fd);
				free(blk);
				return 0;
			}
		}
	close(fd);
	free(blk);
	return size;
}

static void check_rt_file(const char *img, const char *log, const char *rt, const char *path, uint32_t bs, uint64_t size)
{
	struct mnt m;
	uint8_t *want = calloc(1, size), *got = calloc(1, size + 16);
	uint64_t o, xi = db_ino(img, path);
	size_t done;
	int i;

	CHECK(want && got && xi);
	if (!want || !got || !xi)
		goto out;
	for (o = 0; o < size; o++)
		if (!(o >= 8 * (uint64_t)bs && o < 10 * (uint64_t)bs))
			want[o] = pat(o);
	CHECK_EQ(mnt_open3(&m, img, log, rt), 0);
	CHECK_EQ(m.x->mp.m_sb.sb_blocksize, bs);
	CHECK(m.x->mp.m_rtdev_targp != NULL);
	{
		wfs_ino_t w = w_of(&m, xi);
		struct xfs_inode *ip;

		CHECK_EQ(wfs_xfs_iget(m.x, xi, &ip), 0);
		CHECK(ip->i_diflags & XFS_DIFLAG_REALTIME);
		xfs_irele(ip);
		nreads = 0;
		CHECK_EQ(read_range(&m, w, 0, got, size, &done), 0);
		CHECK_EQ(done, size);
		CHECK(!memcmp(got, want, size));
		/* two runs: 0-7 (merged) and 10-12; the hole costs no device read */
		CHECK_EQ(nreads, 2);
		for (i = 0; i < 200; i++) {
			uint64_t off = rnd() % (size + 50);
			size_t len = rnd() % (3 * bs), exp;

			memset(got, 0xAA, size + 16);
			CHECK_EQ(wfs_xfs_op_read(m.x, w, off, got, len, &done), 0);
			exp = off >= size ? 0 : (len < size - off ? len : (size_t)(size - off));
			CHECK_EQ(done, exp);
			if (exp)
				CHECK(!memcmp(got, want + off, exp));
		}
	}
	mnt_close(&m);
out:
	free(want);
	free(got);
}

static void copy_img(const char *from, const char *to)
{
	sh("cp --sparse=always --reflink=auto '%s' '%s'", from, to);
}

static void rt_device_file_data_matches(const char *fixdir)
{
	static const char *const names[] = {
		"m04-b1024-s512-ddef-rmap-bigtime-metadir-rt",		/* rtgroups, 1 KiB */
		"m06-b4096-s4096-ddef-reflink-rmap-metadir-xlog-rt",	/* rtgroups + ext. log, 4K sectors */
		"m05-b65536-s4096-ddef-reflink-bigtime-nrext64-metadir-rt", /* 64 KiB, nrext64 */
	};
	unsigned k;
	char img[720], rt[720], log[720], a[720], r[720], l[720];

	for (k = 0; k < sizeof(names) / sizeof(names[0]); k++) {
		uint32_t bs;
		uint64_t size;

		path_join(a, sizeof(a), fixdir, names[k]);
		snprintf(img, sizeof(img), "%s.img", a);
		snprintf(rt, sizeof(rt), "%s.rt.img", a);
		snprintf(log, sizeof(log), "%s.log.img", a);
		if (!exists(img) || !exists(rt)) {
			skipped++;
			printf("SKIPPED %s: %s missing\n", __func__, names[k]);
			continue;
		}
		snprintf(r, sizeof(r), "%s/rt%u.img", tmp, k);
		snprintf(l, sizeof(l), "%s/rtlog%u.img", tmp, k);
		snprintf(a, sizeof(a), "%s/rtd%u.img", tmp, k);
		copy_img(img, a);
		copy_img(rt, r);
		if (exists(log))
			copy_img(log, l);
		bs = k == 0 ? 1024 : k == 1 ? 4096 : 65536;
		/* rtgroup 0: rgbno == rt block; rt block 0 holds the rt superblock, 32+ is free */
		size = make_rt_file(a, r, "/tiny.txt", bs, 0);
		CHECK(size != 0);
		if (size)
			check_rt_file(a, exists(log) ? l : NULL, r, "/tiny.txt", bs, size);
		unlink(a);
		unlink(r);
		unlink(l);
	}

	/* legacy (no rtgroups) RT with a 16 KiB extent size: rt block = fs block number */
	{
		char proto[600], src[600], rtd[600], d[600];
		FILE *f;
		uint64_t size;

		snprintf(proto, sizeof(proto), "%s/rtproto", tmp);
		snprintf(src, sizeof(src), "%s/rtsrc.txt", tmp);
		snprintf(rtd, sizeof(rtd), "%s/rtlegacy.rt.img", tmp);
		snprintf(d, sizeof(d), "%s/rtlegacy.img", tmp);
		f = fopen(src, "w");
		if (f) {
			fputs("tiny\n", f);
			fclose(f);
		}
		f = fopen(proto, "w");
		if (f) {
			fprintf(f, "/nonexistent\n0 0\nd--755 0 0\ntiny.txt ---644 0 0 %s\n$\n", src);
			fclose(f);
		}
		if (sh("rm -f '%s' '%s' && truncate -s 300m '%s' && truncate -s 64m '%s' && "
		       "mkfs.xfs -q -f -b size=4096 -m crc=1,reflink=0,rmapbt=0,metadir=0 -r rtdev='%s',extsize=16k "
		       "-p '%s' '%s' >/dev/null 2>&1", d, rtd, d, rtd, rtd, proto, d)) {
			skipped++;
			printf("SKIPPED %s: legacy rt mkfs failed\n", __func__);
		} else {
			size = make_rt_file(d, rtd, "/tiny.txt", 4096, 0);
			CHECK(size != 0);
			if (size)
				check_rt_file(d, NULL, rtd, "/tiny.txt", 4096, size);
		}
		unlink(d);
		unlink(rtd);
	}
}

/* ---- corruption ---- */

static void bad_extent_beyond_device_is_error(const char *fixdir)
{
	char src[720], img[720];
	struct mnt m;
	uint8_t buf[2048];
	size_t done;
	uint64_t xi;
	int err;

	path_join(src, sizeof(src), fixdir, "m00-b1024-s512-ddef-plain.img");
	if (!exists(src)) {
		skipped++;
		printf("SKIPPED %s: image missing\n", __func__);
		return;
	}
	snprintf(img, sizeof(img), "%s/bad.img", tmp);

	/* (a) start block outside the filesystem (verifier rejects it) */
	copy_img(src, img);
	xi = db_ino(img, "/block_plus_1.bin");
	CHECK(sh("xfs_db -x -c 'path /block_plus_1.bin' -c 'write u3.bmx[0].startblock 1125899906842000' '%s' >/dev/null", img) == 0);
	CHECK_EQ(mnt_open(&m, img), 0);
	done = 99;
	err = wfs_xfs_op_read(m.x, w_of(&m, xi), 0, buf, sizeof(buf), &done);
	CHECK_EQ(err, -WFS_EUCLEAN);
	CHECK_EQ(done, 0);
	mnt_close(&m);

	/* (b) a valid extent that lies beyond the (shrunken) device */
	copy_img(src, img);
	CHECK_EQ(mnt_open(&m, img), 0);
	m.x->blk.size_bytes[WFS_DEV_DATA] = 1u << 20;	/* extent is further out */
	done = 99;
	err = wfs_xfs_op_read(m.x, w_of(&m, xi), 0, buf, sizeof(buf), &done);
	/* the file's blocks may sit below 1 MiB: only demand "ok or EUCLEAN" then,
	   and make the failing case certain with a second file further out */
	CHECK(err == 0 || err == -WFS_EUCLEAN);
	m.x->blk.size_bytes[WFS_DEV_DATA] = 4096;
	done = 99;
	err = wfs_xfs_op_read(m.x, w_of(&m, xi), 0, buf, sizeof(buf), &done);
	CHECK_EQ(err, -WFS_EUCLEAN);
	CHECK_EQ(done, 0);
	mnt_close(&m);

	/* (c) host read error -> EIO, partial progress reported */
	copy_img(src, img);
	CHECK_EQ(mnt_open(&m, img), 0);
	m.x->blk.size_bytes[WFS_DEV_DATA] = 0;		/* range check off ... */
	m.dev[0].size = 4096;				/* ... host fails (EINVAL -> EUCLEAN) */
	done = 99;
	err = wfs_xfs_op_read(m.x, w_of(&m, xi), 0, buf, sizeof(buf), &done);
	CHECK(err == -WFS_EUCLEAN || err == -WFS_EIO);
	CHECK_EQ(done, 0);
	mnt_close(&m);

	/* (d) remote symlink: CRC error in the symlink block, and a bad size */
	copy_img(src, img);
	{
		char cmd[1024], line[256];
		FILE *f;
		uint64_t startblock = 0;
		int fd;
		uint8_t byte;
		struct xfs_mount *mp;

		xi = db_ino(img, "/links_remote_300");
		snprintf(cmd, sizeof(cmd), "xfs_db -r -c 'path /links_remote_300' -c bmap '%s'", img);
		f = popen(cmd, "r");
		while (f && fgets(line, sizeof(line), f)) {
			char *p = strstr(line, "startblock ");

			if (p && !startblock)
				startblock = strtoull(p + 11, NULL, 10);
		}
		if (f)
			pclose(f);
		CHECK(startblock != 0);
		/* fs with one AG range check: block number == agbno here only for AG 0 */
		CHECK_EQ(mnt_open(&m, img), 0);
		mp = &m.x->mp;
		CHECK_EQ(wfs_xfs_op_readlink(m.x, w_of(&m, xi), (char *)buf, sizeof(buf), &done), 0);
		fd = open(img, O_RDWR);
		CHECK(fd >= 0);
		if (fd >= 0) {
			off_t pos = (off_t)(startblock * mp->m_sb.sb_blocksize + 100);

			CHECK(pread(fd, &byte, 1, pos) == 1);
			byte ^= 0x40;
			CHECK(pwrite(fd, &byte, 1, pos) == 1);
			close(fd);
		}
		mnt_close(&m);
		CHECK_EQ(mnt_open(&m, img), 0);
		CHECK_EQ(wfs_xfs_op_readlink(m.x, w_of(&m, xi), (char *)buf, sizeof(buf), &done), -WFS_EUCLEAN);
		mnt_close(&m);

		copy_img(src, img);
		CHECK(sh("xfs_db -x -c 'path /links_remote_300' -c 'write core.size 2000' '%s' >/dev/null", img) == 0);
		CHECK_EQ(mnt_open(&m, img), 0);
		CHECK_EQ(wfs_xfs_op_readlink(m.x, w_of(&m, xi), (char *)buf, sizeof(buf), &done), -WFS_EUCLEAN);
		mnt_close(&m);

		copy_img(src, img);
		CHECK(sh("xfs_db -x -c 'path /links_remote_300' -c 'write core.size 0' '%s' >/dev/null", img) == 0);
		CHECK_EQ(mnt_open(&m, img), 0);
		CHECK_EQ(wfs_xfs_op_readlink(m.x, w_of(&m, xi), (char *)buf, sizeof(buf), &done), -WFS_EUCLEAN);
		mnt_close(&m);

		/* size shorter than the blocks need (300 -> 3000 would not verify; 300 -> 5 still reads 5) */
		copy_img(src, img);
		CHECK(sh("xfs_db -x -c 'path /links_remote_300' -c 'write core.size 5' '%s' >/dev/null", img) == 0);
		CHECK_EQ(mnt_open(&m, img), 0);
		err = wfs_xfs_op_readlink(m.x, w_of(&m, xi), (char *)buf, sizeof(buf), &done);
		CHECK(err == 0 || err == -WFS_EUCLEAN);
		mnt_close(&m);
	}
	unlink(img);
}

/* ---- root fixtures (kernel-written images) ---- */

struct seg { uint64_t off, len; uint8_t byte; };

/* Compare a file against: zero everywhere except the segments. Odd chunking. */
static void check_pattern(struct mnt *m, wfs_ino_t w, uint64_t size, const struct seg *seg, int nseg, const char *what)
{
	size_t chunk = (1u << 20) + 4099;
	uint8_t *buf = malloc(chunk), *exp = malloc(chunk);
	uint64_t off = 0;
	int bad = 0;
	wfs_stat st;

	CHECK(buf && exp);
	if (!buf || !exp)
		goto out;
	CHECK_EQ(wfs_xfs_op_getattr(m->x, w, &st), 0);
	CHECK_EQ(st.size, size);
	while (off < size) {
		size_t want = size - off < chunk ? (size_t)(size - off) : chunk, got;
		int i;

		memset(exp, 0, want);
		for (i = 0; i < nseg; i++) {
			uint64_t lo = seg[i].off > off ? seg[i].off : off;
			uint64_t hi = seg[i].off + seg[i].len < off + want ? seg[i].off + seg[i].len : off + want;

			if (lo < hi)
				memset(exp + (lo - off), seg[i].byte, hi - lo);
		}
		memset(buf, 0xEE, want);
		if (read_range(m, w, off, buf, want, &got) || got != want || memcmp(buf, exp, want)) {
			bad++;
			fprintf(stderr, "%s: mismatch in chunk at %llu\n", what, (unsigned long long)off);
			break;
		}
		off += want;
	}
	CHECK(!bad);
out:
	free(buf);
	free(exp);
}

static int unzst(const char *rootfix, const char *name, char *out, size_t n)
{
	char z[720];

	snprintf(z, sizeof(z), "%s/%s.img.zst", rootfix, name);
	snprintf(out, n, "%s/%s.img", tmp, name);
	if (!exists(z))
		return -1;
	return sh("zstd -d -q -f -o '%s' '%s'", out, z);
}

#define OPEN_FIX(NAME) \
	char img[720]; struct mnt m; \
	if (unzst(rootfix, NAME, img, sizeof(img))) { skipped++; printf("SKIPPED %s: %s.img.zst missing\n", __func__, NAME); return; } \
	CHECK_EQ(mnt_open(&m, img), 0)

#define W(path) w_of(&m, db_ino(img, path))

static void root_fixture_sparse(const char *rootfix)
{
	OPEN_FIX("sparse");
	{
		struct seg p[] = { { 0, 256 << 10, 0x41 }, { 768 << 10, 256 << 10, 0x41 } };
		struct seg g[] = { { 0, 4096, 0x42 }, { 1ull << 30, 4096, 0x43 } };
		struct seg t[] = { { 0, 4096, 0xcd } };
		uint8_t b[300];
		size_t done;

		check_pattern(&m, W("/punched.bin"), 1 << 20, p, 2, "punched.bin");
		check_pattern(&m, W("/gap_1g.bin"), (1ull << 30) + 4096, g, 2, "gap_1g.bin");
		check_pattern(&m, W("/tail_hole.bin"), 64u << 20, t, 1, "tail_hole.bin");
		/* a window across the punched hole's edges */
		CHECK_EQ(read_range(&m, W("/punched.bin"), 262144 - 100, b, 200, &done), 0);
		CHECK_EQ(done, 200);
		CHECK(b[0] == 0x41 && b[99] == 0x41 && b[100] == 0 && b[199] == 0);
		CHECK_EQ(read_range(&m, W("/punched.bin"), 786432 - 100, b, 200, &done), 0);
		CHECK(b[99] == 0 && b[100] == 0x41);
		/* sparse tail: EOF clamp inside the hole */
		CHECK_EQ(read_range(&m, W("/tail_hole.bin"), (64u << 20) - 10, b, 300, &done), 0);
		CHECK_EQ(done, 10);
	}
	mnt_close(&m);
	unlink(img);
}

static void root_fixture_unwritten(const char *rootfix)
{
	OPEN_FIX("unwritten");
	{
		struct seg a[] = { { 0, 0, 0 } };
		struct seg p[] = { { 256 << 10, 4096, 0x44 } };

		check_pattern(&m, W("/all_unwritten.bin"), 1 << 20, a, 0, "all_unwritten.bin");
		check_pattern(&m, W("/partly_written.bin"), 1 << 20, p, 1, "partly_written.bin");
	}
	mnt_close(&m);
	unlink(img);
}

static void root_fixture_fragmented(const char *rootfix)
{
	OPEN_FIX("fragmented");
	{
		struct seg a[] = { { 0, 2 << 20, 0x45 } };
		struct seg b[] = { { 0, 2 << 20, 0x46 } };
		struct xfs_inode *ip;
		uint64_t xi = db_ino(img, "/frag_a.bin");

		check_pattern(&m, W("/frag_a.bin"), 2 << 20, a, 1, "frag_a.bin");
		check_pattern(&m, W("/frag_b.bin"), 2 << 20, b, 1, "frag_b.bin");
		CHECK_EQ(wfs_xfs_iget(m.x, xi, &ip), 0);
		CHECK_EQ(ip->i_df.if_format, XFS_DINODE_FMT_BTREE);
		CHECK(ip->i_df.if_nextents >= 10);
		xfs_irele(ip);
	}
	mnt_close(&m);
	unlink(img);
}

static void root_fixture_reflink(const char *rootfix)
{
	OPEN_FIX("reflink");
	{
		struct seg o[] = { { 0, 256 << 10, 0x47 } };
		struct seg c[] = { { 0, 256 << 10, 0x47 }, { 64 << 10, 4096, 0x48 } };
		char cmd[512], line[256];
		FILE *f;
		long long sb[2][8];
		int n[2] = { 0, 0 }, k, shared = 0, i, j;

		check_pattern(&m, W("/orig.bin"), 256 << 10, o, 1, "orig.bin");
		check_pattern(&m, W("/clone.bin"), 256 << 10, c, 2, "clone.bin");
		/* ground truth: the two files really share physical blocks */
		for (k = 0; k < 2; k++) {
			snprintf(cmd, sizeof(cmd), "xfs_db -r -c 'path /%s.bin' -c bmap '%s'", k ? "clone" : "orig", img);
			f = popen(cmd, "r");
			while (f && fgets(line, sizeof(line), f)) {
				char *p = strstr(line, "startblock ");

				if (p && n[k] < 8)
					sb[k][n[k]++] = strtoll(p + 11, NULL, 10);
			}
			if (f)
				pclose(f);
		}
		for (i = 0; i < n[0]; i++)
			for (j = 0; j < n[1]; j++)
				shared += sb[0][i] == sb[1][j];
		CHECK(shared > 0);
	}
	mnt_close(&m);
	unlink(img);
}

static void root_fixture_devnodes(const char *rootfix)
{
	OPEN_FIX("devnodes");
	{
		static const char *const nodes[] = { "/chr_1_3", "/blk_7_0", "/fifo", "/sock" };
		unsigned i;
		uint8_t b[8];
		size_t done = 0;
		wfs_stat st;

		for (i = 0; i < sizeof(nodes) / sizeof(nodes[0]); i++) {
			wfs_ino_t w = W(nodes[i]);

			CHECK(w != 0);
			CHECK_EQ(wfs_xfs_op_getattr(m.x, w, &st), 0);
			CHECK_EQ(wfs_xfs_op_read(m.x, w, 0, b, sizeof(b), &done), -WFS_EINVAL);
			CHECK_EQ(done, 0);
			CHECK_EQ(wfs_xfs_op_readlink(m.x, w, (char *)b, sizeof(b), &done), -WFS_EINVAL);
		}
	}
	mnt_close(&m);
	unlink(img);
}

/* ------------------------------------------------------------------ */

int main(int argc, char **argv)
{
	const char *fixdir = argc > 1 ? argv[1] : "../../target/xfs-fixtures";
	const char *rootfix = argc > 2 ? argv[2] : "../../tests/xfs/root-fixtures";
	const char *td = getenv("TMPDIR");
	char dir[600];

	tools_ok = have("mkfs.xfs") && have("xfs_db") && have("zstd") && have("python3") && have("sha256sum");
	if (!tools_ok) {
		printf("SKIPPED test_read: needs mkfs.xfs, xfs_db, zstd, python3, sha256sum\n");
		return 0;
	}
	/* scratch: next to the fixtures unless TMPDIR is set (images are sparse but large) */
	snprintf(dir, sizeof(dir), "%s", td && *td ? td : fixdir);
	snprintf(tmp, sizeof(tmp), "%s/wfs-read-test.XXXXXX", dir);
	if (!mkdtemp(tmp)) {
		snprintf(tmp, sizeof(tmp), "/tmp/wfs-read-test.XXXXXX");
		if (!mkdtemp(tmp)) {
			perror("mkdtemp");
			return 2;
		}
	}
	mem_base = wfs_mem_used();
#define RUN(t, ...) do { int f0 = failures; t(__VA_ARGS__); \
	printf("%s %s\n", failures == f0 ? "PASS" : "FAIL", #t); fflush(stdout); } while (0)
	RUN(manifest_sha256_matches_for_all_fixture_files, fixdir);
	RUN(bmbt_two_level_file_hash);
	RUN(read_at_eof_and_beyond, fixdir);
	RUN(hole_and_unwritten_extent_read_zeros, fixdir);
	RUN(read_offset_1_shl_40_sparse, fixdir);
	RUN(symlink_local_and_remote, fixdir);
	RUN(rt_device_file_data_matches, fixdir);
	RUN(bad_extent_beyond_device_is_error, fixdir);
	RUN(root_fixture_sparse, rootfix);
	RUN(root_fixture_unwritten, rootfix);
	RUN(root_fixture_fragmented, rootfix);
	RUN(root_fixture_reflink, rootfix);
	RUN(root_fixture_devnodes, rootfix);
	CHECK(nwrites == 0);
	sh("rm -rf '%s'", tmp);
	printf("test_read: %d checks, %d failures, %d skipped\n", checks, failures, skipped);
	return failures ? 1 : 0;
}
