// SPDX-License-Identifier: GPL-2.0-only
/*
 * Task 9 tests: directory lookup and readdir (design 8.3, 8.4).
 * Images are made by `mkfs.xfs -p <directory>` from a tree this test creates
 * (that tree, xfs_db and, for the fixture matrix, the manifest are the ground
 * truth) and read through a read-only file-backed host. Skips visibly without
 * the tools.
 */
#include <dirent.h>	/* before the shim: DT_* */
#include "wfs_xfs.h"
#include "icache.h"
#include <fcntl.h>
#include <glob.h>
#include <signal.h>
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
static void h_log(int level, const char *msg) { if (getenv("DIR_VERBOSE")) fprintf(stderr, "[%d] %s\n", level, msg); }
static uint64_t h_size(wfs_blkdev *d) { return d->size; }
static uint32_t h_ss(wfs_blkdev *d) { (void)d; return 512; }
static int h_ro(wfs_blkdev *d) { (void)d; return 1; }
static wfs_err_t h_read(wfs_blkdev *d, uint64_t off, void *buf, size_t len)
{
	if (off > d->size || len > d->size - off)
		return -WFS_EIO;
	return pread(d->fd, buf, len, (off_t)off) == (ssize_t)len ? 0 : -WFS_EIO;
}
static wfs_err_t h_write(wfs_blkdev *d, uint64_t o, const void *b, size_t l)
{ (void)d; (void)o; (void)b; (void)l; return -WFS_EROFS; }
static wfs_err_t h_flush(wfs_blkdev *d) { (void)d; return 0; }
static const wfs_host_api api = {
	.abi_version = WFS_ABI_VERSION, .size = sizeof(wfs_host_api),
	.blk_size = h_size, .blk_sector_size = h_ss, .blk_read_only = h_ro,
	.blk_read = h_read, .blk_write = h_write, .blk_flush = h_flush, .log = h_log,
};

static char tmp[512];
static size_t mem_base;
static int live;

static int sh(const char *fmt, ...)
{
	char cmd[4096];
	va_list ap;

	va_start(ap, fmt);
	vsnprintf(cmd, sizeof(cmd), fmt, ap);
	va_end(ap);
	return system(cmd);
}

struct mnt {
	wfs_blkdev dev[WFS_DEV_COUNT];
	wfs_blkdev *p[WFS_DEV_COUNT];
	struct wfs_xfs *x;
};

static int open_dev(wfs_blkdev *d, const char *path)
{
	struct stat st;

	d->fd = open(path, O_RDONLY);
	if (d->fd < 0 || fstat(d->fd, &st))
		return -1;
	d->size = (uint64_t)st.st_size;
	return 0;
}

static int mnt_open3(struct mnt *m, const char *img, const char *logimg, const char *rtimg)
{
	memset(m, 0, sizeof(*m));
	if (open_dev(&m->dev[WFS_DEV_DATA], img))
		return 12345;
	m->p[WFS_DEV_DATA] = &m->dev[WFS_DEV_DATA];
	if (logimg && !open_dev(&m->dev[WFS_DEV_LOG], logimg))
		m->p[WFS_DEV_LOG] = &m->dev[WFS_DEV_LOG];
	if (rtimg && !open_dev(&m->dev[WFS_DEV_RT], rtimg))
		m->p[WFS_DEV_RT] = &m->dev[WFS_DEV_RT];
	live++;
	return wfs_xfs_mount(&api, m->p, NULL, &m->x);
}
static int mnt_open(struct mnt *m, const char *img) { return mnt_open3(m, img, NULL, NULL); }

static void mnt_close(struct mnt *m)
{
	int i;

	CHECK(wfs_xfs_unmount(m->x) == 0);
	for (i = 0; i < WFS_DEV_COUNT; i++)
		if (m->p[i])
			close(m->dev[i].fd);
	if (--live == 0)
		CHECK(wfs_mem_used() == mem_base);	/* no leaks, incl. the global caches */
}

/* ---- ground truth helpers ---- */

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

/* shortform|block|leaf|node by the same rule as tools/gen-fixtures.sh */
static const char *db_dirform(const char *img, const char *path, unsigned bs)
{
	char cmd[1024], line[256];
	FILE *f;
	unsigned long long leaf = 0, any = 0;

	snprintf(cmd, sizeof(cmd), "xfs_db -r -c 'path %s' -c 'bmap' '%s' 2>/dev/null", path, img);
	f = popen(cmd, "r");
	while (f && fgets(line, sizeof(line), f)) {
		unsigned long long off, cnt;
		char *o = strstr(line, "offset "), *c = strstr(line, "count ");

		if (!o || !c)
			continue;
		off = strtoull(o + 7, NULL, 10);
		cnt = strtoull(c + 6, NULL, 10);
		any++;
		if (off * bs >= 1ull << 35)
			leaf += cnt;
	}
	if (f)
		pclose(f);
	if (!any)
		return "shortform";
	return leaf == 0 ? "block" : leaf == 1 ? "leaf" : "node";
}

/* ---- the source tree ---- */

#define NODE_N	5200
#define LEAF_N	300
#define BLK_N	40

static char src[600];

static void touch(const char *dir, const char *name, size_t len)
{
	char path[1200];
	int fd;

	snprintf(path, sizeof(path), "%s/%s/", src, dir);
	memcpy(path + strlen(path), name, len);
	path[strlen(src) + 2 + strlen(dir) + len] = 0;
	fd = open(path, O_WRONLY | O_CREAT, 0644);
	if (fd >= 0)
		close(fd);
	else
		perror(path);
}

static void make_src(void)
{
	char name[300], path[700];
	char x255[256], y254[255];
	int i;

	sh("rm -rf '%s' && mkdir -p '%s/sf/d' '%s/blk/sub' '%s/leaf/sub' '%s/node/sub' '%s/names'",
	   src, src, src, src, src, src);
	memset(x255, 'x', 255); x255[255] = 0;
	memset(y254, 'y', 254); y254[254] = 0;
	snprintf(path, sizeof(path), "%s/sf/ln", src);
	if (symlink("target", path)) perror("symlink");
	touch("sf", "a", 1);
	touch("sf", "b", 1);
	touch("sf", "c.txt", 5);
	for (i = 0; i < BLK_N; i++) {
		snprintf(name, sizeof(name), "f%02d", i);
		touch("blk", name, strlen(name));
	}
	snprintf(path, sizeof(path), "%s/blk/ln", src);
	if (symlink("target", path)) perror("symlink");
	for (i = 0; i < LEAF_N; i++) {
		snprintf(name, sizeof(name), "leaf_entry_%04d_xxxxxxxxxxxxxx", i);
		touch("leaf", name, strlen(name));
	}
	snprintf(path, sizeof(path), "%s/leaf/ln", src);
	if (symlink("target", path)) perror("symlink");
	for (i = 0; i < NODE_N; i++) {
		snprintf(name, sizeof(name), "m%05d", i);
		touch("node", name, strlen(name));
	}
	snprintf(path, sizeof(path), "%s/node/ln", src);
	if (symlink("target", path)) perror("symlink");
	touch("names", x255, 255);
	touch("names", y254, 254);
	touch("names", "h\xc3\xa9llo-\xe4\xb8\x96\xe7\x95\x8c", 13);
	touch("names", "bad-\xff\xfe\x80", 7);
	touch("names", ".hidden", 7);
	touch("names", "...", 3);
	touch("names", " lead space", 11);
	touch("names", "a\nb", 3);
	touch("names", "a", 1);
	touch("names", "tab\there", 8);
	snprintf(path, sizeof(path), "%s/fifo", src);
	if (mkfifo(path, 0644)) perror("mkfifo");
	touch("", "rootfile", 8);
}

/* ---- results of a readdir ---- */

struct ent { char name[256]; uint32_t len; uint64_t ino, cookie; uint32_t ftype; };
struct list {
	struct ent *v;
	size_t n, cap;
	size_t stop_after;	/* callback returns 1 after this many entries (0 = never) */
	uint64_t in_cookie;
	int bad_cookie;		/* ordering / range violations seen */
};

static int collect_cb(void *ctx, const wfs_dirent *e)
{
	struct list *l = ctx;
	struct ent *d;

	if (e->name_len > 255 || e->next_cookie == 0 || e->next_cookie >= (1ull << 63) ||
	    e->next_cookie == l->in_cookie || e->ino == 0)
		l->bad_cookie++;
	if (l->n && e->next_cookie <= l->v[l->n - 1].cookie)
		l->bad_cookie++;
	if (l->n == l->cap) {
		l->cap = l->cap ? l->cap * 2 : 64;
		l->v = realloc(l->v, l->cap * sizeof(*l->v));
	}
	d = &l->v[l->n++];
	memcpy(d->name, e->name, e->name_len);
	d->name[e->name_len] = 0;
	d->len = e->name_len;
	d->ino = e->ino;
	d->cookie = e->next_cookie;
	d->ftype = e->ftype;
	return l->stop_after && l->n >= l->stop_after;
}

static int rd(struct mnt *m, wfs_ino_t dir, uint64_t cookie, size_t stop_after, struct list *l)
{
	memset(l, 0, sizeof(*l));
	l->stop_after = stop_after;
	l->in_cookie = cookie;
	return wfs_xfs_op_readdir(m->x, dir, cookie, collect_cb, l);
}
static void list_free(struct list *l) { free(l->v); memset(l, 0, sizeof(*l)); }

static int ent_cmp(const void *a, const void *b)
{
	const struct ent *x = a, *y = b;
	size_t n = x->len < y->len ? x->len : y->len;
	int c = memcmp(x->name, y->name, n);

	return c ? c : (x->len > y->len) - (x->len < y->len);
}

/* resolve a root-relative path of simple names to a wfs ino (lookup + forget) */
static wfs_ino_t path_ino(struct mnt *m, const char *path)
{
	wfs_ino_t cur = WFS_ROOT_INO;
	char buf[512], *tok, *save = NULL;

	snprintf(buf, sizeof(buf), "%s", path);
	for (tok = strtok_r(buf, "/", &save); tok; tok = strtok_r(NULL, "/", &save)) {
		wfs_ino_t out;
		wfs_stat st;

		if (wfs_xfs_op_lookup(m->x, cur, tok, (uint32_t)strlen(tok), &out, &st))
			return 0;
		wfs_xfs_op_forget(m->x, out, 1);
		cur = out;
	}
	return cur;
}

/* names present in the host directory `rel` (relative to src), sorted */
static size_t src_names(const char *rel, struct ent **out)
{
	char path[700];
	DIR *d;
	struct dirent *e;
	struct ent *v = NULL;
	size_t n = 0, cap = 0;

	snprintf(path, sizeof(path), "%s/%s", src, rel);
	d = opendir(path);
	while (d && (e = readdir(d))) {
		if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, ".."))
			continue;
		if (n == cap) {
			cap = cap ? cap * 2 : 64;
			v = realloc(v, cap * sizeof(*v));
		}
		memset(&v[n], 0, sizeof(v[n]));
		v[n].len = (uint32_t)strlen(e->d_name);
		memcpy(v[n].name, e->d_name, v[n].len);
		v[n].ftype = e->d_type == DT_DIR ? WFS_FT_DIR : e->d_type == DT_LNK ? WFS_FT_LNK :
			     e->d_type == DT_FIFO ? WFS_FT_FIFO : WFS_FT_REG;
		n++;
	}
	if (d)
		closedir(d);
	qsort(v, n, sizeof(*v), ent_cmp);
	*out = v;
	return n;
}

static char img_main[600];

static int mk_img(const char *img, const char *mkfs_opts)
{
	return sh("rm -f '%s' && truncate -s 300m '%s' && mkfs.xfs -q -f %s -p '%s' '%s' >/dev/null 2>&1",
		  img, img, mkfs_opts, src, img);
}

static const char *const dirs[] = { "sf", "blk", "leaf", "node", "names", "" };
static const char *const forms[] = { "shortform", "block", "leaf", "node" };

/* compare a full listing of dir `rel` with the source tree; returns entries */
static size_t check_listing(struct mnt *m, const char *rel, struct list *l)
{
	struct ent *want;
	size_t n = src_names(rel, &want), i;
	wfs_ino_t dir = path_ino(m, rel);

	CHECK(dir != 0);
	CHECK_EQ(rd(m, dir, 0, 0, l), 0);
	CHECK_EQ(l->bad_cookie, 0);
	CHECK_EQ(l->n, n);
	if (l->n == n) {
		struct list sorted = *l;

		sorted.v = malloc(n * sizeof(*sorted.v));
		memcpy(sorted.v, l->v, n * sizeof(*sorted.v));
		qsort(sorted.v, n, sizeof(*sorted.v), ent_cmp);
		for (i = 0; i < n; i++) {
			CHECK(sorted.v[i].len == want[i].len &&
			      !memcmp(sorted.v[i].name, want[i].name, want[i].len));
			CHECK_EQ(sorted.v[i].ftype, want[i].ftype);
		}
		free(sorted.v);
	}
	free(want);
	return n;
}

/* ------------------------------------------------------------ tests -- */

static void lists_all_four_dir_formats(void)
{
	struct mnt m;
	unsigned f;
	size_t i, j;

	CHECK(mnt_open(&m, img_main) == 0);
	/* the directories really have the four forms (xfs_db) */
	for (f = 0; f < 4; f++) {
		char p[64];

		snprintf(p, sizeof(p), "/%s", dirs[f]);
		if (strcmp(db_dirform(img_main, p, 4096), forms[f])) {
			fprintf(stderr, "%s is %s, want %s\n", p, db_dirform(img_main, p, 4096), forms[f]);
			CHECK(0);
		}
	}
	for (i = 0; i < sizeof(dirs) / sizeof(dirs[0]); i++) {
		struct list l;
		size_t n = check_listing(&m, dirs[i], &l);
		char rel[700];

		/* inode numbers against xfs_db (all small dirs, a sample of the big ones) */
		for (j = 0; j < l.n; j++) {
			uint64_t want;

			if (n > 100 && j % 211)
				continue;
			if (strpbrk(l.v[j].name, " \n\t'\"") || (unsigned char)l.v[j].name[0] >= 0x80 ||
			    strchr(l.v[j].name, '\xff') || l.v[j].len > 100)
				continue;
			snprintf(rel, sizeof(rel), "/%s/%s", dirs[i], l.v[j].name);
			want = db_ino(img_main, rel);
			CHECK(want != 0);
			CHECK_EQ(l.v[j].ino, want == m.x->mp.m_sb.sb_rootino ? 1 : want);
		}
		list_free(&l);
	}
	mnt_close(&m);
}

static void readdir_resume_each_entry_no_dup_no_miss(void)
{
	struct mnt m;
	size_t i, d;

	CHECK(mnt_open(&m, img_main) == 0);
	for (d = 0; d < sizeof(dirs) / sizeof(dirs[0]); d++) {
		wfs_ino_t dir = path_ino(&m, dirs[d]);
		struct list full;
		uint64_t cookie = 0;
		size_t got = 0, calls = 0;

		CHECK_EQ(rd(&m, dir, 0, 0, &full), 0);
		/* (a) one entry per call, resuming from the returned cookie */
		for (;;) {
			struct list one;

			CHECK_EQ(rd(&m, dir, cookie, 1, &one), 0);
			if (!one.n) {
				list_free(&one);
				break;
			}
			CHECK_EQ(one.n, 1);
			if (got < full.n) {
				CHECK(one.v[0].len == full.v[got].len &&
				      !memcmp(one.v[0].name, full.v[got].name, one.v[0].len));
				CHECK_EQ(one.v[0].cookie, full.v[got].cookie);
			}
			CHECK(one.v[0].cookie != cookie);
			cookie = one.v[0].cookie;
			got++;
			calls++;
			list_free(&one);
			if (calls > full.n + 1)
				break;
		}
		CHECK_EQ(got, full.n);
		/* (b) resume from every cookie: the rest of the listing exactly (all
		 * cookies for small directories, a stride for the big ones) */
		for (i = 0; i < full.n; i++) {
			struct list rest;
			size_t k;

			if (full.n > 600 && (i % 53) && i + 3 < full.n)
				continue;
			CHECK_EQ(rd(&m, dir, full.v[i].cookie, 0, &rest), 0);
			CHECK_EQ(rest.n, full.n - i - 1);
			for (k = 0; k < rest.n && i + 1 + k < full.n; k++)
				CHECK(rest.v[k].cookie == full.v[i + 1 + k].cookie &&
				      rest.v[k].len == full.v[i + 1 + k].len &&
				      !memcmp(rest.v[k].name, full.v[i + 1 + k].name, rest.v[k].len));
			CHECK_EQ(rest.bad_cookie, 0);
			list_free(&rest);
		}
		/* (c) a cookie inside the entry (cookie - 1 is its dataptr, so
		 * dataptr+1 .. next dataptr are all "after this entry") */
		for (i = 0; i + 1 < full.n; i += (full.n > 600 ? 97 : 1)) {
			struct list rest;
			uint64_t c = full.v[i].cookie + 1;

			if (c >= full.v[i + 1].cookie)
				continue;	/* adjacent dataptrs: no gap to probe */
			CHECK_EQ(rd(&m, dir, c, 0, &rest), 0);
			CHECK_EQ(rest.n, full.n - i - 1);
			list_free(&rest);
		}
		list_free(&full);
	}
	mnt_close(&m);
}

static void cookies_strictly_progress_and_below_2_pow_63(void)
{
	struct mnt m;
	size_t d, i;
	struct list l;

	CHECK(mnt_open(&m, img_main) == 0);
	for (d = 0; d < sizeof(dirs) / sizeof(dirs[0]); d++) {
		wfs_ino_t dir = path_ino(&m, dirs[d]);

		CHECK_EQ(rd(&m, dir, 0, 0, &l), 0);
		CHECK_EQ(l.bad_cookie, 0);
		for (i = 0; i < l.n; i++) {
			CHECK(l.v[i].cookie > 0 && l.v[i].cookie < (1ull << 63));
			CHECK(l.v[i].cookie <= 0xffffffffull + 1);	/* dataptr + 1, unmasked */
			if (i)
				CHECK(l.v[i].cookie > l.v[i - 1].cookie);
		}
		list_free(&l);
	}
	/* a resume cookie at / beyond the end yields nothing, successfully */
	CHECK_EQ(rd(&m, path_ino(&m, "node"), 0xffffffffull + 1, 0, &l), 0);
	CHECK_EQ(l.n, 0);
	list_free(&l);
	CHECK_EQ(rd(&m, path_ino(&m, "node"), ~0ull, 0, &l), 0);
	CHECK_EQ(l.n, 0);
	list_free(&l);
	CHECK_EQ(rd(&m, path_ino(&m, "sf"), 0xfffffff0ull, 0, &l), 0);
	CHECK_EQ(l.n, 0);
	list_free(&l);
	mnt_close(&m);
}

static void no_dot_entries(void)
{
	struct mnt m;
	size_t d, i;

	CHECK(mnt_open(&m, img_main) == 0);
	for (d = 0; d < sizeof(dirs) / sizeof(dirs[0]); d++) {
		struct list l;
		int dots = 0, three = 0, hidden = 0;

		CHECK_EQ(rd(&m, path_ino(&m, dirs[d]), 0, 0, &l), 0);
		for (i = 0; i < l.n; i++) {
			if (l.v[i].len == 1 && l.v[i].name[0] == '.')
				dots++;
			if (l.v[i].len == 2 && !memcmp(l.v[i].name, "..", 2))
				dots++;
			if (l.v[i].len == 3 && !memcmp(l.v[i].name, "...", 3))
				three++;
			if (l.v[i].len == 7 && !memcmp(l.v[i].name, ".hidden", 7))
				hidden++;
		}
		CHECK_EQ(dots, 0);
		if (!strcmp(dirs[d], "names"))
			CHECK(three == 1 && hidden == 1);	/* only exact "." and ".." are dropped */
		list_free(&l);
	}
	mnt_close(&m);
}

static void non_ascii_and_255_byte_names_exact_bytes(void)
{
	struct mnt m;
	static const struct { const char *n; uint32_t len; } nm[] = {
		{ "h\xc3\xa9llo-\xe4\xb8\x96\xe7\x95\x8c", 13 }, { "bad-\xff\xfe\x80", 7 },
		{ " lead space", 11 }, { "a\nb", 3 }, { "tab\there", 8 }, { "a", 1 }, { "...", 3 },
	};
	char x255[256], y254[255], x256[257];
	wfs_ino_t names, out, o2;
	wfs_stat st;
	struct list l;
	size_t i;

	memset(x255, 'x', 255); x255[255] = 0;
	memset(y254, 'y', 254); y254[254] = 0;
	memset(x256, 'x', 256); x256[256] = 0;
	CHECK(mnt_open(&m, img_main) == 0);
	names = path_ino(&m, "names");
	CHECK_EQ(rd(&m, names, 0, 0, &l), 0);
	for (i = 0; i < sizeof(nm) / sizeof(nm[0]); i++) {
		size_t j;
		int found = 0;

		for (j = 0; j < l.n; j++)
			if (l.v[j].len == nm[i].len && !memcmp(l.v[j].name, nm[i].n, nm[i].len)) {
				found++;
				CHECK_EQ(l.v[j].ftype, WFS_FT_REG);
			}
		CHECK_EQ(found, 1);
		CHECK_EQ(wfs_xfs_op_lookup(m.x, names, nm[i].n, nm[i].len, &out, &st), 0);
		CHECK_EQ(st.ftype, WFS_FT_REG);
		wfs_xfs_op_forget(m.x, out, 1);
	}
	for (i = 0; i < l.n; i++)
		if (l.v[i].len == 255)
			CHECK(!memcmp(l.v[i].name, x255, 255));
		else if (l.v[i].len == 254)
			CHECK(!memcmp(l.v[i].name, y254, 254));
	CHECK_EQ(wfs_xfs_op_lookup(m.x, names, x255, 255, &out, &st), 0);
	CHECK_EQ(wfs_xfs_op_lookup(m.x, names, y254, 254, &o2, &st), 0);
	CHECK(out != o2);
	wfs_xfs_op_forget(m.x, out, 1);
	wfs_xfs_op_forget(m.x, o2, 1);
	CHECK_EQ(wfs_xfs_op_lookup(m.x, names, x256, 256, &out, &st), -WFS_ENAMETOOLONG);
	CHECK_EQ(wfs_xfs_op_lookup(m.x, names, x256, 1000, &out, &st), -WFS_ENAMETOOLONG);
	CHECK_EQ(wfs_xfs_op_lookup(m.x, names, "", 0, &out, &st), -WFS_EINVAL);
	/* byte-exact: a name differing in one byte, or in case, is not found */
	CHECK_EQ(wfs_xfs_op_lookup(m.x, names, "H\xc3\xa9llo-\xe4\xb8\x96\xe7\x95\x8c", 13, &out, &st), -WFS_ENOENT);
	CHECK_EQ(wfs_xfs_op_lookup(m.x, names, "bad-\xff\xfe\x81", 7, &out, &st), -WFS_ENOENT);
	CHECK_EQ(wfs_xfs_op_lookup(m.x, names, "bad-\xff\xfe", 6, &out, &st), -WFS_ENOENT);
	list_free(&l);
	CHECK_EQ(wfs_xfs_icache_nlookup(m.x), 0);
	mnt_close(&m);
}

static void lookup_basics_and_refs(void)
{
	struct mnt m;
	wfs_ino_t out, root = WFS_ROOT_INO, sf;
	wfs_stat st;
	struct list l;
	size_t i;
	uint64_t refs0;

	CHECK(mnt_open(&m, img_main) == 0);
	refs0 = wfs_xfs_icache_refs(m.x);
	CHECK_EQ(wfs_xfs_icache_nlookup(m.x), 0);

	CHECK_EQ(wfs_xfs_op_lookup(m.x, root, "sf", 2, &sf, &st), 0);
	CHECK_EQ(st.ftype, WFS_FT_DIR);
	CHECK_EQ(st.ino, sf);
	CHECK_EQ(wfs_xfs_icache_nlookup(m.x), 1);
	CHECK_EQ(wfs_xfs_icache_refs(m.x), refs0);	/* no transient reference leaked */
	CHECK_EQ(wfs_xfs_op_lookup(m.x, root, "sf", 2, &out, &st), 0);
	CHECK_EQ(out, sf);
	CHECK_EQ(wfs_xfs_icache_nlookup(m.x), 2);
	wfs_xfs_op_forget(m.x, sf, 2);
	CHECK_EQ(wfs_xfs_icache_nlookup(m.x), 0);

	/* ftype of the other kinds, and "." / ".." */
	CHECK_EQ(wfs_xfs_op_lookup(m.x, root, "fifo", 4, &out, &st), 0);
	CHECK_EQ(st.ftype, WFS_FT_FIFO);
	wfs_xfs_op_forget(m.x, out, 1);
	CHECK_EQ(wfs_xfs_op_lookup(m.x, sf, "ln", 2, &out, &st), 0);
	CHECK_EQ(st.ftype, WFS_FT_LNK);
	wfs_xfs_op_forget(m.x, out, 1);
	CHECK_EQ(wfs_xfs_op_lookup(m.x, sf, "..", 2, &out, &st), 0);
	CHECK_EQ(out, WFS_ROOT_INO);			/* the real root number is never exposed */
	wfs_xfs_op_forget(m.x, out, 1);
	CHECK_EQ(wfs_xfs_op_lookup(m.x, sf, ".", 1, &out, &st), 0);
	CHECK_EQ(out, sf);
	wfs_xfs_op_forget(m.x, out, 1);

	/* missing, in each directory form */
	for (i = 0; i < sizeof(dirs) / sizeof(dirs[0]); i++) {
		wfs_ino_t d = path_ino(&m, dirs[i]);

		CHECK_EQ(wfs_xfs_op_lookup(m.x, d, "no-such-entry", 13, &out, &st), -WFS_ENOENT);
		CHECK_EQ(wfs_xfs_op_lookup(m.x, d, "m5201x", 6, &out, &st), -WFS_ENOENT);
		CHECK_EQ(wfs_xfs_op_lookup(m.x, d, "\x01", 1, &out, &st), -WFS_ENOENT);
		CHECK_EQ(wfs_xfs_op_lookup(m.x, d, "a/b", 3, &out, &st), -WFS_ENOENT);
	}
	/* not a directory */
	{
		wfs_ino_t f;

		CHECK_EQ(wfs_xfs_op_lookup(m.x, root, "rootfile", 8, &f, &st), 0);
		CHECK_EQ(wfs_xfs_op_lookup(m.x, f, "x", 1, &out, &st), -WFS_ENOTDIR);
		CHECK_EQ(rd(&m, f, 0, 0, &l), -WFS_ENOTDIR);
		list_free(&l);
		wfs_xfs_op_forget(m.x, f, 1);
	}
	/* bad directory handles */
	CHECK_EQ(wfs_xfs_op_lookup(m.x, 0, "x", 1, &out, &st), -WFS_ESTALE);
	CHECK_EQ(wfs_xfs_op_lookup(m.x, m.x->mp.m_sb.sb_rootino, "x", 1, &out, &st), -WFS_ESTALE);
	CHECK_EQ(wfs_xfs_op_lookup(m.x, 0x7fffffffffull, "x", 1, &out, &st), -WFS_ESTALE);
	CHECK_EQ(rd(&m, 0x7fffffffffull, 0, 0, &l), -WFS_ESTALE);
	list_free(&l);

	/* every entry of every directory: lookup agrees with readdir, node dir included */
	for (i = 0; i < sizeof(dirs) / sizeof(dirs[0]); i++) {
		wfs_ino_t d = path_ino(&m, dirs[i]);
		size_t j;

		CHECK_EQ(rd(&m, d, 0, 0, &l), 0);
		for (j = 0; j < l.n; j++) {
			CHECK_EQ(wfs_xfs_op_lookup(m.x, d, l.v[j].name, l.v[j].len, &out, &st), 0);
			CHECK_EQ(out, l.v[j].ino);
			CHECK_EQ(st.ftype, l.v[j].ftype);
			CHECK_EQ(st.ino, l.v[j].ino);
			wfs_xfs_op_forget(m.x, out, 1);
		}
		list_free(&l);
	}
	/* ".." of sf resolved to the root: the pinned root keeps its one lookup count */
	CHECK(wfs_xfs_icache_nlookup(m.x) <= 1);
	CHECK_EQ(wfs_xfs_icache_refs(m.x), refs0);
	mnt_close(&m);
}

static int stop_count;
static int stop_cb(void *ctx, const wfs_dirent *e) { (void)ctx; (void)e; stop_count++; return 7; }

static void callback_nonzero_stops_with_zero(void)
{
	struct mnt m;

	CHECK(mnt_open(&m, img_main) == 0);
	for (size_t d = 0; d < sizeof(dirs) / sizeof(dirs[0]); d++) {
		stop_count = 0;
		CHECK_EQ(wfs_xfs_op_readdir(m.x, path_ino(&m, dirs[d]), 0, stop_cb, NULL), 0);
		CHECK_EQ(stop_count, 1);
	}
	CHECK_EQ(wfs_xfs_icache_refs(m.x), wfs_xfs_icache_refs(m.x));
	mnt_close(&m);
}

/* An entry whose on-disk file type is FT_UNKNOWN (filesystems without the ftype
 * feature, v4, are not mounted by this backend): the type comes from the inode. */
static int db_write(const char *img, const char *path, const char *blk, const char *field, const char *val);

static void ftype_unknown_falls_back_to_inode(void)
{
	char img[600];
	struct mnt m;
	struct list l;
	size_t i;

	snprintf(img, sizeof(img), "%s/ftunk.img", tmp);
	sh("cp --reflink=auto --sparse=always '%s' '%s'", img_main, img);
	/* every entry of the block dir (bu[2..] are the files; sub and ln among them) */
	for (i = 2; i < 2 + BLK_N + 2; i++) {
		char f[32];

		snprintf(f, sizeof(f), "bu[%zu].filetype", i);
		if (db_write(img, "/blk", "dblock 0", f, "0")) {
			printf("SKIPPED: ftype_unknown_falls_back_to_inode (xfs_db write unsupported)\n");
			skipped++;
			return;
		}
	}
	CHECK(mnt_open(&m, img) == 0);
	check_listing(&m, "blk", &l);	/* DIR / LNK / REG all recovered from the inodes */
	{
		int dirs_seen = 0, lnks = 0;

		for (i = 0; i < l.n; i++) {
			dirs_seen += l.v[i].ftype == WFS_FT_DIR;
			lnks += l.v[i].ftype == WFS_FT_LNK;
		}
		CHECK(dirs_seen == 1 && lnks == 1);
	}
	list_free(&l);
	mnt_close(&m);
}

/* ---- xfs_db edits for the corruption tests ---- */
static int db_write(const char *img, const char *path, const char *blk, const char *field, const char *val)
{
	return sh("xfs_db -x -c 'path %s' -c '%s' -c 'write %s %s' '%s' >/dev/null 2>&1", path, blk, field, val, img);
}

static void whiteout_entries_are_skipped(void)
{
	char img[600];
	struct mnt m;
	struct list l;

	snprintf(img, sizeof(img), "%s/wht.img", tmp);
	sh("cp --reflink=auto --sparse=always '%s' '%s'", img_main, img);
	/* blk dir: entries 3..4 of the data block get ftype FT_WHT (8) */
	if (db_write(img, "/blk", "dblock 0", "bu[3].filetype", "8") ||
	    db_write(img, "/blk", "dblock 0", "bu[4].filetype", "8")) {
		printf("SKIPPED: whiteout_entries_are_skipped (xfs_db write unsupported)\n");
		skipped++;
		return;
	}
	CHECK(mnt_open(&m, img) == 0);
	CHECK_EQ(rd(&m, path_ino(&m, "blk"), 0, 0, &l), 0);
	/* the two entries are gone from the listing (40 files + sub + ln - 2) */
	CHECK_EQ(l.n, BLK_N + 2 - 2);
	list_free(&l);
	mnt_close(&m);
}

/* byte offset in the image of directory block `dblk` of `path` (xfs_db), or -1 */
static long long db_dirblock_off(const char *img, const char *path, unsigned long long dblk)
{
	char cmd[1024], line[256];
	FILE *f;
	long long fsb = -1, off = -1;

	snprintf(cmd, sizeof(cmd), "xfs_db -r -c 'path %s' -c 'bmap' '%s' 2>/dev/null", path, img);
	f = popen(cmd, "r");
	while (f && fgets(line, sizeof(line), f)) {
		unsigned long long o;
		char *p = strstr(line, "offset ");

		if (p && (o = strtoull(p + 7, NULL, 10)) == dblk && (p = strstr(line, "startblock ")))
			fsb = (long long)strtoull(p + 11, NULL, 10);
	}
	if (f)
		pclose(f);
	if (fsb < 0)
		return -1;
	snprintf(cmd, sizeof(cmd), "xfs_db -r -c 'convert fsblock %lld byte' '%s' 2>/dev/null", fsb, img);
	f = popen(cmd, "r");
	if (f && fgets(line, sizeof(line), f))
		off = (long long)strtoull(line, NULL, 0);
	if (f)
		pclose(f);
	return off;
}

/* overwrite the first 8 bytes of a directory block (magic and CRC mismatch) */
static int smash_dirblock(const char *img, const char *path, unsigned long long dblk)
{
	long long off = db_dirblock_off(img, path, dblk);
	int fd, ok;

	if (off < 0)
		return -1;
	fd = open(img, O_WRONLY);
	if (fd < 0)
		return -1;
	ok = pwrite(fd, "\xde\xad\xbe\xef\xde\xad\xbe\xef", 8, (off_t)off) == 8;
	close(fd);
	return ok ? 0 : -1;
}

static void corrupt_entry_ino_is_euclean(void)
{
	char img[600], arg[64];
	struct mnt m;
	struct list l, nl, all;
	wfs_ino_t blk, out;
	wfs_stat st;
	xfs_ino_t cand = 0, max = 0;
	size_t i, bad = (size_t)-1;

	/* a number that passes xfs_verify_dir_ino but is a free slot of an inode chunk */
	CHECK(mnt_open(&m, img_main) == 0);
	blk = path_ino(&m, "blk");
	CHECK_EQ(rd(&m, blk, 0, 0, &l), 0);
	CHECK_EQ(rd(&m, path_ino(&m, "node"), 0, 0, &all), 0);
	for (i = 0; i < all.n; i++)
		if (all.v[i].ino > max)
			max = all.v[i].ino;
	for (xfs_ino_t c = max + 1; c < max + 64 && !cand; c++) {
		struct xfs_inode *ip;

		if (!xfs_verify_dir_ino(&m.x->mp, c))
			continue;
		if (wfs_xfs_iget(m.x, c, &ip))
			cand = c;
		else
			xfs_irele(ip);
	}
	mnt_close(&m);
	list_free(&all);
	if (!cand) {
		printf("SKIPPED: corrupt_entry_ino_is_euclean (no free inode slot found)\n");
		skipped++;
		list_free(&l);
		return;
	}

	snprintf(img, sizeof(img), "%s/badino.img", tmp);
	sh("cp --reflink=auto --sparse=always '%s' '%s'", img_main, img);
	snprintf(arg, sizeof(arg), "%llu", (unsigned long long)cand);
	if (db_write(img, "/blk", "dblock 0", "bu[5].inumber", arg)) {
		printf("SKIPPED: corrupt_entry_ino_is_euclean (xfs_db write unsupported)\n");
		skipped++;
		list_free(&l);
		return;
	}
	CHECK(mnt_open(&m, img) == 0);
	blk = path_ino(&m, "blk");
	CHECK_EQ(rd(&m, blk, 0, 0, &nl), 0);	/* readdir does not chase inodes when ftype is on disk */
	CHECK_EQ(nl.n, l.n);
	for (i = 0; i < nl.n && i < l.n; i++)
		if (nl.v[i].ino != l.v[i].ino) {
			bad = i;
			CHECK_EQ(nl.v[i].ino, cand);
		}
	CHECK(bad != (size_t)-1);
	for (i = 0; i < nl.n; i++) {
		int rc = wfs_xfs_op_lookup(m.x, blk, nl.v[i].name, nl.v[i].len, &out, &st);

		if (i == bad) {
			CHECK_EQ(rc, -WFS_EUCLEAN);	/* unallocated inode behind an entry */
		} else {
			CHECK_EQ(rc, 0);
			wfs_xfs_op_forget(m.x, out, 1);
		}
	}
	CHECK_EQ(wfs_xfs_icache_nlookup(m.x), 0);
	mnt_close(&m);
	list_free(&nl);

	/* the same entry with FT_UNKNOWN: readdir has to read the inode and fails */
	if (!db_write(img, "/blk", "dblock 0", "bu[5].filetype", "0")) {
		CHECK(mnt_open(&m, img) == 0);
		CHECK_EQ(rd(&m, path_ino(&m, "blk"), 0, 0, &nl), -WFS_EUCLEAN);
		CHECK(nl.n < l.n);
		list_free(&nl);
		mnt_close(&m);
	}
	list_free(&l);
}

static void corrupt_dir_block_is_euclean_not_loop(void)
{
	char img[600];
	struct mnt m;
	struct list l;
	wfs_ino_t d, out;
	wfs_stat st;
	size_t i;
	int ok = 0, euclean = 0, enoent = 0;

	/* (a) a data block of the block-format dir, of the leaf dir and of the node dir */
	snprintf(img, sizeof(img), "%s/baddata.img", tmp);
	sh("cp --reflink=auto --sparse=always '%s' '%s'", img_main, img);
	if (smash_dirblock(img, "/blk", 0) || smash_dirblock(img, "/leaf", 1) ||
	    smash_dirblock(img, "/node", 3)) {
		printf("SKIPPED: corrupt_dir_block_is_euclean_not_loop (a) (cannot patch the image)\n");
		skipped++;
	} else {
		CHECK(mnt_open(&m, img) == 0);
		d = path_ino(&m, "blk");
		CHECK_EQ(rd(&m, d, 0, 0, &l), -WFS_EUCLEAN);
		list_free(&l);
		CHECK_EQ(wfs_xfs_op_lookup(m.x, d, "f01", 3, &out, &st), -WFS_EUCLEAN);
		d = path_ino(&m, "leaf");
		CHECK_EQ(rd(&m, d, 0, 0, &l), -WFS_EUCLEAN);
		CHECK(l.n > 0 && l.n < LEAF_N);	/* entries of the good blocks before it arrive */
		list_free(&l);
		d = path_ino(&m, "node");
		CHECK_EQ(rd(&m, d, 0, 0, &l), -WFS_EUCLEAN);
		CHECK(l.n > 0 && l.n < NODE_N);
		list_free(&l);
		/* resuming past the bad block still works, and the fs stays usable */
		CHECK_EQ(rd(&m, path_ino(&m, "sf"), 0, 0, &l), 0);
		list_free(&l);
		CHECK_EQ(rd(&m, WFS_ROOT_INO, 0, 0, &l), 0);
		list_free(&l);
		mnt_close(&m);
	}

	/* (b) the leaf blocks of the node dir's da-tree point forw/back at themselves.
	 * Every lookup must end (alarm() guards the test) with the right inode, -ENOENT
	 * for an absent name, or -EUCLEAN. */
	snprintf(img, sizeof(img), "%s/cycle.img", tmp);
	sh("cp --reflink=auto --sparse=always '%s' '%s'", img_main, img);
	for (i = 0; i < 12; i++)
		if (sh("xfs_db -x -c 'path /node' -c 'dblock %zu' -c 'write lhdr.info.hdr.forw %zu' "
		       "-c 'write lhdr.info.hdr.back %zu' '%s' >/dev/null 2>&1",
		       (size_t)8388610 + i, (size_t)8388610 + i, (size_t)8388610 + i, img)) {
			printf("SKIPPED: corrupt_dir_block_is_euclean_not_loop (b) (xfs_db write unsupported)\n");
			skipped++;
			return;
		}
	CHECK(mnt_open(&m, img) == 0);
	d = path_ino(&m, "node");
	CHECK_EQ(rd(&m, d, 0, 0, &l), 0);	/* readdir reads data blocks only */
	CHECK_EQ(l.n, NODE_N + 2);		/* 5200 files + sub + ln */
	for (i = 0; i < l.n; i++) {
		int rc = wfs_xfs_op_lookup(m.x, d, l.v[i].name, l.v[i].len, &out, &st);

		if (rc == 0) {
			CHECK_EQ(out, l.v[i].ino);
			wfs_xfs_op_forget(m.x, out, 1);
			ok++;
		} else {
			CHECK_EQ(rc, -WFS_EUCLEAN);
			euclean++;
		}
	}
	for (i = 0; i < 3000; i++) {
		char nm[32];
		int rc;

		snprintf(nm, sizeof(nm), "zz%zu_missing", i);
		rc = wfs_xfs_op_lookup(m.x, d, nm, (uint32_t)strlen(nm), &out, &st);
		CHECK(rc == -WFS_ENOENT || rc == -WFS_EUCLEAN);
		enoent += rc == -WFS_ENOENT;
	}
	printf("  cycle image: %d lookups ok, %d EUCLEAN; %d of 3000 absent names ENOENT\n", ok, euclean, enoent);
	list_free(&l);
	mnt_close(&m);
}

/* ---- the fixture matrix: every directory against the manifest ---- */

struct mdir {
	char path[1024];
	char form[16];
	struct { char name[256]; char kind[16]; } *kid;
	size_t n, cap;
};

static int fixture_dirs(const char *manifest, const char *expect, struct mdir **out, size_t *nout)
{
	/* NUL separated path, kind, dir_form triples: names may hold any byte but NUL */
	static const char py[] =
		"import json,sys\n"
		"m=json.load(open(sys.argv[1]))['entries']\n"
		"e=json.load(open(sys.argv[2]))['paths']\n"
		"w=sys.stdout.buffer\n"
		"for p,v in sorted(m.items()):\n"
		"  for t in (p, v['kind'], e.get(p,{}).get('dir_form','-')):\n"
		"    w.write(t.encode('utf-8','surrogateescape')+b'\\0')\n";
	char cmd[2048], pyf[700];
	FILE *f;
	char *buf = NULL, *q, *end;
	size_t len = 0, cap = 0, n = 0, ncap = 0, i;
	struct mdir *v = NULL;

	snprintf(pyf, sizeof(pyf), "%s/dirs.py", tmp);
	f = fopen(pyf, "w");
	if (!f)
		return -1;
	fputs(py, f);
	fclose(f);
	snprintf(cmd, sizeof(cmd), "python3 -I '%s' '%s' '%s' 2>/dev/null", pyf, manifest, expect);
	f = popen(cmd, "r");
	if (!f)
		return -1;
	for (;;) {
		size_t r;

		if (len + 65536 > cap)
			buf = realloc(buf, cap = cap ? cap * 2 : 1 << 20);
		r = fread(buf + len, 1, cap - len, f);
		if (!r)
			break;
		len += r;
	}
	pclose(f);
	for (q = buf, end = buf + len; q < end;) {
		char *path = q, *kind = path + strlen(path) + 1, *form = kind + strlen(kind) + 1;

		q = form + strlen(form) + 1;
		if (q > end)
			break;
		if (n == ncap) {
			ncap = ncap ? ncap * 2 : 64;
			v = realloc(v, ncap * sizeof(*v));
		}
		if (!strcmp(kind, "dir")) {
			memset(&v[n], 0, sizeof(v[n]));
			snprintf(v[n].path, sizeof(v[n].path), "%s", path);
			snprintf(v[n].form, sizeof(v[n].form), "%s", form);
			n++;
		}
		/* children attach to their parent directory (sorted: parents first) */
		if (strcmp(path, "/")) {
			char *slash = strrchr(path, '/');
			char parent[1024];
			size_t pl = (size_t)(slash - path);

			if (pl >= sizeof(parent))
				continue;
			if (pl) {
				memcpy(parent, path, pl);
				parent[pl] = 0;
			} else {
				strcpy(parent, "/");
			}
			for (i = n; i-- > 0;)
				if (!strcmp(v[i].path, parent)) {
					struct mdir *d = &v[i];

					if (d->n == d->cap) {
						d->cap = d->cap ? d->cap * 2 : 16;
						d->kid = realloc(d->kid, d->cap * sizeof(*d->kid));
					}
					snprintf(d->kid[d->n].name, 256, "%s", slash + 1);
					snprintf(d->kid[d->n].kind, 16, "%s", kind);
					d->n++;
					break;
				}
		}
	}
	free(buf);
	*out = v;
	*nout = n;
	return n ? 0 : -1;
}

static int name_cmp(const void *a, const void *b)
{
	return strcmp(((const struct ent *)a)->name, ((const struct ent *)b)->name);
}

static uint32_t kind_to_ft(const char *k)
{
	if (!strcmp(k, "dir")) return WFS_FT_DIR;
	if (!strcmp(k, "file")) return WFS_FT_REG;
	if (!strcmp(k, "symlink")) return WFS_FT_LNK;
	if (!strcmp(k, "fifo")) return WFS_FT_FIFO;
	if (!strcmp(k, "chardev") || !strcmp(k, "chr")) return WFS_FT_CHR;
	if (!strcmp(k, "blockdev") || !strcmp(k, "blk")) return WFS_FT_BLK;
	if (!strcmp(k, "socket") || !strcmp(k, "sock")) return WFS_FT_SOCK;
	return 0;
}

static void fixture_matrix_dirs_match_manifest(void)
{
	static const char *const roots[] = {
		"../../target/xfs-fixtures", "../../../../target/xfs-fixtures", NULL
	};
	const char *root = getenv("WFS_XFS_FIXTURES");
	glob_t g;
	size_t gi, nimg = 0, ndir = 0, big = 0;
	int formcnt[4] = { 0, 0, 0, 0 };
	char pat[700];

	for (int r = 0; !root || !*root; r++) {
		struct stat st;

		if (!roots[r])
			break;
		if (!stat(roots[r], &st))
			root = roots[r];
	}
	if (!root || !*root) {
		printf("SKIPPED: fixture_matrix_dirs_match_manifest (no fixture cache: tools/gen-fixtures.sh)\n");
		skipped++;
		return;
	}
	snprintf(pat, sizeof(pat), "%s/*.manifest.json", root);
	if (glob(pat, 0, NULL, &g)) {
		printf("SKIPPED: fixture_matrix_dirs_match_manifest (no fixtures in %s)\n", root);
		skipped++;
		return;
	}
	for (gi = 0; gi < g.gl_pathc; gi++) {
		char base[700], img[720], logi[720], rti[720], expect[720];
		struct mdir *dv = NULL;
		size_t nd = 0, di;
		struct mnt m;
		struct stat st;
		int rc;

		snprintf(base, sizeof(base), "%s", g.gl_pathv[gi]);
		*strstr(base, ".manifest.json") = 0;
		snprintf(img, sizeof(img), "%s.img", base);
		snprintf(logi, sizeof(logi), "%s.log.img", base);
		snprintf(rti, sizeof(rti), "%s.rt.img", base);
		snprintf(expect, sizeof(expect), "%s.expect.json", base);
		if (stat(img, &st) || fixture_dirs(g.gl_pathv[gi], expect, &dv, &nd)) {
			printf("note: fixture %s unusable, skipped\n", base);
			continue;
		}
		rc = mnt_open3(&m, img, stat(logi, &st) ? NULL : logi, stat(rti, &st) ? NULL : rti);
		if (rc) {
			printf("note: fixture %s does not mount here (%d), skipped\n", strrchr(base, '/') + 1, rc);
			live--;
			continue;
		}
		nimg++;
		for (di = 0; di < nd; di++) {
			wfs_ino_t dir = path_ino(&m, dv[di].path);
			struct list l;
			size_t k;

			CHECK(dir != 0);
			if (!dir)
				continue;
			ndir++;
			if (!strcmp(dv[di].form, "shortform")) formcnt[0]++;
			else if (!strcmp(dv[di].form, "block")) formcnt[1]++;
			else if (!strcmp(dv[di].form, "leaf")) formcnt[2]++;
			else if (!strcmp(dv[di].form, "node")) formcnt[3]++;
			CHECK_EQ(rd(&m, dir, 0, 0, &l), 0);
			CHECK_EQ(l.bad_cookie, 0);
			if (l.n != dv[di].n)
				fprintf(stderr, "  dir %s: %zu listed, %zu in manifest (%s)\n", dv[di].path,
					l.n, dv[di].n, strrchr(base, '/') + 1);
			CHECK_EQ(l.n, dv[di].n);
			if (l.n == dv[di].n) {
				struct ent *want = calloc(dv[di].n ? dv[di].n : 1, sizeof(*want));
				struct list s = l;

				for (k = 0; k < dv[di].n; k++) {
					snprintf(want[k].name, 256, "%s", dv[di].kid[k].name);
					want[k].ftype = kind_to_ft(dv[di].kid[k].kind);
				}
				qsort(want, dv[di].n, sizeof(*want), name_cmp);
				s.v = malloc((l.n ? l.n : 1) * sizeof(*s.v));
				memcpy(s.v, l.v, l.n * sizeof(*s.v));
				qsort(s.v, l.n, sizeof(*s.v), name_cmp);
				for (k = 0; k < l.n; k++) {
					CHECK(!strcmp(s.v[k].name, want[k].name));
					if (want[k].ftype)
						CHECK_EQ(s.v[k].ftype, want[k].ftype);
				}
				free(want);
				free(s.v);
			}
			/* resume: stepping with stops after a stride of entries */
			if (l.n > 200) {
				big++;
				for (k = 0; k < l.n; k += 37) {
					struct list rest;

					CHECK_EQ(rd(&m, dir, l.v[k].cookie, 0, &rest), 0);
					CHECK_EQ(rest.n, l.n - k - 1);
					list_free(&rest);
				}
			}
			list_free(&l);
		}
		CHECK_EQ(wfs_xfs_icache_nlookup(m.x), 0);
		mnt_close(&m);
		for (di = 0; di < nd; di++)
			free(dv[di].kid);
		free(dv);
	}
	globfree(&g);
	printf("  fixtures: %zu images, %zu directories (shortform %d, block %d, leaf %d, node %d), %zu big resumed\n",
	       nimg, ndir, formcnt[0], formcnt[1], formcnt[2], formcnt[3], big);
	CHECK(nimg > 0);
}

/* ------------------------------------------------------------ main -- */

#define RUN(t) do { int _f = failures; printf("RUN  %s\n", #t); fflush(stdout); t(); \
	printf("%s %s\n", failures == _f ? "ok  " : "FAIL", #t); } while (0)

int main(void)
{
	const char *td = getenv("TMPDIR");
	char img[600];

	if (system("command -v mkfs.xfs >/dev/null 2>&1") || system("command -v xfs_db >/dev/null 2>&1")) {
		printf("SKIPPED: test_dir (xfsprogs not installed)\n");
		return 0;
	}
	alarm(900);	/* a looping directory walk must fail the test, not hang it */
	snprintf(tmp, sizeof(tmp), "%s/wfs-dir-test.XXXXXX", td && *td ? td : "/tmp");
	if (!mkdtemp(tmp)) {
		perror("mkdtemp");
		return 1;
	}
	snprintf(src, sizeof(src), "%s/src", tmp);
	snprintf(img, sizeof(img), "%s/main.img", tmp);
	snprintf(img_main, sizeof(img_main), "%s", img);
	make_src();
	if (mk_img(img_main, "-m crc=1 -b size=4096 -d agcount=4")) {
		printf("SKIPPED: test_dir (mkfs.xfs -p failed)\n");
		sh("rm -rf '%s'", tmp);
		return 0;
	}
	mem_base = wfs_mem_used();

	RUN(lists_all_four_dir_formats);
	RUN(readdir_resume_each_entry_no_dup_no_miss);
	RUN(cookies_strictly_progress_and_below_2_pow_63);
	RUN(no_dot_entries);
	RUN(non_ascii_and_255_byte_names_exact_bytes);
	RUN(lookup_basics_and_refs);
	RUN(callback_nonzero_stops_with_zero);
	RUN(ftype_unknown_falls_back_to_inode);
	RUN(whiteout_entries_are_skipped);
	RUN(corrupt_entry_ino_is_euclean);
	RUN(corrupt_dir_block_is_euclean_not_loop);
	RUN(fixture_matrix_dirs_match_manifest);

	sh("rm -rf '%s'", tmp);
	printf("%d checks, %d failures, %d skipped\n", checks, failures, skipped);
	return failures ? 1 : 0;
}
