// SPDX-License-Identifier: GPL-2.0-only
/*
 * M3a T4 tests: read-write mount, the first logged transaction, clean
 * unmount, read-only demotion and shutdown on write errors.
 * usage: test_rw TMPDIR
 * The host is a file-backed fake that really writes (pwrite) and counts
 * writes and flushes; it can be made to fail every write. Ground truth comes
 * from xfsprogs: xfs_repair -n (consistency), xfs_db (superblock fields),
 * xfs_logprint (log contents). Images are made with mkfs.xfs in TMPDIR.
 */
#include "wfs_xfs.h"
#include "xfs_sb.h"
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
static bool log_has(const char *s) { return strstr(logbuf, s) != NULL; }

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
	nwrites++;
	if (d->ro || fail_writes || off > d->size || len > d->size - off)
		return -WFS_EIO;
	return pwrite(d->fd, buf, len, (off_t)off) == (ssize_t)len ? 0 : -WFS_EIO;
}
static wfs_err_t h_flush(wfs_blkdev *d)
{
	(void)d;
	nflushes++;
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
static void sh_out(char *out, size_t n, const char *fmt, ...)
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
static void log_head(struct wfs_xfs *x, int *cycle, int *block)
{
	struct xlog *log = x->mp.m_log;

	*cycle = log->l_curr_cycle;
	*block = log->l_curr_block;
}

/* ---------------------------------------------------------------- tests -- */

/* A writable device mounts read-write; unmount writes an unmount record and
 * leaves a filesystem xfs_repair accepts, with the log head moving forward. */
static void rw_mount_unmount_is_clean(void)
{
	const char *img = mkimg("clean", PLAIN);
	int c0 = 0, b0 = 0, c1 = 0, b1 = 0;
	struct mnt m;

	CHECK(img != NULL);
	if (!img)
		return;
	nwrites = 0;
	CHECK_RC(mnt_open(&m, img, "", false), 0);
	if (!m.x)
		return;
	CHECK(m.x->rw);
	CHECK(!xfs_is_readonly(&m.x->mp));
	CHECK(m.x->mp.m_log != NULL);
	log_head(m.x, &c0, &b0);
	CHECK_RC(mnt_close(&m), 0);
	CHECK(nwrites > 0);		/* at least the unmount record */
	CHECK(repair_clean(img));

	CHECK_RC(mnt_open(&m, img, "", false), 0);
	if (!m.x)
		return;
	log_head(m.x, &c1, &b1);
	CHECK(c1 > c0 || (c1 == c0 && b1 > b0));
	CHECK_RC(mnt_close(&m), 0);
	CHECK(repair_clean(img));
}

/* xfs_sync_sb is a real logged transaction: synchronous commit forces the
 * log (the change is in the log before the call returns), unmount writes the
 * superblock home; xfs_db sees the new label, xfs_repair is happy. */
static void sync_sb_transaction_reaches_disk(void)
{
	const char *img = mkimg("label", PLAIN_STR " -L before");
	char snap[600], out[256];
	struct mnt m;
	long w0, f0;

	CHECK(img != NULL);
	if (!img)
		return;
	CHECK_RC(mnt_open(&m, img, "", false), 0);
	if (!m.x)
		return;
	CHECK(m.x->rw);
	w0 = nwrites;
	f0 = nflushes;
	spin_lock(&m.x->mp.m_sb_lock);
	memset(m.x->mp.m_sb.sb_fname, 0, sizeof(m.x->mp.m_sb.sb_fname));
	memcpy(m.x->mp.m_sb.sb_fname, "wfsrw", 5);
	spin_unlock(&m.x->mp.m_sb_lock);
	CHECK_RC(xfs_sync_sb(&m.x->mp, true), 0);
	CHECK(nwrites > w0);		/* the iclog */
	CHECK(nflushes > f0);		/* PREFLUSH/FUA of a sync commit */

	/* the transaction is in the log already (crash here = replayable) */
	snprintf(snap, sizeof(snap), "%s/label-snap.img", tmp);
	CHECK(sh("cp --sparse=always '%s' '%s'", img, snap) == 0);
	sh_out(out, sizeof(out), "xfs_logprint -t '%s' 2>/dev/null | grep -o 'state: <[A-Z]*>'", snap);
	CHECK(strcmp(out, "state: <DIRTY>") == 0);

	CHECK_RC(mnt_close(&m), 0);
	CHECK(repair_clean(img));
	/* (xfs_db's label command reads the secondaries too: ask sb 0) */
	sh_out(out, sizeof(out), "xfs_db -r -c 'sb 0' -c 'p fname' '%s' 2>/dev/null", img);
	CHECK(strncmp(out, "fname = \"wfsrw\\000", 16) == 0);
	sh_out(out, sizeof(out), "xfs_logprint -t '%s' 2>/dev/null | grep -o 'state: <[A-Z]*>'", img);
	CHECK(strcmp(out, "state: <CLEAN>") == 0);
	unlink(snap);
}

/* The ro option, a read-only host device and unsupported features all mount
 * read-only and write nothing at all. */
static void read_only_demotion_writes_nothing(void)
{
	const char *plain = mkimg("ro", PLAIN);
	const char *rmap = mkimg("rmap", "-m crc=1,rmapbt=1,reflink=1 -n parent=0");
	const char *ppt = mkimg("ppt", "-m crc=1,rmapbt=1,reflink=1 -n parent=1");
	const char *mdir = mkimg("mdir", "-m crc=1,metadir=1");
	struct mnt m;

	CHECK(plain && rmap && ppt && mdir);
	if (!plain || !rmap || !ppt || !mdir)
		return;
	nwrites = nflushes = 0;
	CHECK_RC(mnt_open(&m, plain, "ro", false), 0);
	CHECK(m.x && !m.x->rw && xfs_is_readonly(&m.x->mp));
	CHECK_RC(mnt_close(&m), 0);

	log_reset();
	CHECK_RC(mnt_open(&m, plain, "", true), 0);
	CHECK(m.x && !m.x->rw);
	CHECK(log_has("read-only"));
	CHECK_RC(mnt_close(&m), 0);

	log_reset();
	CHECK_RC(mnt_open(&m, mdir, "", false), 0);
	CHECK(m.x && !m.x->rw);
	CHECK(log_has("metadir"));
	CHECK_RC(mnt_close(&m), 0);

	CHECK(nwrites == 0 && nflushes == 0);
	/* parent pointers are writable now */
	log_reset();
	CHECK_RC(mnt_open(&m, ppt, "", false), 0);
	CHECK(m.x && m.x->rw);
	CHECK_RC(mnt_close(&m), 0);
	/* rmapbt and reflink are writable now (M3d) */
	log_reset();
	CHECK_RC(mnt_open(&m, rmap, "", false), 0);
	CHECK(m.x && m.x->rw);
	CHECK_RC(mnt_close(&m), 0);
	/* a read-only mount still reads the same root */
	log_reset();
	CHECK_RC(mnt_open(&m, plain, "rw", false), 0);
	CHECK(m.x && m.x->rw);
	if (!m.x || !m.x->rw)
		fprintf(stderr, "log: %s\n", logbuf);
	CHECK_RC(mnt_close(&m), 0);
}

/* A failing device shuts the filesystem down: the commit reports the error,
 * later commits fail fast, unmount completes and returns. */
static void write_error_shuts_down(void)
{
	const char *img = mkimg("eio", PLAIN);
	struct mnt m;
	int rc;

	CHECK(img != NULL);
	if (!img)
		return;
	CHECK_RC(mnt_open(&m, img, "", false), 0);
	if (!m.x)
		return;
	fail_writes = true;
	rc = xfs_sync_sb(&m.x->mp, true);
	CHECK(rc != 0);
	CHECK(xfs_is_shutdown(&m.x->mp));
	CHECK(xfs_sync_sb(&m.x->mp, true) != 0);
	mnt_close(&m);			/* must not hang */
	fail_writes = false;
	/* nothing reached the disk: still the clean mkfs image */
	CHECK(repair_clean(img));
}

/* mount -> unmount many times keeps the image clean and the memory balanced */
static void repeated_rw_mounts_balance(void)
{
	const char *img = mkimg("repeat", PLAIN);
	size_t base = wfs_mem_used();
	struct mnt m;
	int i;

	CHECK(img != NULL);
	if (!img)
		return;
	for (i = 0; i < 20; i++) {
		CHECK_RC(mnt_open(&m, img, "", false), 0);
		if (!m.x)
			break;
		CHECK_RC(xfs_sync_sb(&m.x->mp, i & 1), 0);
		CHECK_RC(mnt_close(&m), 0);
	}
	CHECK(wfs_mem_used() == base);
	CHECK(repair_clean(img));
}

int main(int argc, char **argv)
{
	if (argc < 2) {
		fprintf(stderr, "usage: test_rw TMPDIR\n");
		return 2;
	}
	snprintf(tmp, sizeof(tmp), "%s/test_rw.%d", argv[1], (int)getpid());
	if (sh("mkdir -p '%s'", tmp))
		return 2;
	RUN(rw_mount_unmount_is_clean);
	RUN(sync_sb_transaction_reaches_disk);
	RUN(read_only_demotion_writes_nothing);
	RUN(write_error_shuts_down);
	RUN(repeated_rw_mounts_balance);
	if (!failures)
		sh("rm -rf '%s'", tmp);
	else
		printf("images kept in %s\n", tmp);
	printf("test_rw: %d checks, %d failures, %d skipped\n", checks, failures, skipped);
	return failures ? 1 : 0;
}
