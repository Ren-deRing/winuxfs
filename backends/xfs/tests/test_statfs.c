// SPDX-License-Identifier: GPL-2.0-only
/*
 * Task 11 tests: statfs and the mount-time summary counters.
 * usage: test_statfs FIXTURE_DIR ROOT_FIXTURE_DIR
 *
 * Ground truth is xfs_db on the image, never the backend's own numbers:
 *   sb 0:  blocksize dblocks logstart logblocks icount ifree fdblocks agcount
 *          imax_pct inopblock inopblog inodesize features_ro_compat
 *   agf N: freeblks flcount btreeblks rmapblocks      agi N: count freecount
 * Expected statfs (kernel xfs_fs_statfs, M2 design 8.7; read-only mount, so
 * no reserved pool):
 *   unavail = agcount * (4 + 4) + sum over AGs of max(0, btreeblks -
 *             (rmapbt ? rmapblocks - 1 : 0))
 *   total   = dblocks - (logstart ? logblocks : 0)
 *   free    = max(0, fdblocks - unavail)            avail = free
 *   maxino  = (dblocks * imax_pct / 100 / ialloc_blks) * ialloc_blks << inopblog
 *             (0 when imax_pct is 0), ialloc_blks = max(64, inopblock) >> inopblog
 *   files   = max(icount, min(icount + (free << inopblog), maxino|inf, 2^56-1))
 *   ffree   = files - (icount - ifree)
 * Counter policy: clean log + sane sb counters -> trusted; norecovery on a
 * dirty log or insane counters -> AGF/AGI sums. Skips visibly without tools.
 */
#include "wfs_xfs.h"
#ifndef offsetofend
#define offsetofend(T, m) (offsetof(T, m) + sizeof(((T *)0)->m))
#endif
#include "xfs_bit.h"
#include "xfs_cksum.h"
#include "xfs_sb.h"
#include "xfs_ag.h"
#include "xfs_alloc.h"
#include "ops.h"
#include <dirent.h>
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
#define CHECK_EQ(a, b) do { unsigned long long _a = (a), _b = (b); checks++; if (_a != _b) { failures++; \
	fprintf(stderr, "%s:%d: %s = %llu, want %llu [%s]\n", __FILE__, __LINE__, #a, _a, _b, cur); } } while (0)

static const char *cur = "";	/* image under test, for failure messages */

/* ---------------------------------------------------------------- host -- */
struct wfs_blkdev { int fd; uint64_t size; };
static long nwrites;
static void h_log(int level, const char *msg) { if (getenv("ST_VERBOSE")) fprintf(stderr, "[%d] %s\n", level, msg); }
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
{ (void)d; (void)o; (void)b; (void)l; nwrites++; return -WFS_EROFS; }
static wfs_err_t h_flush(wfs_blkdev *d) { (void)d; nwrites++; return 0; }
static const wfs_host_api api = {
	.abi_version = WFS_ABI_VERSION, .size = sizeof(wfs_host_api),
	.blk_size = h_size, .blk_sector_size = h_ss, .blk_read_only = h_ro,
	.blk_read = h_read, .blk_write = h_write, .blk_flush = h_flush, .log = h_log,
};

static char tmp[512];
static size_t mem_base;

struct mnt { wfs_blkdev dev[WFS_DEV_COUNT]; wfs_blkdev *p[WFS_DEV_COUNT]; struct wfs_xfs *x; };

static int dev_open(wfs_blkdev *d, const char *path)
{
	struct stat st;

	d->fd = open(path, O_RDONLY);
	if (d->fd < 0 || fstat(d->fd, &st))
		return -1;
	d->size = (uint64_t)st.st_size;
	return 0;
}
static void mnt_close_devs(struct mnt *m)
{
	for (int i = 0; i < WFS_DEV_COUNT; i++)
		if (m->p[i])
			close(m->dev[i].fd);
}
static int mnt_open(struct mnt *m, const char *data, const char *logd, const char *rtd,
		    const char *opts)
{
	const char *path[WFS_DEV_COUNT] = { data, logd, rtd };
	int rc;

	memset(m, 0, sizeof(*m));
	for (int i = 0; i < WFS_DEV_COUNT; i++) {
		if (!path[i])
			continue;
		if (dev_open(&m->dev[i], path[i]))
			return 12345;
		m->p[i] = &m->dev[i];
	}
	rc = wfs_xfs_mount(&api, m->p, opts, &m->x);
	if (rc)
		mnt_close_devs(m);
	return rc;
}
static void mnt_unmount(struct mnt *m)
{
	CHECK(wfs_xfs_unmount(m->x) == 0);
	mnt_close_devs(m);
	CHECK(wfs_mem_used() == mem_base);
}

static int sh(const char *fmt, ...)
{
	char cmd[2048];
	va_list ap;

	va_start(ap, fmt);
	vsnprintf(cmd, sizeof(cmd), fmt, ap);
	va_end(ap);
	return system(cmd);
}

/* ---------------------------------------------------------- ground truth -- */
static unsigned long long db_val(const char *img, const char *sec, const char *field)
{
	char cmd[1024], line[256];
	FILE *f;
	unsigned long long v = ~0ull;
	size_t fl = strlen(field);

	snprintf(cmd, sizeof(cmd), "xfs_db -r -c '%s' -c 'print %s' '%s' 2>/dev/null", sec, field, img);
	f = popen(cmd, "r");
	while (f && fgets(line, sizeof(line), f)) {
		if (!strncmp(line, field, fl) && !strncmp(line + fl, " = ", 3))
			v = strtoull(line + fl + 3, NULL, 0);
	}
	if (f)
		pclose(f);
	return v;
}

struct truth {
	unsigned long long blocksize, dblocks, logstart, logblocks, icount, ifree, fdblocks;
	unsigned long long agcount, imax_pct, inopblock, inopblog;
	unsigned long long allocbt, agf_sum_fd, agi_icount, agi_ifree;
	unsigned long long total, free, files, ffree;	/* expected statfs from the sb counters */
	unsigned long long s_total, s_free, s_files, s_ffree;	/* same from the AG sums */
};

static void truth_expect(const struct truth *t, unsigned long long icount, unsigned long long ifree,
			 unsigned long long fdblocks, unsigned long long *total,
			 unsigned long long *free_, unsigned long long *files, unsigned long long *ffree)
{
	unsigned long long inos = t->inopblock > 64 ? t->inopblock : 64;
	unsigned long long ablks = inos >> t->inopblog;
	unsigned long long maxino = 0, unavail = t->agcount * 8 + t->allocbt, f, fi;

	if (t->imax_pct)
		maxino = (t->dblocks * t->imax_pct / 100 / ablks) * ablks << t->inopblog;
	*total = t->dblocks - (t->logstart ? t->logblocks : 0);
	*free_ = fdblocks > unavail ? fdblocks - unavail : 0;
	f = icount + (*free_ << t->inopblog);
	if (f > (1ull << 56) - 1)
		f = (1ull << 56) - 1;
	if (maxino && f > maxino)
		f = maxino;
	if (f < icount)
		f = icount;
	fi = f - (icount - ifree);
	*files = f;
	*ffree = fi;
}

/* returns 0 when xfs_db could read the image */
static int get_truth(const char *img, struct truth *t)
{
	char sec[32];
	unsigned long long ro;

	memset(t, 0, sizeof(*t));
#define SB(f) db_val(img, "sb 0", #f)
	t->blocksize = SB(blocksize); t->dblocks = SB(dblocks); t->logstart = SB(logstart);
	t->logblocks = SB(logblocks); t->icount = SB(icount); t->ifree = SB(ifree);
	t->fdblocks = SB(fdblocks); t->agcount = SB(agcount); t->imax_pct = SB(imax_pct);
	t->inopblock = SB(inopblock); t->inopblog = SB(inopblog); ro = SB(features_ro_compat);
#undef SB
	if (t->blocksize == ~0ull || t->agcount == ~0ull || t->agcount == 0 || t->agcount > 4096)
		return -1;
	for (unsigned long long ag = 0; ag < t->agcount; ag++) {
		unsigned long long bt, rmapb = 0;

		snprintf(sec, sizeof(sec), "agf %llu", ag);
		bt = db_val(img, sec, "btreeblks");
		t->agf_sum_fd += db_val(img, sec, "freeblks") + db_val(img, sec, "flcount") + bt;
		if (ro & 2)		/* rmapbt: its blocks are not allocbt blocks */
			rmapb = db_val(img, sec, "rmapblocks");
		if ((long long)(bt - (rmapb ? rmapb - 1 : 0)) > 0)
			t->allocbt += bt - (rmapb ? rmapb - 1 : 0);
		snprintf(sec, sizeof(sec), "agi %llu", ag);
		t->agi_icount += db_val(img, sec, "count");
		t->agi_ifree += db_val(img, sec, "freecount");
	}
	truth_expect(t, t->icount, t->ifree, t->fdblocks, &t->total, &t->free, &t->files, &t->ffree);
	truth_expect(t, t->agi_icount, t->agi_ifree, t->agf_sum_fd,
		     &t->s_total, &t->s_free, &t->s_files, &t->s_ffree);
	return 0;
}

static void check_statfs(struct wfs_xfs *x, const struct truth *t, bool from_ag_sums)
{
	wfs_statfs st;
	unsigned long long bs = t->blocksize;

	memset(&st, 0xAA, sizeof(st));
	CHECK(wfs_xfs_op_statfs(x, &st) == 0);
	CHECK_EQ(st.block_size, bs);
	CHECK_EQ(st.name_max, 255);
	CHECK_EQ(st.total_bytes, (from_ag_sums ? t->s_total : t->total) * bs);
	CHECK_EQ(st.free_bytes, (from_ag_sums ? t->s_free : t->free) * bs);
	CHECK_EQ(st.avail_bytes, st.free_bytes);
	CHECK_EQ(st.total_inodes, from_ag_sums ? t->s_files : t->files);
	CHECK_EQ(st.free_inodes, from_ag_sums ? t->s_ffree : t->ffree);
	CHECK(st.free_bytes <= st.total_bytes);
	CHECK(st.free_inodes <= st.total_inodes);
}

/* A clean mkfs image: the sb counters equal the AG sums, so both agree. */
static void check_image(const char *img, const char *logd, const char *rtd, const char *opts)
{
	struct truth t;
	struct mnt m;

	cur = img;
	if (get_truth(img, &t)) {
		CHECK(0 && "xfs_db could not read the image");
		return;
	}
	if (mnt_open(&m, img, logd, rtd, opts)) {
		CHECK(0 && "mount failed");
		return;
	}
	CHECK_EQ(m.x->mp.m_sb.sb_icount, t.icount);
	CHECK_EQ(m.x->mp.m_sb.sb_ifree, t.ifree);
	CHECK_EQ(m.x->mp.m_sb.sb_fdblocks, t.fdblocks);
	CHECK_EQ(atomic64_read(&m.x->mp.m_allocbt_blks), t.allocbt);
	check_statfs(m.x, &t, false);
	mnt_unmount(&m);
}

/* ------------------------------------------------------------- images -- */
static int mkimg(const char *path, const char *opts)
{
	return sh("rm -f '%s' && truncate -s 2g '%s' && mkfs.xfs -q -f %s '%s' >/dev/null 2>&1",
		  path, path, opts, path);
}

/* set sb 0 counters in place (xfs_db's write verifier refuses insane ones); ~0 keeps a field */
static int patch_counters(const char *path, unsigned long long icount, unsigned long long ifree,
			  unsigned long long fdblocks)
{
	uint8_t buf[4096];
	struct xfs_dsb *d = (struct xfs_dsb *)buf;
	int fd = open(path, O_RDWR), rc = -1;
	unsigned sect;

	if (fd < 0 || pread(fd, buf, 512, 0) != 512)
		goto out;
	sect = be16_to_cpu(d->sb_sectsize);
	if (sect < 512 || sect > sizeof(buf) || pread(fd, buf, sect, 0) != (ssize_t)sect)
		goto out;
	if (icount != ~0ull)
		d->sb_icount = cpu_to_be64(icount);
	if (ifree != ~0ull)
		d->sb_ifree = cpu_to_be64(ifree);
	if (fdblocks != ~0ull)
		d->sb_fdblocks = cpu_to_be64(fdblocks);
	xfs_update_cksum((char *)buf, sect, XFS_SB_CRC_OFF);
	rc = pwrite(fd, buf, sect, 0) == (ssize_t)sect ? 0 : -1;
out:
	if (fd >= 0)
		close(fd);
	return rc;
}

struct fixlist { char name[64][128]; int n; };
static int cmp_str(const void *a, const void *b) { return strcmp(a, b); }
static void list_images(const char *dir, const char *suffix, struct fixlist *fl)
{
	DIR *d = opendir(dir);
	struct dirent *e;
	size_t sl = strlen(suffix);

	fl->n = 0;
	while (d && (e = readdir(d)) && fl->n < 64) {
		size_t l = strlen(e->d_name);

		if (l > sl && !strcmp(e->d_name + l - sl, suffix) &&
		    !strstr(e->d_name, ".log.") && !strstr(e->d_name, ".rt.") &&
		    !strstr(e->d_name, "dirty"))
			snprintf(fl->name[fl->n++], sizeof(fl->name[0]), "%s", e->d_name);
	}
	if (d)
		closedir(d);
	qsort(fl->name, (size_t)fl->n, sizeof(fl->name[0]), cmp_str);
}

/* ---------------------------------------------------------- the tests -- */
static void statfs_matches_formula_from_xfs_db_fields(const char *fixdir, const char *rootdir)
{
	struct fixlist fl;
	struct stat st;
	char data[700], logp[700], rtp[700], base[128];
	int nfix = 0, i;
	static const struct { const char *name, *opts; } mk[] = {
		{ "a2", "-m crc=1 -b size=4096 -d agcount=2" },
		{ "b1k16", "-m crc=1 -b size=1024 -d agcount=16" },
		{ "a7", "-m crc=1 -b size=4096 -d agcount=7 -i maxpct=1" },
		{ "pct0", "-m crc=1 -b size=4096 -d agcount=4 -i maxpct=0" },
		{ "pct100", "-m crc=1 -b size=4096 -d agcount=2 -i maxpct=100" },
		{ "rmap", "-m crc=1,reflink=1,rmapbt=1 -b size=4096 -d agcount=5" },
		{ "isize1k", "-m crc=1 -b size=4096 -i size=1024 -d agcount=3" },
		{ "b64k", "-m crc=1 -b size=65536 -d agcount=4" },
		{ "log100m", "-m crc=1 -b size=4096 -d agcount=3 -l size=100m" },
	};

	if (sh("command -v mkfs.xfs >/dev/null 2>&1") || sh("command -v xfs_db >/dev/null 2>&1")) {
		skipped++;
		printf("SKIPPED %s: mkfs.xfs/xfs_db not installed\n", __func__);
		return;
	}
	for (i = 0; i < (int)(sizeof(mk) / sizeof(mk[0])); i++) {
		snprintf(data, sizeof(data), "%s/%s.img", tmp, mk[i].name);
		if (mkimg(data, mk[i].opts)) {
			CHECK(0 && "mkfs failed");
			continue;
		}
		check_image(data, NULL, NULL, "");
		/* the answer must not depend on how the mount got there */
		check_image(data, NULL, NULL, "norecovery");
		sh("rm -f '%s'", data);
	}

	list_images(fixdir, ".img", &fl);
	for (i = 0; i < fl.n; i++) {
		snprintf(base, sizeof(base), "%.*s", (int)strlen(fl.name[i]) - 4, fl.name[i]);
		snprintf(data, sizeof(data), "%s/%s.img", fixdir, base);
		snprintf(logp, sizeof(logp), "%s/%s.log.img", fixdir, base);
		snprintf(rtp, sizeof(rtp), "%s/%s.rt.img", fixdir, base);
		check_image(data, stat(logp, &st) == 0 ? logp : NULL,
			    stat(rtp, &st) == 0 ? rtp : NULL, "");
		nfix++;
	}
	if (!nfix) {
		skipped++;
		printf("  note: no fixture images in %s (run tools/gen-fixtures.sh)\n", fixdir);
	}

	/* real kernel-written images */
	if (!sh("command -v zstd >/dev/null 2>&1")) {
		list_images(rootdir, ".img.zst", &fl);
		for (i = 0; i < fl.n; i++) {
			snprintf(data, sizeof(data), "%s/root.img", tmp);
			if (sh("zstd -dqf '%s/%s' -o '%s'", rootdir, fl.name[i], data)) {
				CHECK(0 && "zstd failed");
				continue;
			}
			check_image(data, NULL, NULL, "norecovery");	/* kernel images may carry a dirty log */
			sh("rm -f '%s'", data);
		}
	}
}

static void block_size_and_name_max_fields(const char *fixdir)
{
	struct fixlist fl;
	static const unsigned bs[] = { 1024, 4096, 65536 };
	char data[700], mkopt[128];
	const wfs_backend_desc *d;
	int seen[3] = { 0 }, i, k;
	struct truth t;
	struct mnt m;
	wfs_statfs st;

	if (sh("command -v mkfs.xfs >/dev/null 2>&1") || sh("command -v xfs_db >/dev/null 2>&1")) {
		skipped++;
		printf("SKIPPED %s: mkfs.xfs/xfs_db not installed\n", __func__);
		return;
	}
	d = wfs_backend_entry();
	CHECK(d && d->ops && d->ops->statfs);
	for (k = 0; k < 3; k++) {
		snprintf(data, sizeof(data), "%s/bs.img", tmp);
		snprintf(mkopt, sizeof(mkopt), "-m crc=1 -b size=%u -d agcount=4", bs[k]);
		CHECK(mkimg(data, mkopt) == 0);
		cur = data;
		CHECK(mnt_open(&m, data, NULL, NULL, "") == 0);
		memset(&st, 0, sizeof(st));
		/* through the exported ops table, as the host calls it */
		CHECK(d->ops->statfs((wfs_fs *)m.x, &st) == 0);
		CHECK_EQ(st.block_size, bs[k]);
		CHECK_EQ(st.name_max, 255);
		CHECK_EQ(st.total_bytes % bs[k], 0);
		CHECK_EQ(st.free_bytes % bs[k], 0);
		CHECK(get_truth(data, &t) == 0);
		CHECK_EQ(st.total_bytes, t.total * bs[k]);
		mnt_unmount(&m);
		sh("rm -f '%s'", data);
	}
	/* every fixture too: block_size always equals the superblock's */
	list_images(fixdir, ".img", &fl);
	for (i = 0; i < fl.n; i++) {
		char logp[700], rtp[700], base[128];
		struct stat s;

		snprintf(base, sizeof(base), "%.*s", (int)strlen(fl.name[i]) - 4, fl.name[i]);
		snprintf(data, sizeof(data), "%s/%s.img", fixdir, base);
		snprintf(logp, sizeof(logp), "%s/%s.log.img", fixdir, base);
		snprintf(rtp, sizeof(rtp), "%s/%s.rt.img", fixdir, base);
		cur = data;
		if (mnt_open(&m, data, stat(logp, &s) == 0 ? logp : NULL,
			     stat(rtp, &s) == 0 ? rtp : NULL, "")) {
			CHECK(0 && "mount failed");
			continue;
		}
		CHECK(wfs_xfs_op_statfs(m.x, &st) == 0);
		CHECK_EQ(st.block_size, m.x->mp.m_sb.sb_blocksize);
		CHECK_EQ(st.name_max, 255);
		for (k = 0; k < 3; k++)
			seen[k] |= st.block_size == bs[k];
		mnt_unmount(&m);
	}
	if (fl.n && !(seen[0] && seen[1] && seen[2]))
		printf("  note: fixtures do not cover every block size (%d %d %d)\n",
		       seen[0], seen[1], seen[2]);
}

/* Insane in-core counters must give clamped, never wrapped, numbers. */
static void insane_counters_are_clamped_not_negative(void)
{
	char data[700];
	struct mnt m;
	struct xfs_sb *sb;
	wfs_statfs st;
	uint64_t bs;

	if (sh("command -v mkfs.xfs >/dev/null 2>&1")) {
		skipped++;
		printf("SKIPPED %s: mkfs.xfs not installed\n", __func__);
		return;
	}
	snprintf(data, sizeof(data), "%s/insane.img", tmp);
	CHECK(mkimg(data, "-m crc=1 -b size=4096 -d agcount=4") == 0);
	cur = data;
	CHECK(mnt_open(&m, data, NULL, NULL, "") == 0);
	sb = &m.x->mp.m_sb;
	bs = sb->sb_blocksize;

	/* fdblocks below the set-aside space: free is 0, not 2^64 - n */
	sb->sb_fdblocks = 1;
	CHECK(wfs_xfs_op_statfs(m.x, &st) == 0);
	CHECK_EQ(st.free_bytes, 0);
	CHECK_EQ(st.avail_bytes, 0);
	CHECK(st.total_inodes >= sb->sb_icount);

	/* fdblocks beyond the data section: free never exceeds total */
	sb->sb_fdblocks = ~0ull >> 2;
	CHECK(wfs_xfs_op_statfs(m.x, &st) == 0);
	CHECK(st.free_bytes <= st.total_bytes);
	CHECK(st.free_inodes <= st.total_inodes);
	CHECK(st.total_inodes <= (1ull << 56) - 1 || st.total_inodes == sb->sb_icount);

	/* more free inodes than inodes: ffree capped by files */
	sb->sb_fdblocks = 100;
	sb->sb_ifree = sb->sb_icount + 1000;
	CHECK(wfs_xfs_op_statfs(m.x, &st) == 0);
	CHECK(st.free_inodes <= st.total_inodes);

	/* icount above maxicount (imax_pct lowered below use): files == icount */
	sb->sb_ifree = 0;
	sb->sb_icount = 1ull << 40;
	CHECK(wfs_xfs_op_statfs(m.x, &st) == 0);
	CHECK(st.total_inodes >= sb->sb_icount || st.total_inodes == (1ull << 56) - 1);
	CHECK(st.free_inodes <= st.total_inodes);

	/* log larger than the data section: total 0, not a wrap */
	sb->sb_icount = 64;
	sb->sb_ifree = 10;
	sb->sb_dblocks = 10;
	sb->sb_logstart = 5;
	sb->sb_logblocks = 1000;
	sb->sb_fdblocks = 3;
	CHECK(wfs_xfs_op_statfs(m.x, &st) == 0);
	CHECK_EQ(st.total_bytes, 0);
	CHECK_EQ(st.free_bytes, 0);
	CHECK_EQ(st.block_size, bs);

	/* huge block counts: bytes saturate instead of overflowing */
	sb->sb_dblocks = ~0ull >> 1;
	sb->sb_logstart = 0;
	sb->sb_fdblocks = ~0ull >> 1;
	CHECK(wfs_xfs_op_statfs(m.x, &st) == 0);
	CHECK(st.total_bytes >= (~0ull >> 1) || st.total_bytes == ~0ull);
	CHECK(st.free_bytes <= st.total_bytes);

	CHECK(wfs_xfs_op_statfs(m.x, NULL) == -WFS_EINVAL);
	mnt_unmount(&m);
	sh("rm -f '%s'", data);
}

/* sb counters that are impossible get replaced by the AG sums at mount */
static void wrong_sb_counters_are_recomputed(void)
{
	static const struct { const char *what; unsigned long long ic, fr, fd; } bad[] = {
		{ "fdblocks above dblocks", ~0ull, ~0ull, 999999999 },
		{ "ifree above icount", ~0ull, 99999999, ~0ull },
		{ "icount zero", 0, 0, ~0ull },
		{ "icount above any possible", 99999999999ull, 3, ~0ull },
	};
	char base[700], img[700];
	struct truth t, w;

	if (sh("command -v mkfs.xfs >/dev/null 2>&1") || sh("command -v xfs_db >/dev/null 2>&1")) {
		skipped++;
		printf("SKIPPED %s: mkfs.xfs/xfs_db not installed\n", __func__);
		return;
	}
	snprintf(base, sizeof(base), "%s/wbase.img", tmp);
	CHECK(mkimg(base, "-m crc=1 -b size=4096 -d agcount=4") == 0);
	CHECK(get_truth(base, &t) == 0);
	CHECK_EQ(t.agi_icount, t.icount);	/* sanity of the reference: mkfs counters = AG sums */
	CHECK_EQ(t.agf_sum_fd, t.fdblocks);
	snprintf(img, sizeof(img), "%s/wrong.img", tmp);
	for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
		struct mnt m;

		sh("cp --sparse=always '%s' '%s'", base, img);
		cur = bad[i].what;
		CHECK(patch_counters(img, bad[i].ic, bad[i].fr, bad[i].fd) == 0);
		CHECK(get_truth(img, &w) == 0);
		CHECK(w.icount != t.icount || w.ifree != t.ifree || w.fdblocks != t.fdblocks);
		if (mnt_open(&m, img, NULL, NULL, "")) {
			CHECK(0 && "mount failed");
			continue;
		}
		CHECK_EQ(m.x->mp.m_sb.sb_icount, t.icount);
		CHECK_EQ(m.x->mp.m_sb.sb_ifree, t.ifree);
		CHECK_EQ(m.x->mp.m_sb.sb_fdblocks, t.fdblocks);
		check_statfs(m.x, &t, true);
		mnt_unmount(&m);
	}
	/*
	 * Wrong but plausible counters on a CLEAN log are trusted, as the kernel
	 * does (design 5.6): a lie that passes the sanity checks is the log's
	 * business, not ours.
	 */
	sh("cp --sparse=always '%s' '%s'", base, img);
	CHECK(patch_counters(img, ~0ull, t.ifree + 1, t.fdblocks - 5) == 0);
	{
		struct mnt m;

		cur = "plausible lie, clean log";
		CHECK(mnt_open(&m, img, NULL, NULL, "") == 0);
		CHECK_EQ(m.x->mp.m_sb.sb_fdblocks, t.fdblocks - 5);
		CHECK_EQ(m.x->mp.m_sb.sb_ifree, t.ifree + 1);
		mnt_unmount(&m);
	}
	sh("rm -f '%s' '%s'", base, img);
}

/* A synthesized dirty log (as test_mount.c) after one transaction record. */
static int make_dirty(const char *img)
{
	struct mnt m;
	uint8_t log[3 * BBSIZE] = { 0 };
	uint8_t *d = log + BBSIZE;
	struct xlog_rec_header *rh = (struct xlog_rec_header *)log;
	uint64_t off;
	uint32_t be, crc;
	int fd, rc = -1;

	if (mnt_open(&m, img, NULL, NULL, ""))
		return -1;
	off = BBTOB((uint64_t)XFS_FSB_TO_DADDR(&m.x->mp, m.x->mp.m_sb.sb_logstart));
	be = cpu_to_be32(0x1234);
	memcpy(d, &be, 4);
	be = cpu_to_be32(1024 - 12);
	memcpy(d + 4, &be, 4);
	d[8] = 0x69;
	d[9] = 0x01;
	rh->h_magicno = cpu_to_be32(XLOG_HEADER_MAGIC_NUM);
	rh->h_cycle = cpu_to_be32(1);
	rh->h_version = cpu_to_be32(2);
	rh->h_len = cpu_to_be32(1024);
	rh->h_lsn = cpu_to_be64(((uint64_t)1 << 32) | 2);
	rh->h_tail_lsn = cpu_to_be64(((uint64_t)1 << 32) | 2);
	rh->h_prev_block = cpu_to_be32((uint32_t)-1);
	rh->h_num_logops = cpu_to_be32(1);
	rh->h_fmt = cpu_to_be32(XLOG_FMT);
	memcpy(&rh->h_fs_uuid, &m.x->mp.m_sb.sb_uuid, 16);
	rh->h_size = cpu_to_be32(32768);
	be = cpu_to_be32(1);
	memcpy(&rh->h_cycle_data[0], d, 4);
	memcpy(d, &be, 4);
	memcpy(&rh->h_cycle_data[1], d + BBSIZE, 4);
	memcpy(d + BBSIZE, &be, 4);
	crc = xfs_start_cksum_update((char *)rh, XLOG_REC_SIZE, offsetof(struct xlog_rec_header, h_crc));
	crc = crc32c(crc, d, 1024);
	rh->h_crc = xfs_end_cksum(crc);
	mnt_unmount(&m);
	fd = open(img, O_WRONLY);
	if (fd >= 0 && pwrite(fd, log, sizeof(log), (off_t)(off + 2 * BBSIZE)) == (ssize_t)sizeof(log))
		rc = 0;
	if (fd >= 0)
		close(fd);
	return rc;
}

/* norecovery on a dirty log must not trust stale (but plausible) sb counters */
static void dirty_log_norecovery_recomputes_counters(void)
{
	char img[700], clean[700];
	struct truth t;
	struct mnt m;

	if (sh("command -v mkfs.xfs >/dev/null 2>&1") || sh("command -v xfs_db >/dev/null 2>&1")) {
		skipped++;
		printf("SKIPPED %s: mkfs.xfs/xfs_db not installed\n", __func__);
		return;
	}
	snprintf(img, sizeof(img), "%s/dirty.img", tmp);
	snprintf(clean, sizeof(clean), "%s/dirty-ref.img", tmp);
	CHECK(mkimg(img, "-m crc=1 -b size=4096 -d agcount=4") == 0);
	CHECK(get_truth(img, &t) == 0);
	/* stale yet sane counters, as a lazy-count fs leaves them after a crash */
	CHECK(patch_counters(img, t.icount + 64, t.ifree + 3, t.fdblocks - 7) == 0);
	sh("cp --sparse=always '%s' '%s'", img, clean);
	CHECK(make_dirty(img) == 0);
	cur = img;
	CHECK(mnt_open(&m, img, NULL, NULL, "") == -WFS_EUCLEAN);	/* still refused by default */
	if (mnt_open(&m, img, NULL, NULL, "norecovery")) {
		CHECK(0 && "norecovery mount failed");
		return;
	}
	CHECK_EQ(m.x->mp.m_sb.sb_icount, t.agi_icount);
	CHECK_EQ(m.x->mp.m_sb.sb_ifree, t.agi_ifree);
	CHECK_EQ(m.x->mp.m_sb.sb_fdblocks, t.agf_sum_fd);
	check_statfs(m.x, &t, true);
	mnt_unmount(&m);
	/* the same stale counters with a clean log are taken as they are */
	cur = clean;
	CHECK(mnt_open(&m, clean, NULL, NULL, "") == 0);
	CHECK_EQ(m.x->mp.m_sb.sb_fdblocks, t.fdblocks - 7);
	CHECK_EQ(m.x->mp.m_sb.sb_icount, t.icount + 64);
	mnt_unmount(&m);
	sh("rm -f '%s' '%s'", img, clean);
}

/* the allocbt block count is filled by reading all AGFs at mount */
static void mount_reads_every_agf(void)
{
	char img[700];
	struct mnt m;
	struct truth t;

	if (sh("command -v mkfs.xfs >/dev/null 2>&1") || sh("command -v xfs_db >/dev/null 2>&1")) {
		skipped++;
		printf("SKIPPED %s: mkfs.xfs/xfs_db not installed\n", __func__);
		return;
	}
	snprintf(img, sizeof(img), "%s/agf.img", tmp);
	CHECK(mkimg(img, "-m crc=1,rmapbt=1,reflink=1 -b size=4096 -d agcount=9") == 0);
	/* a fresh fs has single-block btrees (allocbt 0): raise btreeblks per AG */
	for (int ag = 0; ag < 9; ag++)
		CHECK(sh("xfs_db -x -c 'agf %d' -c 'write btreeblks %d' '%s' >/dev/null 2>&1",
			 ag, 10 + 3 * ag, img) == 0);
	CHECK(get_truth(img, &t) == 0);
	cur = img;
	if (mnt_open(&m, img, NULL, NULL, "")) {
		CHECK(0 && "mount failed");
		return;
	}
	CHECK_EQ(atomic64_read(&m.x->mp.m_allocbt_blks), t.allocbt);
	CHECK_EQ(m.x->mp.m_alloc_set_aside, t.agcount * 8);
	CHECK(t.allocbt > 0);
	for (xfs_agnumber_t a = 0; a < m.x->mp.m_sb.sb_agcount; a++) {
		struct xfs_perag *pag = xfs_perag_get(&m.x->mp, a);

		CHECK(pag && xfs_perag_initialised_agf(pag));
		if (pag)
			xfs_perag_put(pag);
	}
	mnt_unmount(&m);
	sh("rm -f '%s'", img);
}

int main(int argc, char **argv)
{
	const char *fixdir = argc > 1 ? argv[1] : "../../target/xfs-fixtures";
	const char *rootdir = argc > 2 ? argv[2] : "../../tests/xfs/root-fixtures";
	const char *td = getenv("TMPDIR");

	snprintf(tmp, sizeof(tmp), "%s/wfs-statfs-test.XXXXXX", td && *td ? td : "/tmp");
	if (!mkdtemp(tmp)) {
		perror("mkdtemp");
		return 2;
	}
	mem_base = wfs_mem_used();
#define RUN(t, ...) do { int f0 = failures; t(__VA_ARGS__); \
	printf("%s %s\n", failures == f0 ? "PASS" : "FAIL", #t); } while (0)
	RUN(statfs_matches_formula_from_xfs_db_fields, fixdir, rootdir);
	RUN(block_size_and_name_max_fields, fixdir);
	RUN(insane_counters_are_clamped_not_negative);
	RUN(wrong_sb_counters_are_recomputed);
	RUN(dirty_log_norecovery_recomputes_counters);
	RUN(mount_reads_every_agf);
	CHECK(nwrites == 0);
	sh("rm -rf '%s'", tmp);
	printf("test_statfs: %d checks, %d failures, %d skipped\n", checks, failures, skipped);
	return failures ? 1 : 0;
}
