// SPDX-License-Identifier: GPL-2.0-only
/*
 * Task 8 tests: inode number mapping, getattr against xfs_db, cache limit,
 * lookup/forget reference balance, corrupt inodes are not cached.
 * Images are made by mkfs.xfs -p (protofile) and read through a read-only
 * file-backed host; ground truth is xfs_db. Skips visibly without the tools.
 */
#include "wfs_xfs.h"
#include "icache.h"
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

/* _GNU_SOURCE cannot be set: it clashes with the shim's kernel typedefs */
char *strptime(const char *, const char *, struct tm *);
time_t timegm(struct tm *);

static int failures, checks, skipped;
#define CHECK(c) do { checks++; if (!(c)) { failures++; \
	fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #c); } } while (0)
#define CHECK_EQ(a, b) do { long long _a = (a), _b = (b); checks++; if (_a != _b) { failures++; \
	fprintf(stderr, "%s:%d: %s = %lld, want %lld\n", __FILE__, __LINE__, #a, _a, _b); } } while (0)

struct wfs_blkdev { int fd; uint64_t size; };
static void h_log(int level, const char *msg) { if (getenv("IC_VERBOSE")) fprintf(stderr, "[%d] %s\n", level, msg); }
static uint64_t h_size(wfs_blkdev *d) { return d->size; }
static uint32_t h_ss(wfs_blkdev *d) { (void)d; return 512; }
static int h_ro(wfs_blkdev *d) { (void)d; return 1; }
static wfs_err_t h_read(wfs_blkdev *d, uint64_t off, void *buf, size_t len)
{
	if (off > d->size || len > d->size - off)
		return -WFS_EIO;
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
static int live;	/* mounts open; the leak check runs at the last close */

static int sh(const char *fmt, ...)
{
	char cmd[2048];
	va_list ap;

	va_start(ap, fmt);
	vsnprintf(cmd, sizeof(cmd), fmt, ap);
	va_end(ap);
	return system(cmd);
}

struct mnt { wfs_blkdev dev; wfs_blkdev *p[WFS_DEV_COUNT]; struct wfs_xfs *x; };

static int mnt_open(struct mnt *m, const char *img, const char *opts)
{
	struct stat st;

	memset(m, 0, sizeof(*m));
	m->dev.fd = open(img, O_RDONLY);
	if (m->dev.fd < 0 || fstat(m->dev.fd, &st))
		return 12345;
	m->dev.size = (uint64_t)st.st_size;
	m->p[0] = &m->dev;
	live++;
	return wfs_xfs_mount(&api, m->p, opts, &m->x);
}
static void mnt_close(struct mnt *m)
{
	CHECK(wfs_xfs_unmount(m->x) == 0);
	close(m->dev.fd);
	if (--live == 0)
		CHECK(wfs_mem_used() == mem_base);	/* no leaks, incl. the global caches */
}

/* ---- ground truth ---- */
static int db_get(const char *img, const char *path, const char *field, char *out, size_t n)
{
	char cmd[1024], line[512];
	FILE *f;
	int ok = -1;

	snprintf(cmd, sizeof(cmd), "TZ=UTC xfs_db -r -c 'path %s' -c 'print %s' '%s' 2>/dev/null",
		 path, field, img);
	f = popen(cmd, "r");
	while (f && fgets(line, sizeof(line), f)) {
		char *eq = strstr(line, " = ");

		if (eq && !strncmp(line, field, strlen(field))) {
			snprintf(out, n, "%s", eq + 3);
			out[strcspn(out, "\n")] = 0;
			ok = 0;
		}
	}
	if (f)
		pclose(f);
	return ok;
}
static long long db_num(const char *img, const char *path, const char *field)
{
	char v[128];

	return db_get(img, path, field, v, sizeof(v)) ? -1 : strtoll(v, NULL, 0);
}
static long long db_time(const char *img, const char *path, const char *field)
{
	char v[128];
	struct tm tm;

	if (db_get(img, path, field, v, sizeof(v)))
		return -1;
	memset(&tm, 0, sizeof(tm));
	if (!strptime(v, "%a %b %d %H:%M:%S %Y", &tm))
		return -1;
	return (long long)timegm(&tm);
}
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

/* image with a small tree; N extra files in /many when many > 0 */
static char img_main[600], img_many[600];
static int mk_proto(const char *img, int many)
{
	char proto[600], src[600];
	FILE *f;
	int i;

	snprintf(proto, sizeof(proto), "%s/proto.%d", tmp, many);
	snprintf(src, sizeof(src), "%s/src.txt", tmp);
	f = fopen(src, "w");
	if (!f)
		return -1;
	fputs("hello world data\n", f);
	fclose(f);
	f = fopen(proto, "w");
	if (!f)
		return -1;
	fprintf(f, "/nonexistent\n0 0\nd--755 1000 1001\n"
		   "f1 ---4644 1234 5678 %s\n"
		   "sub d--750 3 4\n inner ---600 5 6 %s\n $\n"
		   "sl l--777 7 8 %s\n"
		   "cdev c--666 0 0 12 34\nbdev b--660 0 0 8 1\nfifo p--644 0 0\n", src, src, src);
	if (many) {
		fprintf(f, "many d--755 0 0\n");
		for (i = 0; i < many; i++)
			fprintf(f, " m%04d ---644 %d %d %s\n", i, i, i, src);
		fprintf(f, " $\n");
	}
	fprintf(f, "$\n");
	fclose(f);
	return sh("rm -f '%s' && truncate -s 300m '%s' && mkfs.xfs -q -f -m crc=1 -b size=4096 -d agcount=4 -p '%s' '%s' >/dev/null 2>&1",
		  img, img, proto, img);
}

/* ------------------------------------------------------------ tests -- */

static void ino_mapping_roundtrip_and_no_collision(void)
{
	struct mnt m;
	xfs_ino_t x = 0, root;
	struct xfs_inode *ip;
	uint64_t i;

	CHECK(mnt_open(&m, img_main, NULL) == 0);
	root = m.x->mp.m_sb.sb_rootino;
	CHECK(root != WFS_ROOT_INO);
	CHECK(wfs_xfs_ino_to_wfs(&m.x->mp, root) == WFS_ROOT_INO);
	CHECK(wfs_xfs_ino_from_wfs(&m.x->mp, WFS_ROOT_INO, &x) == 0 && x == root);
	CHECK_EQ(wfs_xfs_ino_from_wfs(&m.x->mp, root, &x), -WFS_ESTALE);	/* never handed out */
	CHECK_EQ(wfs_xfs_ino_from_wfs(&m.x->mp, 0, &x), -WFS_ESTALE);
	CHECK_EQ(wfs_xfs_ino_from_wfs(&m.x->mp, XFS_MAXINUMBER + 1, &x), -WFS_ESTALE);
	for (i = 2; i < 2000; i++) {
		if (i == root)
			continue;
		CHECK(wfs_xfs_ino_from_wfs(&m.x->mp, i, &x) == 0 && x == i);
		CHECK(wfs_xfs_ino_to_wfs(&m.x->mp, x) == i);
	}
	/* ino 1 is not a real XFS inode, so mapping it to the root loses nothing */
	CHECK_EQ(wfs_xfs_iget(m.x, 1, &ip), -EINVAL);
	CHECK(wfs_xfs_icache_refs(m.x) == 0);
	mnt_close(&m);
}

static void check_path(struct mnt *m, const char *img, const char *path, uint32_t ftype)
{
	wfs_stat st;
	uint64_t xi = strcmp(path, "/") ? db_ino(img, path) : m->x->mp.m_sb.sb_rootino;
	wfs_ino_t w = wfs_xfs_ino_to_wfs(&m->x->mp, xi);
	char v[64];

	CHECK(xi != 0);
	CHECK_EQ(wfs_xfs_op_getattr(m->x, w, &st), 0);
	CHECK_EQ(st.ino, w);
	CHECK_EQ(st.ftype, ftype);
	CHECK_EQ(st.mode, db_num(img, path, "core.mode") & 07777);
	CHECK_EQ(st.uid, db_num(img, path, "core.uid"));
	CHECK_EQ(st.gid, db_num(img, path, "core.gid"));
	CHECK_EQ(st.nlink, db_num(img, path, "core.nlinkv2"));
	CHECK_EQ(st.size, db_num(img, path, "core.size"));
	CHECK_EQ(st.blocks, db_num(img, path, "core.nblocks") * 8);	/* 4096 B blocks */
	CHECK_EQ(st.atime.sec, db_time(img, path, "core.atime.sec"));
	CHECK_EQ(st.mtime.sec, db_time(img, path, "core.mtime.sec"));
	CHECK_EQ(st.ctime.sec, db_time(img, path, "core.ctime.sec"));
	CHECK_EQ(st.btime.sec, db_time(img, path, "v3.crtime.sec"));
	CHECK_EQ(st.mtime.nsec, db_num(img, path, "core.mtime.nsec"));
	CHECK_EQ(st.btime.nsec, db_num(img, path, "v3.crtime.nsec"));
	if (ftype == WFS_FT_CHR || ftype == WFS_FT_BLK) {
		long long dev;

		if (db_get(img, path, "u3.dev", v, sizeof(v)))
			dev = -1;
		else
			dev = strtoll(v, NULL, 0);
		CHECK_EQ(st.rdev_major, (dev >> 18) & 0x1ff);
		CHECK_EQ(st.rdev_minor, dev & 0x3ffff);
	} else {
		CHECK(st.rdev_major == 0 && st.rdev_minor == 0);
	}
}

static void getattr_matches_xfs_db_for_fixture_paths(void)
{
	struct mnt m;
	wfs_stat st;

	CHECK(mnt_open(&m, img_main, NULL) == 0);
	check_path(&m, img_main, "/", WFS_FT_DIR);
	check_path(&m, img_main, "/f1", WFS_FT_REG);
	check_path(&m, img_main, "/sub", WFS_FT_DIR);
	check_path(&m, img_main, "/sub/inner", WFS_FT_REG);
	check_path(&m, img_main, "/sl", WFS_FT_LNK);
	check_path(&m, img_main, "/cdev", WFS_FT_CHR);
	check_path(&m, img_main, "/bdev", WFS_FT_BLK);
	check_path(&m, img_main, "/fifo", WFS_FT_FIFO);
	CHECK_EQ(wfs_xfs_op_getattr(m.x, WFS_ROOT_INO, &st), 0);
	CHECK_EQ(st.ino, WFS_ROOT_INO);
	/* stale handles */
	CHECK_EQ(wfs_xfs_op_getattr(m.x, 0, &st), -WFS_ESTALE);
	CHECK_EQ(wfs_xfs_op_getattr(m.x, m.x->mp.m_sb.sb_rootino, &st), -WFS_ESTALE);
	CHECK_EQ(wfs_xfs_op_getattr(m.x, (uint64_t)1 << 40, &st), -WFS_ESTALE);	/* AG out of range */
	CHECK_EQ(wfs_xfs_op_getattr(m.x, 5, &st), -WFS_ESTALE);	/* not an inode */
	/* an unallocated slot after the last inode of the first chunk */
	CHECK_EQ(wfs_xfs_op_getattr(m.x, m.x->mp.m_sb.sb_rootino + 60, &st), -WFS_ESTALE);
	CHECK(wfs_xfs_icache_refs(m.x) == 0);
	mnt_close(&m);
}

static void cache_limit_64_inodes_still_correct(void)
{
	struct mnt m, big;
	wfs_stat a, b;
	uint64_t i, ok = 0, base;

	CHECK(mnt_open(&m, img_many, "inodecache_entries=64") == 0);
	CHECK(mnt_open(&big, img_many, NULL) == 0);
	base = db_ino(img_many, "/many");
	CHECK(base != 0);
	for (i = base; i < base + 6000; i++) {
		int ra = wfs_xfs_op_getattr(m.x, i, &a);
		int rb = wfs_xfs_op_getattr(big.x, i, &b);

		CHECK_EQ(ra, rb);
		if (!ra) {
			ok++;
			CHECK(!memcmp(&a, &b, sizeof(a)));
		}
		CHECK(wfs_xfs_icache_count(m.x) <= 64);
	}
	if (ok < 300) fprintf(stderr, "ok=%llu base=%llu\n", (unsigned long long)ok, (unsigned long long)base);
	CHECK(ok >= 300);
	CHECK(wfs_xfs_icache_count(big.x) > 64);
	CHECK(wfs_xfs_icache_refs(m.x) == 0);
	/* spot check against xfs_db */
	CHECK_EQ(wfs_xfs_op_getattr(m.x, db_ino(img_many, "/many/m0123"), &a), 0);
	CHECK_EQ(a.uid, 123);
	CHECK_EQ(a.size, 17);
	mnt_close(&big);
	mnt_close(&m);
}

static void forget_balances_refs(void)
{
	struct mnt m;
	struct xfs_inode *ip, *other;
	uint64_t a = db_ino(img_many, "/many/m0001"), i, base = db_ino(img_many, "/many");

	CHECK(mnt_open(&m, img_many, "inodecache_entries=64") == 0);
	CHECK_EQ(wfs_xfs_iget(m.x, a, &ip), 0);
	CHECK(wfs_xfs_icache_refs(m.x) == 1);
	wfs_xfs_ilookup_ref(ip);
	wfs_xfs_ilookup_ref(ip);
	xfs_irele(ip);
	CHECK(wfs_xfs_icache_refs(m.x) == 0);
	CHECK(wfs_xfs_icache_nlookup(m.x) == 2);
	/* churn far beyond the limit: the looked-up inode must survive */
	for (i = base + 1; i < base + 600; i++)
		if (wfs_xfs_iget(m.x, i, &other) == 0)
			xfs_irele(other);
	CHECK(wfs_xfs_icache_nlookup(m.x) == 2);
	wfs_xfs_iforget(m.x, a, 1);
	CHECK(wfs_xfs_icache_nlookup(m.x) == 1);
	wfs_xfs_iforget(m.x, a, 5);		/* over-forget saturates, no underflow */
	CHECK(wfs_xfs_icache_nlookup(m.x) == 0);
	wfs_xfs_iforget(m.x, a + 100000, 3);	/* unknown inode ignored */
	wfs_xfs_op_forget(m.x, WFS_ROOT_INO, 1);	/* root is advisory */
	wfs_xfs_op_forget(m.x, 0, 1);
	CHECK(wfs_xfs_icache_count(m.x) <= 64);
	CHECK(m.x->mp.m_rootip != NULL);
	mnt_close(&m);
}

static void corrupt_inode_not_cached(void)
{
	struct mnt m;
	char bad[600];
	uint64_t last;
	wfs_stat st;
	struct xfs_inode *ip;
	size_t before;

	snprintf(bad, sizeof(bad), "%s/bad.img", tmp);
	sh("cp --sparse=always '%s' '%s'", img_many, bad);
	last = db_ino(bad, "/many/m0299");
	/* write -d bypasses the verifier and keeps a good CRC: a logical error (bad magic) */
	CHECK_EQ(sh("xfs_db -x -c 'inode %llu' -c 'write -d core.magic 0' '%s' >/dev/null 2>&1",
		    (unsigned long long)last, bad), 0);
	CHECK(mnt_open(&m, bad, NULL) == 0);
	before = wfs_xfs_icache_count(m.x);
	CHECK_EQ(wfs_xfs_op_getattr(m.x, last, &st), -WFS_EUCLEAN);
	CHECK_EQ(wfs_xfs_op_getattr(m.x, last, &st), -WFS_EUCLEAN);	/* re-read, not cached */
	CHECK_EQ(wfs_xfs_iget(m.x, last, &ip), -EFSCORRUPTED);
	CHECK(wfs_xfs_icache_count(m.x) == before);
	CHECK(wfs_xfs_icache_refs(m.x) == 0);
	/* the root and the other clusters stay readable */
	CHECK_EQ(wfs_xfs_op_getattr(m.x, WFS_ROOT_INO, &st), 0);
	mnt_close(&m);
}

int main(void)
{
	const char *td = getenv("TMPDIR");

	if (sh("command -v mkfs.xfs >/dev/null 2>&1") || sh("command -v xfs_db >/dev/null 2>&1")) {
		printf("SKIPPED test_icache: mkfs.xfs/xfs_db not installed\n");
		return 0;
	}
	snprintf(tmp, sizeof(tmp), "%s/wfs-icache-test.XXXXXX", td && *td ? td : "/tmp");
	if (!mkdtemp(tmp)) {
		perror("mkdtemp");
		return 2;
	}
	snprintf(img_main, sizeof(img_main), "%s/main.img", tmp);
	snprintf(img_many, sizeof(img_many), "%s/many.img", tmp);
	if (mk_proto(img_main, 0) || mk_proto(img_many, 300)) {
		fprintf(stderr, "cannot create the test images\n");
		return 2;
	}
	mem_base = wfs_mem_used();
#define RUN(t) do { int f0 = failures; t(); printf("%s %s\n", failures == f0 ? "PASS" : "FAIL", #t); } while (0)
	RUN(ino_mapping_roundtrip_and_no_collision);
	RUN(getattr_matches_xfs_db_for_fixture_paths);
	RUN(cache_limit_64_inodes_still_correct);
	RUN(forget_balances_refs);
	RUN(corrupt_inode_not_cached);
	CHECK(nwrites == 0);
	sh("rm -rf '%s'", tmp);
	printf("test_icache: %d checks, %d failures, %d skipped\n", checks, failures, skipped);
	return failures ? 1 : 0;
}
