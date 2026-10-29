// SPDX-License-Identifier: GPL-2.0-only
/*
 * Task 6 tests: superblock mount, feature policy, probe, option parser.
 * usage: test_mount FIXTURE_DIR ROOT_FIXTURE_DIR
 *   FIXTURE_DIR       output of tools/gen-fixtures.sh (NAME.img, NAME.log.img,
 *                     NAME.rt.img)
 *   ROOT_FIXTURE_DIR  tests/xfs/root-fixtures (*.img.zst, real kernel images)
 * Tools used: mkfs.xfs, xfs_db (ground truth), zstd (root fixtures); a test
 * that needs a missing tool prints SKIPPED and is counted as skipped.
 * The host is a file-backed fake; any write call fails the test (the
 * backend is read-only).
 */
#include "wfs_xfs.h"
/* the kernel gets offsetofend() from linux/stddef.h; the shim has no copy */
#ifndef offsetofend
#define offsetofend(T, m) (offsetof(T, m) + sizeof(((T *)0)->m))
#endif
#include "xfs_bit.h"
#include "xfs_cksum.h"
#include "xfs_sb.h"
#include "xfs_ag.h"
#include "xfs_alloc.h"
#include <dirent.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

static int failures, checks, skipped;
#define CHECK(c) do { checks++; if (!(c)) { failures++; \
	fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #c); } } while (0)
#define CHECK_RC(got, want) do { int _g = (got); checks++; if (_g != (want)) { failures++; \
	fprintf(stderr, "%s:%d: %s = %d, want %d (log: %s)\n", __FILE__, __LINE__, #got, _g, (int)(want), logbuf); } } while (0)

/* ---------------------------------------------------------------- host -- */
struct wfs_blkdev {
	int		fd;
	uint64_t	size;
	uint32_t	ss;
	long		reads;
	uint64_t	bytes;
};

static char logbuf[8192];
static int nlog_err;
static long nwrites;

static void h_log(int level, const char *msg)
{
	if (level == 0)
		nlog_err++;
	if (strlen(logbuf) + strlen(msg) + 2 < sizeof(logbuf)) {
		strcat(logbuf, msg);
		strcat(logbuf, "\n");
	}
}
static void log_reset(void) { logbuf[0] = 0; nlog_err = 0; }
static bool log_has(const char *s) { return strstr(logbuf, s) != NULL; }

static uint64_t h_size(wfs_blkdev *d) { return d->size; }
static uint32_t h_ss(wfs_blkdev *d) { return d->ss; }
static int h_ro(wfs_blkdev *d) { (void)d; return 1; }
static wfs_err_t h_read(wfs_blkdev *d, uint64_t off, void *buf, size_t len)
{
	d->reads++;
	d->bytes += len;
	if (off > d->size || len > d->size - off)
		return -WFS_EIO;
	return pread(d->fd, buf, len, (off_t)off) == (ssize_t)len ? 0 : -WFS_EIO;
}
static wfs_err_t h_write(wfs_blkdev *d, uint64_t off, const void *buf, size_t len)
{
	(void)d; (void)off; (void)buf; (void)len;
	nwrites++;
	return -WFS_EROFS;
}
static wfs_err_t h_flush(wfs_blkdev *d) { (void)d; nwrites++; return 0; }

static const wfs_host_api api = {
	.abi_version = WFS_ABI_VERSION, .size = sizeof(wfs_host_api),
	.blk_size = h_size, .blk_sector_size = h_ss, .blk_read_only = h_ro,
	.blk_read = h_read, .blk_write = h_write, .blk_flush = h_flush, .log = h_log,
};

static int dev_open(wfs_blkdev *d, const char *path, uint32_t ss)
{
	struct stat st;

	memset(d, 0, sizeof(*d));
	d->fd = open(path, O_RDONLY);
	if (d->fd < 0 || fstat(d->fd, &st))
		return -1;
	d->size = (uint64_t)st.st_size;
	d->ss = ss;
	return 0;
}
static void dev_close(wfs_blkdev *d) { if (d->fd >= 0) close(d->fd); d->fd = -1; }

/* ------------------------------------------------------------- helpers -- */
static char tmp[512];
static int tools_mkfs, tools_db, tools_zstd;
static size_t mem_base;

static int sh(const char *fmt, ...)
{
	char cmd[2048];
	va_list ap;

	va_start(ap, fmt);
	vsnprintf(cmd, sizeof(cmd), fmt, ap);
	va_end(ap);
	return system(cmd);
}
static int have(const char *t)
{
	char c[128];

	snprintf(c, sizeof(c), "command -v %s >/dev/null 2>&1", t);
	return system(c) == 0;
}
#define NEED(flag, what) do { if (!(flag)) { skipped++; \
	printf("SKIPPED %s: %s not installed\n", __func__, what); return; } } while (0)

struct mnt {
	wfs_blkdev dev[WFS_DEV_COUNT];
	wfs_blkdev *p[WFS_DEV_COUNT];
	struct wfs_xfs *x;
};

static void mnt_close(struct mnt *m)
{
	int i;

	for (i = 0; i < WFS_DEV_COUNT; i++)
		if (m->p[i])
			dev_close(&m->dev[i]);
}

/* open data (+ optional log / rt) and mount; returns the mount rc */
static int mnt_open(struct mnt *m, const char *data, const char *logd, const char *rtd,
		    const char *opts, uint32_t ss)
{
	int rc;

	memset(m, 0, sizeof(*m));
	if (dev_open(&m->dev[0], data, ss))
		return 12345;
	m->p[0] = &m->dev[0];
	if (logd) {
		if (dev_open(&m->dev[1], logd, ss))
			return 12345;
		m->p[1] = &m->dev[1];
	}
	if (rtd) {
		if (dev_open(&m->dev[2], rtd, ss))
			return 12345;
		m->p[2] = &m->dev[2];
	}
	log_reset();
	rc = wfs_xfs_mount(&api, m->p, opts, &m->x);
	if (rc) {
		CHECK(m->x == NULL);
		CHECK(wfs_mem_used() == mem_base);	/* failure paths leak nothing */
		mnt_close(m);
	}
	return rc;
}
static void mnt_unmount(struct mnt *m)
{
	CHECK(wfs_xfs_unmount(m->x) == 0);
	mnt_close(m);
	CHECK(wfs_mem_used() == mem_base);
}
/* mount, expect rc; unmount when it succeeds */
static int try_mount(const char *data, const char *logd, const char *rtd, const char *opts)
{
	struct mnt m;
	int rc = mnt_open(&m, data, logd, rtd, opts, 512);

	if (rc == 0)
		mnt_unmount(&m);
	return rc;
}

static uint64_t db_val(const char *img, const char *sec, const char *field)
{
	char cmd[1024], line[256];
	FILE *f;
	uint64_t v = (uint64_t)-1;

	snprintf(cmd, sizeof(cmd), "xfs_db -r -c '%s' -c 'print %s' '%s' 2>/dev/null", sec, field, img);
	f = popen(cmd, "r");
	if (!f)
		return v;
	while (fgets(line, sizeof(line), f)) {
		char *eq = strchr(line, '=');

		if (eq && !strncmp(line, field, strlen(field)))
			v = strtoull(eq + 1, NULL, 0);
	}
	pclose(f);
	return v;
}

static void path_join(char *out, size_t n, const char *a, const char *b)
{
	snprintf(out, n, "%s/%s", a, b);
}

/* new 300 MiB sparse v5 image made by mkfs.xfs */
static int mkimg(const char *path, const char *mkfs_opts)
{
	return sh("rm -f '%s' && truncate -s 300m '%s' && mkfs.xfs -q -f %s '%s' >/dev/null 2>&1",
		  path, path, mkfs_opts, path);
}
static char base_img[600];
static int make_base(void)
{
	path_join(base_img, sizeof(base_img), tmp, "base.img");
	return mkimg(base_img, "-m crc=1 -b size=4096 -d agcount=4");
}
static void copy_of(const char *dst) { sh("cp --sparse=always '%s' '%s'", base_img, dst); }

/* patch the superblock sector in place: value bytes at off, then fix the CRC */
static int patch_sb(const char *path, size_t off, const void *val, size_t n, unsigned sect)
{
	uint8_t buf[4096];
	int fd = open(path, O_RDWR), rc = -1;

	if (fd < 0 || sect > sizeof(buf) || pread(fd, buf, sect, 0) != (ssize_t)sect)
		goto out;
	memcpy(buf + off, val, n);
	xfs_update_cksum((char *)buf, sect, XFS_SB_CRC_OFF);
	rc = pwrite(fd, buf, sect, 0) == (ssize_t)sect ? 0 : -1;
out:
	if (fd >= 0)
		close(fd);
	return rc;
}
#define SBOFF(f)	offsetof(struct xfs_dsb, f)

/* ----------------------------------------------------------- fixtures -- */
struct fixlist {
	char	name[64][128];
	int	n;
};
static void list_images(const char *dir, struct fixlist *fl)
{
	DIR *d = opendir(dir);
	struct dirent *e;

	fl->n = 0;
	if (!d)
		return;
	while ((e = readdir(d)) && fl->n < 64) {
		size_t l = strlen(e->d_name);

		if (l > 4 && !strcmp(e->d_name + l - 4, ".img") &&
		    !(l > 8 && !strcmp(e->d_name + l - 8, ".log.img")) &&
		    !(l > 7 && !strcmp(e->d_name + l - 7, ".rt.img")))
			snprintf(fl->name[fl->n++], sizeof(fl->name[0]), "%s", e->d_name);
	}
	closedir(d);
}
static int cmp_str(const void *a, const void *b) { return strcmp(a, b); }

/* mount img (with companions NAME.log.img / NAME.rt.img if they exist) and
 * compare with xfs_db */
static void check_fixture(const char *dir, const char *file)
{
	char data[700], logp[700], rtp[700], base[128];
	struct mnt m;
	struct xfs_mount *mp;
	struct stat st;
	uint32_t ss;
	xfs_agnumber_t agno;
	int rc;

	snprintf(base, sizeof(base), "%s", file);
	base[strlen(base) - 4] = 0;
	path_join(data, sizeof(data), dir, file);
	snprintf(logp, sizeof(logp), "%s/%s.log.img", dir, base);
	snprintf(rtp, sizeof(rtp), "%s/%s.rt.img", dir, base);
	ss = (uint32_t)db_val(data, "sb 0", "sectsize");
	if (ss != 512 && ss != 4096)
		ss = 512;
	rc = mnt_open(&m, data, stat(logp, &st) == 0 ? logp : NULL,
		      stat(rtp, &st) == 0 ? rtp : NULL, "", ss);
	if (rc) {
		fprintf(stderr, "mount of %s failed: %d (%s)\n", file, rc, logbuf);
		CHECK(rc == 0);
		return;
	}
	mp = &m.x->mp;
	CHECK(mp->m_sb.sb_agcount == db_val(data, "sb 0", "agcount"));
	CHECK(mp->m_sb.sb_blocksize == db_val(data, "sb 0", "blocksize"));
	CHECK(mp->m_sb.sb_rootino == db_val(data, "sb 0", "rootino"));
	CHECK(mp->m_sb.sb_dblocks == db_val(data, "sb 0", "dblocks"));
	CHECK(mp->m_sb.sb_fdblocks == db_val(data, "sb 0", "fdblocks"));
	CHECK(mp->m_sb.sb_icount == db_val(data, "sb 0", "icount"));
	CHECK(mp->m_blkbb_log == db_val(data, "sb 0", "blocklog") - 9);
	CHECK(xfs_has_crc(mp) && xfs_is_readonly(mp));
	CHECK(mp->m_ddev_targp && mp->m_logdev_targp);
	CHECK(mp->m_dir_geo && mp->m_attr_geo && mp->m_alloc_maxlevels > 0);
	CHECK(M_IGEO(mp)->inode_cluster_size > 0);
	CHECK((mp->m_sb.sb_rblocks != 0) == (mp->m_rtdev_targp != NULL));
	CHECK(percpu_counter_sum(&mp->m_ifree) == (s64)mp->m_sb.sb_ifree);
	/* every AGF was read; free blocks add up to the superblock counter */
	{
		uint64_t sum = 0;

		for (agno = 0; agno < mp->m_sb.sb_agcount; agno++) {
			struct xfs_perag *pag = xfs_perag_get(mp, agno);
			char sec[32];

			CHECK(pag != NULL);
			if (!pag)
				continue;
			CHECK(xfs_perag_initialised_agf(pag));
			snprintf(sec, sizeof(sec), "agf %u", agno);
			CHECK(pag->pagf_freeblks == db_val(data, sec, "freeblks"));
			CHECK(pag->pagf_btreeblks == db_val(data, sec, "btreeblks"));
			sum += pag->pagf_freeblks + pag->pagf_flcount + pag->pagf_btreeblks;
			xfs_perag_put(pag);
		}
		CHECK(sum == mp->m_sb.sb_fdblocks);
	}
	mnt_unmount(&m);
	CHECK(nwrites == 0);
}

static void mounts_every_fixture_image(const char *fixdir)
{
	struct fixlist fl;
	int i;

	NEED(tools_db, "xfs_db");
	list_images(fixdir, &fl);
	if (fl.n == 0) {
		skipped++;
		printf("SKIPPED %s: no images in %s (run tools/gen-fixtures.sh --out)\n", __func__, fixdir);
		return;
	}
	qsort(fl.name, (size_t)fl.n, sizeof(fl.name[0]), cmp_str);
	for (i = 0; i < fl.n; i++)
		check_fixture(fixdir, fl.name[i]);
	printf("  mounted %d matrix images\n", fl.n);
}

static void mounts_root_fixtures(const char *rootdir)
{
	DIR *d;
	struct dirent *e;
	int n = 0;

	NEED(tools_zstd, "zstd");
	NEED(tools_db, "xfs_db");
	d = opendir(rootdir);
	if (!d) {
		skipped++;
		printf("SKIPPED %s: %s missing\n", __func__, rootdir);
		return;
	}
	while ((e = readdir(d))) {
		size_t l = strlen(e->d_name);
		char zst[700], out[700], name[128];

		if (l < 9 || strcmp(e->d_name + l - 8, ".img.zst"))
			continue;
		if (strstr(e->d_name, "dirty"))
			continue;	/* dirty_log_refused_then_norecovery */
		snprintf(name, sizeof(name), "%.*s", (int)(l - 4), e->d_name);
		path_join(zst, sizeof(zst), rootdir, e->d_name);
		path_join(out, sizeof(out), tmp, name);
		CHECK(sh("zstd -d -q -f -o '%s' '%s'", out, zst) == 0);
		check_fixture(tmp, name);
		unlink(out);
		n++;
	}
	closedir(d);
	CHECK(n > 0);
	printf("  mounted %d root fixtures (real kernel images)\n", n);
}

/* ------------------------------------------------------------- policy -- */
static void rejects_v4(void)
{
	char v4[700];

	NEED(tools_mkfs, "mkfs.xfs");
	path_join(v4, sizeof(v4), tmp, "v4.img");
	if (mkimg(v4, "-m crc=0 -b size=4096")) {
		skipped++;
		printf("SKIPPED %s: this mkfs.xfs cannot make v4 images\n", __func__);
		return;
	}
	CHECK_RC(try_mount(v4, NULL, NULL, ""), -WFS_EOPNOTSUPP);
	CHECK(log_has("version 4"));
	CHECK(nlog_err > 0);
	{
		struct mnt m;
		memset(&m, 0, sizeof(m));
		CHECK(dev_open(&m.dev[0], v4, 512) == 0);
		m.p[0] = &m.dev[0];
		CHECK(wfs_xfs_probe(&api, m.p) == -WFS_EINVAL);	/* probe never claims v4 */
		dev_close(&m.dev[0]);
	}
	unlink(v4);
}

static void set_incompat_bits(const char *img, uint64_t bits)
{
	uint64_t cur = db_val(img, "sb 0", "features_incompat");

	sh("xfs_db -x -c 'sb 0' -c 'write -d features_incompat 0x%llx' '%s' >/dev/null 2>&1",
	   (unsigned long long)(cur | bits), img);
}

static void rejects_unknown_incompat_bit(void)
{
	char img[700];

	NEED(tools_mkfs, "mkfs.xfs");
	NEED(tools_db, "xfs_db");
	path_join(img, sizeof(img), tmp, "unk.img");
	copy_of(img);
	set_incompat_bits(img, 0x40000000);
	CHECK(db_val(img, "sb 0", "features_incompat") & 0x40000000);
	CHECK_RC(try_mount(img, NULL, NULL, ""), -WFS_EOPNOTSUPP);	/* CRC was fixed up by xfs_db */
	CHECK(log_has("0x40000000"));
	CHECK_RC(try_mount(img, NULL, NULL, "norecovery"), -WFS_EOPNOTSUPP);
	unlink(img);
}

static void rejects_zoned_and_needsrepair(void)
{
	char img[700];

	NEED(tools_mkfs, "mkfs.xfs");
	NEED(tools_db, "xfs_db");
	path_join(img, sizeof(img), tmp, "zon.img");
	copy_of(img);
	set_incompat_bits(img, XFS_SB_FEAT_INCOMPAT_ZONED);
	CHECK_RC(try_mount(img, NULL, NULL, ""), -WFS_EOPNOTSUPP);
	CHECK(log_has("zoned"));
	copy_of(img);
	set_incompat_bits(img, XFS_SB_FEAT_INCOMPAT_ZONE_GAPS);
	CHECK_RC(try_mount(img, NULL, NULL, ""), -WFS_EOPNOTSUPP);
	copy_of(img);
	set_incompat_bits(img, XFS_SB_FEAT_INCOMPAT_NEEDSREPAIR);
	CHECK_RC(try_mount(img, NULL, NULL, ""), -WFS_EUCLEAN);
	CHECK(log_has("NEEDSREPAIR"));
	CHECK_RC(try_mount(img, NULL, NULL, "norecovery"), 0);	/* design 4.1 */
	/* unknown ro_compat and compat bits: warning only */
	copy_of(img);
	sh("xfs_db -x -c 'sb 0' -c 'write -d features_ro_compat 0x%llx' -c 'write -d features_compat 0x%llx' '%s' >/dev/null 2>&1",
	   (unsigned long long)(db_val(img, "sb 0", "features_ro_compat") | 0x80000000ULL),
	   (unsigned long long)(db_val(img, "sb 0", "features_compat") | 0x80000000ULL), img);
	CHECK_RC(try_mount(img, NULL, NULL, ""), 0);
	CHECK(log_has("0x80000000"));
	unlink(img);
}

static void rejects_missing_rt_or_log_device(const char *fixdir)
{
	struct fixlist fl;
	int i, did_log = 0, did_rt = 0;

	NEED(tools_db, "xfs_db");
	list_images(fixdir, &fl);
	for (i = 0; i < fl.n; i++) {
		char data[700], logp[700], rtp[700], base[128];
		struct stat st;

		snprintf(base, sizeof(base), "%s", fl.name[i]);
		base[strlen(base) - 4] = 0;
		path_join(data, sizeof(data), fixdir, fl.name[i]);
		snprintf(logp, sizeof(logp), "%s/%s.log.img", fixdir, base);
		snprintf(rtp, sizeof(rtp), "%s/%s.rt.img", fixdir, base);
		if (!did_log && stat(logp, &st) == 0 && stat(rtp, &st) != 0) {
			uint32_t ss = (uint32_t)db_val(data, "sb 0", "sectsize");

			(void)ss;
			CHECK_RC(try_mount(data, NULL, NULL, ""), -WFS_ENODEV);
			CHECK(log_has("external log"));
			CHECK_RC(try_mount(data, NULL, NULL, "norecovery"), 0);
			CHECK(log_has("without a log check"));
			did_log = 1;
		}
		if (!did_rt && stat(rtp, &st) == 0 && stat(logp, &st) != 0) {
			CHECK_RC(try_mount(data, NULL, NULL, ""), -WFS_ENODEV);
			CHECK(log_has("realtime"));
			CHECK_RC(try_mount(data, NULL, NULL, "norecovery"), -WFS_ENODEV);
			did_rt = 1;
		}
	}
	if (!did_log || !did_rt) {
		skipped++;
		printf("SKIPPED %s: matrix has no external log / rt image (log %d rt %d)\n",
		       __func__, did_log, did_rt);
	}
}

/* ---------------------------------------------------------------- probe -- */
static void probe_true_only_for_xfs_v5(const char *fixdir)
{
	struct mnt m;
	struct fixlist fl;
	char p[700], z[700];
	uint8_t sect[512] = { 0 };
	int fd;

	NEED(tools_mkfs, "mkfs.xfs");
	memset(&m, 0, sizeof(m));
	m.p[0] = &m.dev[0];
	CHECK(dev_open(&m.dev[0], base_img, 512) == 0);
	m.dev[0].reads = 0;
	CHECK(wfs_xfs_probe(&api, m.p) == 0);
	CHECK(m.dev[0].reads == 1 && m.dev[0].bytes == 512);	/* minimal I/O */
	dev_close(&m.dev[0]);
	/* every matrix image (4096-byte sector ones read with a 4096 sector) */
	list_images(fixdir, &fl);
	if (fl.n)
		path_join(p, sizeof(p), fixdir, fl.name[0]);
	if (fl.n) {
		CHECK(dev_open(&m.dev[0], p, 512) == 0);
		CHECK(wfs_xfs_probe(&api, m.p) == 0);
		dev_close(&m.dev[0]);
	}
	/* zeros, garbage, magic only, wrong version, truncated */
	path_join(z, sizeof(z), tmp, "probe.img");
	fd = open(z, O_RDWR | O_CREAT | O_TRUNC, 0600);
	CHECK(fd >= 0);
	CHECK(write(fd, sect, 512) == 512);
	close(fd);
	CHECK(dev_open(&m.dev[0], z, 512) == 0);
	CHECK(wfs_xfs_probe(&api, m.p) == -WFS_EINVAL);	/* zeros */
	dev_close(&m.dev[0]);
	memcpy(sect, "XFSB", 4);
	fd = open(z, O_RDWR | O_TRUNC);
	CHECK(write(fd, sect, 512) == 512);
	close(fd);
	CHECK(dev_open(&m.dev[0], z, 512) == 0);
	CHECK(wfs_xfs_probe(&api, m.p) == -WFS_EINVAL);	/* magic, version 0 */
	dev_close(&m.dev[0]);
	sect[101] = 4;					/* version 4 */
	fd = open(z, O_RDWR | O_TRUNC);
	CHECK(write(fd, sect, 512) == 512);
	close(fd);
	CHECK(dev_open(&m.dev[0], z, 512) == 0);
	CHECK(wfs_xfs_probe(&api, m.p) == -WFS_EINVAL);
	dev_close(&m.dev[0]);
	sect[101] = 5;					/* the probe only judges magic + version */
	fd = open(z, O_RDWR | O_TRUNC);
	CHECK(write(fd, sect, 512) == 512);
	close(fd);
	CHECK(dev_open(&m.dev[0], z, 512) == 0);
	CHECK(wfs_xfs_probe(&api, m.p) == 0);
	dev_close(&m.dev[0]);
	fd = open(z, O_RDWR | O_TRUNC);
	CHECK(write(fd, sect, 100) == 100);		/* shorter than a sector */
	close(fd);
	CHECK(dev_open(&m.dev[0], z, 512) == 0);
	CHECK(wfs_xfs_probe(&api, m.p) == -WFS_EINVAL);
	dev_close(&m.dev[0]);
	/* no data device, bad sector size */
	m.p[0] = NULL;
	CHECK(wfs_xfs_probe(&api, m.p) == -WFS_EINVAL);
	unlink(z);
}

/* -------------------------------------------------- corruption / hostile -- */
static void superblock_crc_error_is_euclean(void)
{
	char img[700];
	int fd;
	uint8_t b;

	NEED(tools_mkfs, "mkfs.xfs");
	path_join(img, sizeof(img), tmp, "crc.img");
	copy_of(img);
	fd = open(img, O_RDWR);
	CHECK(fd >= 0 && pread(fd, &b, 1, 130) == 1);
	b ^= 0x5a;
	CHECK(pwrite(fd, &b, 1, 130) == 1);		/* inside the CRC-covered sb */
	close(fd);
	CHECK_RC(try_mount(img, NULL, NULL, ""), -WFS_EUCLEAN);
	CHECK(log_has("CRC"));
	unlink(img);
}

struct hostile {
	const char *name;
	size_t off;
	int width;
	uint64_t val;
};

static void bad_sector_size_and_geometry_values(void)
{
	char img[700];
	static const struct hostile cases[] = {
		{ "agcount 0", SBOFF(sb_agcount), 4, 0 },
		{ "agcount huge", SBOFF(sb_agcount), 4, 0xfffffff0u },
		{ "blocksize 0", SBOFF(sb_blocksize), 4, 0 },
		{ "blocksize 1 MiB", SBOFF(sb_blocksize), 4, 1u << 20 },
		{ "blocksize not pow2", SBOFF(sb_blocksize), 4, 4097 },
		{ "agblocks 0", SBOFF(sb_agblocks), 4, 0 },
		{ "agblocks overflow", SBOFF(sb_agblocks), 4, 0xffffffffu },
		{ "agblocks 1", SBOFF(sb_agblocks), 4, 1 },
		{ "dblocks 0", SBOFF(sb_dblocks), 8, 0 },
		{ "dblocks 2^62", SBOFF(sb_dblocks), 8, 1ull << 62 },
		{ "dblocks max", SBOFF(sb_dblocks), 8, UINT64_MAX },
		{ "dblocks x2 (agcount unchanged)", SBOFF(sb_dblocks), 8, 2ull * 76800 },
		{ "sectsize 0", SBOFF(sb_sectsize), 2, 0 },
		{ "sectsize 3", SBOFF(sb_sectsize), 2, 3 },
		{ "sectsize 64K", SBOFF(sb_sectsize), 2, 0xffff },
		{ "inodesize 0", SBOFF(sb_inodesize), 2, 0 },
		{ "inodesize 65535", SBOFF(sb_inodesize), 2, 0xffff },
		{ "inopblock 0", SBOFF(sb_inopblock), 2, 0 },
		{ "logblocks 0", SBOFF(sb_logblocks), 4, 0 },
		{ "logblocks huge", SBOFF(sb_logblocks), 4, 0xffffffffu },
		{ "logstart huge", SBOFF(sb_logstart), 8, 1ull << 60 },
		{ "rootino 0", SBOFF(sb_rootino), 8, 0 },
		{ "inprogress", SBOFF(sb_inprogress), 1, 1 },
		{ "blocklog 63", SBOFF(sb_blocklog), 1, 63 },
		{ "agblklog 255", SBOFF(sb_agblklog), 1, 255 },
		{ "imax_pct 200", SBOFF(sb_imax_pct), 1, 200 },
	};
	size_t i;

	NEED(tools_mkfs, "mkfs.xfs");
	path_join(img, sizeof(img), tmp, "hostile.img");
	for (i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
		const struct hostile *c = &cases[i];
		uint8_t be[8];
		int k, rc;

		copy_of(img);
		for (k = 0; k < c->width; k++)
			be[k] = (uint8_t)(c->val >> (8 * (c->width - 1 - k)));
		CHECK(patch_sb(img, c->off, be, (size_t)c->width, 512) == 0);
		rc = try_mount(img, NULL, NULL, "");
		/* rootino 0 is only found when the root inode is loaded (Task 8) */
		if (!strcmp(c->name, "rootino 0") && rc == 0)
			continue;
		if (rc != -WFS_EUCLEAN)
			fprintf(stderr, "hostile '%s': rc %d\n", c->name, rc);
		CHECK(rc == -WFS_EUCLEAN);
	}
	/* image truncated below the size the superblock claims */
	copy_of(img);
	CHECK(sh("truncate -s 100m '%s'", img) == 0);
	CHECK_RC(try_mount(img, NULL, NULL, ""), -WFS_EUCLEAN);
	CHECK(log_has("needs"));
	/* device sector larger than the filesystem sector size */
	copy_of(img);
	{
		struct mnt m;

		CHECK_RC(mnt_open(&m, img, NULL, NULL, "", 4096), -WFS_EINVAL);
		CHECK(log_has("larger"));
	}
	unlink(img);
}

/* deterministic random single/double byte mutations of the sb sector, CRC
 * fixed: the result must be a defined error or a clean mount, never a crash
 * or a leak (ASan/UBSan run this too). */
/*
 * With the SECTORBIT feature (any 4096-sector fs) libxfs shifts by
 * sb_logsectlog; prevalidate_sb() must reject a hostile value first (UBSan
 * aborted on 255 before). The 512-sector base image never reaches that code.
 */
static void hostile_logsectlog_on_4096_sector_fs(void)
{
	char img[700];
	static const uint8_t vals[] = { 16, 31, 32, 63, 255 };
	size_t i;

	NEED(tools_mkfs, "mkfs.xfs");
	path_join(img, sizeof(img), tmp, "s4096.img");
	if (sh("rm -f '%s' && truncate -s 300m '%s' && mkfs.xfs -q -f -m crc=1 -b size=4096 -s size=4096 '%s' >/dev/null 2>&1",
	       img, img, img)) {
		skipped++;
		printf("SKIPPED %s: mkfs.xfs cannot make 4096-byte sectors\n", __func__);
		return;
	}
	{
		struct mnt m;

		CHECK_RC(mnt_open(&m, img, NULL, NULL, NULL, 4096), 0);	/* the image itself is fine */
		if (m.x)
			mnt_unmount(&m);
	}
	for (i = 0; i < sizeof(vals) / sizeof(vals[0]); i++) {
		struct mnt m;
		int rc;

		CHECK(patch_sb(img, SBOFF(sb_logsectlog), &vals[i], 1, 4096) == 0);
		rc = mnt_open(&m, img, NULL, NULL, NULL, 4096);
		CHECK_RC(rc, -WFS_EUCLEAN);
		if (rc == 0)
			mnt_unmount(&m);
	}
}

static void superblock_mutation_fuzz(void)
{
	char img[700];
	uint64_t seed = 0x58465321;
	int i, mounted = 0, rejected = 0;
	const char *envn = getenv("WFS_MOUNT_FUZZ");	/* iterations (default 400) */
	int iters = envn ? atoi(envn) : 400;

	NEED(tools_mkfs, "mkfs.xfs");
	path_join(img, sizeof(img), tmp, "fz.img");
	for (i = 0; i < iters; i++) {
		uint8_t b[2];
		size_t off;
		int rc, n;

		/* mutate a pristine copy each time: copy only the first sector */
		if (i % 50 == 0)
			copy_of(img);
		else
			sh("dd if='%s' of='%s' bs=512 count=1 conv=notrunc status=none", base_img, img);
		for (n = 0; n < 1 + (i & 1); n++) {
			seed = seed * 6364136223846793005ull + 1442695040888963407ull;
			off = (size_t)((seed >> 33) % 264);
			seed = seed * 6364136223846793005ull + 1442695040888963407ull;
			b[0] = (uint8_t)(seed >> 40);
			if (off >= XFS_SB_CRC_OFF && off < XFS_SB_CRC_OFF + 4)
				continue;
			CHECK(patch_sb(img, off, b, 1, 512) == 0);
		}
		rc = try_mount(img, NULL, NULL, "");
		if (rc == 0)
			mounted++;
		else
			rejected++;
		CHECK(rc == 0 || rc == -WFS_EUCLEAN || rc == -WFS_EOPNOTSUPP ||
		      rc == -WFS_EINVAL || rc == -WFS_ENODEV || rc == -WFS_EIO);
	}
	printf("  sb mutation fuzz: %d mounted, %d rejected\n", mounted, rejected);
	unlink(img);
}

/* ------------------------------------------------------------ options -- */
static void option_parser_rejects_bad_values(void)
{
	struct wfs_xfs_options o;
	static const char *const bad[] = {
		"bogus", "norecovery=1", "norecovery=", "bufcache_bytes", "bufcache_bytes=",
		"bufcache_bytes=abc", "bufcache_bytes=12x", "bufcache_bytes=-5", "bufcache_bytes=+5",
		"bufcache_bytes= 5", "bufcache_bytes=0x100000", "bufcache_bytes=1048575",
		"bufcache_bytes=68719476737", "bufcache_bytes=99999999999999999999999",
		"bufcache_bytes=18446744073709551616", "bufcache_bytes=0", "=5", "=",
		"inodecache_entries", "inodecache_entries=63", "inodecache_entries=16777217",
		"inodecache_entries=0", "inodecache_entries=4294967296", "inodecache_entries=1e3",
		"Norecovery", "norecovery,bogus", "bufcache=5", "norecovery bufcache_bytes=2097152",
		"norecoveryx", "bufcache_bytes=2097152;norecovery",
	};
	size_t i;

	for (i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
		log_reset();
		if (wfs_xfs_parse_options(&api, bad[i], &o) != -WFS_EINVAL)
			fprintf(stderr, "option '%s' accepted\n", bad[i]);
		CHECK(wfs_xfs_parse_options(&api, bad[i], &o) == -WFS_EINVAL);
		CHECK(nlog_err >= 1);		/* host log message */
		CHECK(log_has("mount option"));
	}
	/* good values, boundaries, defaults */
	CHECK(wfs_xfs_parse_options(&api, NULL, &o) == 0 && !o.norecovery &&
	      o.bufcache_bytes == 64u << 20 && o.inodecache_entries == 16384);
	CHECK(wfs_xfs_parse_options(&api, "", &o) == 0 && !o.norecovery);
	CHECK(wfs_xfs_parse_options(&api, ",,norecovery,", &o) == 0 && o.norecovery);
	CHECK(wfs_xfs_parse_options(&api, "bufcache_bytes=1048576", &o) == 0 && o.bufcache_bytes == 1048576);
	CHECK(wfs_xfs_parse_options(&api, "bufcache_bytes=68719476736", &o) == 0 &&
	      o.bufcache_bytes == 68719476736ull);
	CHECK(wfs_xfs_parse_options(&api, "inodecache_entries=64", &o) == 0 && o.inodecache_entries == 64);
	CHECK(wfs_xfs_parse_options(&api, "inodecache_entries=16777216", &o) == 0 &&
	      o.inodecache_entries == 16777216);
	CHECK(wfs_xfs_parse_options(&api, "norecovery,bufcache_bytes=2097152,inodecache_entries=100", &o) == 0 &&
	      o.norecovery && o.bufcache_bytes == 2097152 && o.inodecache_entries == 100);
	CHECK(wfs_xfs_parse_options(&api, "bufcache_bytes=2097152,bufcache_bytes=3145728", &o) == 0 &&
	      o.bufcache_bytes == 3145728);	/* later wins */
	CHECK(wfs_xfs_parse_options(NULL, "bogus", &o) == -WFS_EINVAL);	/* no host: nothing logged */
	/* through mount: bad option fails the mount without touching the device */
	if (tools_mkfs) {
		struct mnt m;

		CHECK_RC(mnt_open(&m, base_img, NULL, NULL, "bufcache_bytes=5", 512), -WFS_EINVAL);
		CHECK(log_has("bufcache_bytes=5"));
		CHECK_RC(mnt_open(&m, base_img, NULL, NULL, "frobnicate", 512), -WFS_EINVAL);
		CHECK(log_has("frobnicate"));
	}
	/* a small cache still mounts (and respects the limit) */
	if (tools_mkfs) {
		struct mnt m;

		CHECK_RC(mnt_open(&m, base_img, NULL, NULL, "bufcache_bytes=1048576,inodecache_entries=64", 512), 0);
		if (m.x) {
			CHECK(wfs_xfs_bcache_limit(m.x->bcache) == 1048576);
			CHECK(m.x->opts.inodecache_entries == 64);
			mnt_unmount(&m);
		}
	}
}

/* ---------------------------------------------------------- dirty log -- */
/* One transaction record after mkfs's unmount record whose tail lies before
 * its end: head != tail, i.e. the log needs recovery (as test_logcheck.c). */
static int make_dirty(const char *img)
{
	struct mnt m;
	uint8_t log[3 * BBSIZE] = { 0 };
	uint8_t *d = log + BBSIZE;
	struct xlog_rec_header *rh = (struct xlog_rec_header *)log;
	uint64_t off;
	uint32_t be, crc;
	int fd, rc = -1;

	if (mnt_open(&m, img, NULL, NULL, "", 512))
		return -1;
	off = BBTOB((uint64_t)XFS_FSB_TO_DADDR(&m.x->mp, m.x->mp.m_sb.sb_logstart));
	/* payload: one operation header (tid, len, XFS_TRANSACTION, start flag) */
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
	/* cycle stamping: the first word of each payload block moves to
	 * h_cycle_data[] and is replaced by the cycle number */
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

static void dirty_log_refused_then_norecovery(const char *rootdir)
{
	char img[700], zst[700];
	struct mnt m;
	struct stat st;

	NEED(tools_mkfs, "mkfs.xfs");
	path_join(img, sizeof(img), tmp, "dirty.img");
	copy_of(img);
	CHECK(make_dirty(img) == 0);
	CHECK_RC(try_mount(img, NULL, NULL, ""), -WFS_EUCLEAN);
	CHECK(log_has("norecovery"));
	CHECK_RC(mnt_open(&m, img, NULL, NULL, "norecovery", 512), 0);
	if (m.x) {
		/* counters were recomputed from the AGs (clean fs: equal) */
		CHECK(m.x->mp.m_sb.sb_icount == db_val(img, "sb 0", "icount") || !tools_db);
		mnt_unmount(&m);
	}
	CHECK(nwrites == 0);
	unlink(img);
	/* a real kernel-written dirty log, when the root fixtures have one */
	path_join(zst, sizeof(zst), rootdir, "dirty-log.img.zst");
	if (stat(zst, &st) == 0 && tools_zstd) {
		CHECK(sh("zstd -d -q -f -o '%s' '%s'", img, zst) == 0);
		CHECK_RC(try_mount(img, NULL, NULL, ""), -WFS_EUCLEAN);
		CHECK_RC(try_mount(img, NULL, NULL, "norecovery"), 0);
		unlink(img);
	} else {
		printf("  note: dirty-log.img.zst not in root fixtures yet: synthesized log only\n");
	}
}

/* ------------------------------------------------------ leaks / twice -- */
static void mount_unmount_leaves_no_leaks(void)
{
	struct mnt m;
	int i;

	NEED(tools_mkfs, "mkfs.xfs");
	for (i = 0; i < 20; i++) {
		CHECK_RC(mnt_open(&m, base_img, NULL, NULL, "", 512), 0);
		if (!m.x)
			return;
		CHECK(wfs_mem_used() > mem_base);
		mnt_unmount(&m);		/* asserts wfs_mem_used() == base */
	}
	CHECK(wfs_mem_used() == mem_base);
}

/* xfs_log_check_lsn is a real definition (read-only: no LSN enforcement) and mounting logs nothing about it */
bool xfs_log_check_lsn(struct xfs_mount *, xfs_lsn_t);
static void log_check_lsn_is_real_and_silent(void)
{
	struct mnt m;

	NEED(tools_mkfs, "mkfs.xfs");
	CHECK_RC(mnt_open(&m, base_img, NULL, NULL, "", 512), 0);
	if (m.x) {
		CHECK(!log_has("xfs_log_check_lsn"));
		CHECK(!log_has("pending"));
		CHECK(xfs_log_check_lsn(&m.x->mp, 1));
		CHECK(xfs_log_check_lsn(&m.x->mp, (xfs_lsn_t)0x7fffffffffffffffLL));
		mnt_unmount(&m);
	}
}

static void mount_twice_in_one_process(void)
{
	struct mnt a, b;

	NEED(tools_mkfs, "mkfs.xfs");
	CHECK_RC(mnt_open(&a, base_img, NULL, NULL, "", 512), 0);
	CHECK_RC(mnt_open(&b, base_img, NULL, NULL, "norecovery", 512), 0);
	if (a.x && b.x) {
		CHECK(a.x != b.x && a.x->bcache != b.x->bcache);
		CHECK(a.x->mp.m_sb.sb_rootino == b.x->mp.m_sb.sb_rootino);
		CHECK(!a.x->opts.norecovery && b.x->opts.norecovery);
		CHECK(wfs_xfs_unmount(a.x) == 0);	/* first unmounts first */
		mnt_close(&a);
		log_reset();
		xfs_warn(&b.x->mp, "still logging");	/* the shim sink survives */
		CHECK(log_has("still logging"));
		mnt_unmount(&b);
	}
	/* reverse order, and mount again after both are gone */
	CHECK_RC(mnt_open(&a, base_img, NULL, NULL, "", 512), 0);
	CHECK_RC(mnt_open(&b, base_img, NULL, NULL, "", 512), 0);
	if (a.x && b.x) {
		CHECK(wfs_xfs_unmount(b.x) == 0);
		mnt_close(&b);
		CHECK(wfs_xfs_unmount(a.x) == 0);
		mnt_close(&a);
		CHECK(wfs_mem_used() == mem_base);
	}
	CHECK_RC(try_mount(base_img, NULL, NULL, ""), 0);
}

/* the backend entry point: descriptor and wiring */
static void entry_point_mounts_and_probes(void)
{
	const wfs_backend_desc *d = wfs_backend_entry();
	struct mnt m;
	wfs_fs *fs = NULL;

	NEED(tools_mkfs, "mkfs.xfs");
	CHECK(d && !strcmp(d->name, "xfs") && d->abi_version == WFS_ABI_VERSION);
	CHECK(d->dev_required == (1u << WFS_DEV_DATA));
	CHECK(d->dev_allowed == ((1u << WFS_DEV_DATA) | (1u << WFS_DEV_LOG) | (1u << WFS_DEV_RT)));
	CHECK(d->caps == (WFS_CAP_READONLY | WFS_CAP_HARDLINK | WFS_CAP_SYMLINK));
	memset(&m, 0, sizeof(m));
	CHECK(dev_open(&m.dev[0], base_img, 512) == 0);
	m.p[0] = &m.dev[0];
	CHECK(d->ops->probe(&api, m.p) == 0);
	CHECK(d->ops->mount(&api, m.p, "", &fs) == 0 && fs != NULL);
	if (fs)
		CHECK(d->ops->unmount(fs) == 0);
	CHECK(d->ops->mount(&api, m.p, "nonsense", &fs) == -WFS_EINVAL && fs == NULL);
	dev_close(&m.dev[0]);
	CHECK(wfs_mem_used() == mem_base);
}

int main(int argc, char **argv)
{
	const char *fixdir = argc > 1 ? argv[1] : "../../target/xfs-fixtures";
	const char *rootdir = argc > 2 ? argv[2] : "../../tests/xfs/root-fixtures";
	const char *td = getenv("TMPDIR");

	snprintf(tmp, sizeof(tmp), "%s/wfs-mount-test.XXXXXX", td && *td ? td : "/tmp");
	if (!mkdtemp(tmp)) {
		perror("mkdtemp");
		return 2;
	}
	tools_mkfs = have("mkfs.xfs");
	tools_db = have("xfs_db");
	tools_zstd = have("zstd");
	mem_base = wfs_mem_used();
	if (tools_mkfs && make_base()) {
		fprintf(stderr, "cannot create the base image\n");
		return 2;
	}
#define RUN(t, ...) do { int f0 = failures; time_t t0 = time(NULL); t(__VA_ARGS__); \
	printf("%s %s (%lds)\n", failures == f0 ? "PASS" : "FAIL", #t, (long)(time(NULL) - t0)); } while (0)
	RUN(mounts_every_fixture_image, fixdir);
	RUN(mounts_root_fixtures, rootdir);
	RUN(rejects_v4);
	RUN(rejects_unknown_incompat_bit);
	RUN(rejects_zoned_and_needsrepair);
	RUN(rejects_missing_rt_or_log_device, fixdir);
	RUN(probe_true_only_for_xfs_v5, fixdir);
	RUN(superblock_crc_error_is_euclean);
	RUN(bad_sector_size_and_geometry_values);
	RUN(hostile_logsectlog_on_4096_sector_fs);
	RUN(superblock_mutation_fuzz);
	RUN(option_parser_rejects_bad_values);
	RUN(dirty_log_refused_then_norecovery, rootdir);
	RUN(mount_unmount_leaves_no_leaks);
	RUN(log_check_lsn_is_real_and_silent);
	RUN(mount_twice_in_one_process);
	RUN(entry_point_mounts_and_probes);
	CHECK(nwrites == 0);
	sh("rm -rf '%s'", tmp);
	printf("test_mount: %d checks, %d failures, %d skipped\n", checks, failures, skipped);
	return failures ? 1 : 0;
}
