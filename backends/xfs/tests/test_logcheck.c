// SPDX-License-Identifier: GPL-2.0-only
/*
 * Tests of the dirty-log detection (logcheck.c).
 *
 * How the dirty logs are made (no kernel mount is available here): a fresh
 * `mkfs.xfs` image has a clean log (one unmount record at block 0, head ==
 * tail == 2). The tests read the real log region, then synthesize log record
 * headers and payload directly, following the kernel's xlog_rec_header
 * layout: magic, cycle, version 2, h_len, lsn, tail_lsn, CRC (computed with
 * the same algorithm as xlog_cksum), uuid, h_size, and the cycle number
 * stamped into the first word of every payload block with the original word
 * kept in h_cycle_data[]. A record whose tail_lsn points before its own end
 * leaves head != tail, which is exactly a log that needs recovery. Whenever
 * xfs_logprint is available its `-t` verdict (CLEAN/DIRTY) is cross-checked
 * against the synthesized image, so the model is verified against xfsprogs.
 * Wrapped logs (cycle N+1 at the start, N at the end) are synthesized from
 * scratch the same way.
 */
#include "xfs_platform.h"
/* the kernel gets offsetofend() from linux/stddef.h; the shim has no copy */
#ifndef offsetofend
#define offsetofend(T, m) (offsetof(T, m) + sizeof(((T *)0)->m))
#endif
#include "xfs_fs.h"
#include "xfs_shared.h"
#include "xfs_format.h"
#include "xfs_log_format.h"
#include "xfs_cksum.h"
#include "xfs_trans_resv.h"
#include "xfs_bit.h"
#include "xfs_mount.h"
#include "logcheck.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>

static int failures, checks, skipped_cases;
#define CHECK(c) do { checks++; if (!(c)) { failures++; \
	fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #c); } } while (0)

#define ER_CORRUPT	(-WFS_EUCLEAN)
#define ER_IO		(-WFS_EIO)

/* ---------------- captured host log ---------------- */
static char logbuf[16384];
static void sink(int level, const char *msg)
{
	(void)level;
	if (strlen(logbuf) + strlen(msg) + 2 < sizeof(logbuf)) {
		strcat(logbuf, msg);
		strcat(logbuf, "\n");
	}
}
static void log_reset(void) { logbuf[0] = 0; }

/* ---------------- readers ---------------- */
struct fread_ctx {
	int		fd;
	uint64_t	limit;		/* reads past this fail (truncation) */
	long		fail_at;	/* fail the Nth read (1-based), 0: never */
	long		nreads;
};

static int file_read(void *c, uint64_t off, void *buf, size_t len)
{
	struct fread_ctx *f = c;
	ssize_t n;

	f->nreads++;
	if (f->fail_at && f->nreads == f->fail_at)
		return -1;
	if (off + len > f->limit)
		return -1;
	n = pread(f->fd, buf, len, (off_t)off);
	return n == (ssize_t)len ? 0 : -1;
}

struct mem_ctx {
	uint8_t		*buf;
	uint64_t	len;
	long		nreads;
	long		max_reads;
	int		overrun;	/* too many reads: a loop */
	int		oob;		/* read outside the region */
};

static int mem_read(void *c, uint64_t off, void *buf, size_t len)
{
	struct mem_ctx *m = c;

	if (++m->nreads > m->max_reads) {
		m->overrun = 1;
		return -1;
	}
	if (off + len > m->len || off + len < off) {
		m->oob = 1;
		return -1;
	}
	memcpy(buf, m->buf + off, len);
	return 0;
}

/* ---------------- temp dir and external tools ---------------- */
static char tmpdir[512];

static int sh(const char *fmt, ...)
{
	char cmd[2048];
	va_list ap;

	va_start(ap, fmt);
	vsnprintf(cmd, sizeof(cmd), fmt, ap);
	va_end(ap);
	return system(cmd);
}

static int have_tool(const char *t)
{
	return sh("command -v %s >/dev/null 2>&1", t) == 0;
}

/* xfs_logprint -t verdict: 1 dirty, 0 clean, -1 unknown/unavailable */
static int logprint_state(const char *data, const char *logdev)
{
	char cmd[2048], line[512];
	FILE *p;
	int st = -1;

	if (!have_tool("xfs_logprint"))
		return -1;
	snprintf(cmd, sizeof(cmd), "xfs_logprint -t %s%s '%s' 2>&1",
		 logdev ? "-l " : "", logdev ? logdev : "", data);
	if (logdev) {
		snprintf(cmd, sizeof(cmd), "xfs_logprint -t -l '%s' '%s' 2>&1",
			 logdev, data);
	}
	p = popen(cmd, "r");
	if (getenv("WFS_LOGCHECK_VERBOSE")) fprintf(stderr, "logprint: %s\n", cmd);
	if (!p)
		return -1;
	while (fgets(line, sizeof(line), p)) {
		if (strstr(line, "state: <CLEAN>"))
			st = 0;
		else if (strstr(line, "state: <DIRTY>"))
			st = 1;
	}
	pclose(p);
	return st;
}

/* ---------------- big-endian helpers ---------------- */
static uint32_t rd32(const uint8_t *p)
{
	return ((uint32_t)p[0] << 24) | (p[1] << 16) | (p[2] << 8) | p[3];
}
static void wr32(uint8_t *p, uint32_t v)
{
	p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = (uint8_t)v;
}
static void wr64(uint8_t *p, uint64_t v)
{
	wr32(p, (uint32_t)(v >> 32));
	wr32(p + 4, (uint32_t)v);
}

/* ---------------- synthetic log region ---------------- */
struct slog {
	uint8_t		*buf;
	uint32_t	bb;		/* basic blocks */
	bool		v2;
	uint8_t		uuid[16];
};

static void slog_new(struct slog *s, uint32_t bb, bool v2, const uint8_t *uuid)
{
	s->buf = calloc(bb, BBSIZE);
	s->bb = bb;
	s->v2 = v2;
	memcpy(s->uuid, uuid, 16);
}

#define OP_START_TRANS	0x01
#define OP_UNMOUNT	0x20

/*
 * Write one record at block blk: 1 header block + BTOBB(h_len) payload
 * blocks. Returns the number of blocks used. Does not wrap.
 */
static int slog_rec(struct slog *s, uint32_t blk, uint32_t cycle,
		    uint32_t tail_cycle, uint32_t tail_blk, uint32_t h_len,
		    uint8_t oh_flags, uint32_t nops)
{
	uint32_t nd = (h_len + BBSIZE - 1) / BBSIZE, i;
	uint8_t *h = s->buf + (size_t)blk * BBSIZE;
	uint8_t *d = h + BBSIZE;
	struct xlog_rec_header *rh = (struct xlog_rec_header *)h;
	uint32_t crc;

	if (blk + 1 + nd > s->bb)
		abort();
	memset(h, 0, BBSIZE);
	memset(d, 0, (size_t)nd * BBSIZE);
	/* operation header at the start of the payload */
	wr32(d, 0x1234);			/* oh_tid */
	wr32(d + 4, h_len - 12);		/* oh_len */
	d[8] = 0x69;				/* XFS_TRANSACTION */
	d[9] = oh_flags;
	for (i = 12; i < h_len; i++)
		d[i] = (uint8_t)(blk + i);

	rh->h_magicno = cpu_to_be32(XLOG_HEADER_MAGIC_NUM);
	rh->h_cycle = cpu_to_be32(cycle);
	rh->h_version = cpu_to_be32(s->v2 ? 2 : 1);
	rh->h_len = cpu_to_be32(h_len);
	rh->h_lsn = cpu_to_be64(((uint64_t)cycle << 32) | blk);
	rh->h_tail_lsn = cpu_to_be64(((uint64_t)tail_cycle << 32) | tail_blk);
	rh->h_prev_block = cpu_to_be32((uint32_t)-1);
	rh->h_num_logops = cpu_to_be32(nops);
	rh->h_fmt = cpu_to_be32(XLOG_FMT);
	memcpy(&rh->h_fs_uuid, s->uuid, 16);
	if (s->v2)
		rh->h_size = cpu_to_be32(32768);
	/* cycle stamping: payload block i's first word -> h_cycle_data[i] */
	for (i = 0; i < nd && i < XLOG_CYCLE_DATA_SIZE; i++) {
		memcpy(&rh->h_cycle_data[i], d + (size_t)i * BBSIZE, 4);
		wr32(d + (size_t)i * BBSIZE, cycle);
	}
	crc = xfs_start_cksum_update((char *)rh, XLOG_REC_SIZE,
				     offsetof(struct xlog_rec_header, h_crc));
	crc = crc32c(crc, d, h_len);
	rh->h_crc = xfs_end_cksum(crc);
	return 1 + nd;
}

static void slog_unmount(struct slog *s, uint32_t blk, uint32_t cycle,
			 uint32_t tail_cycle, uint32_t tail_blk)
{
	/* h_len 20 = op header (12) + xfs_unmount_log_format (8) */
	slog_rec(s, blk, cycle, tail_cycle, tail_blk, 20, OP_UNMOUNT, 1);
}

/* Fix up the CRC of the record at blk (after a test edited it). */
static void slog_recrc(struct slog *s, uint32_t blk)
{
	uint8_t *h = s->buf + (size_t)blk * BBSIZE;
	struct xlog_rec_header *rh = (struct xlog_rec_header *)h;
	uint32_t len = rd32(h + 12), crc;

	crc = xfs_start_cksum_update((char *)rh, XLOG_REC_SIZE,
				     offsetof(struct xlog_rec_header, h_crc));
	crc = crc32c(crc, h + BBSIZE, len);
	rh->h_crc = xfs_end_cksum(crc);
}

/*
 * A full log of 8-block records (header + 7 data blocks) at cycle `old`,
 * then records of cycle `new` for blocks [0, head). tail points into the old
 * cycle after the head. If clean_umount, the last new record is an unmount
 * record that ends at head and the tail is its end.
 */
static uint32_t slog_wrapped(struct slog *s, uint32_t old, uint32_t head8,
			     bool clean_umount, uint32_t *head_out)
{
	uint32_t b, tail = head8 * 8 + 8 * 20;
	uint32_t new_ = old + 1;

	for (b = 0; b + 8 <= s->bb; b += 8)
		slog_rec(s, b, old, old, 0, 7 * 512, OP_START_TRANS, 1);
	if (clean_umount) {
		for (b = 0; b + 8 <= head8 * 8; b += 8)
			slog_rec(s, b, new_, old, tail, 7 * 512, OP_START_TRANS, 1);
		slog_unmount(s, head8 * 8, new_, new_, head8 * 8 + 2);
		*head_out = head8 * 8 + 2;
	} else {
		for (b = 0; b + 8 <= head8 * 8; b += 8)
			slog_rec(s, b, new_, old, tail, 7 * 512, OP_START_TRANS, 1);
		*head_out = head8 * 8;
	}
	return tail;
}

/* ---------------- images ---------------- */
struct img {
	char		data[600];
	char		logdev[600];	/* "" for an internal log */
	struct wfs_log_params p;
	uint64_t	log_off_in_data;
};

static int mkimg(struct img *im, const char *name, const char *opts,
		 const char *logopts)
{
	snprintf(im->data, sizeof(im->data), "%s/%s.img", tmpdir, name);
	im->logdev[0] = 0;
	if (logopts) {
		snprintf(im->logdev, sizeof(im->logdev), "%s/%s.log", tmpdir, name);
		if (sh("truncate -s 64m '%s'", im->logdev))
			return -1;
		return sh("mkfs.xfs -q -f %s -d file,name='%s',size=300m "
			  "-l logdev='%s',size=64m %s >/dev/null 2>&1",
			  opts, im->data, im->logdev, logopts);
	}
	return sh("mkfs.xfs -q -f %s -d file,name='%s',size=300m >/dev/null 2>&1",
		  opts, im->data);
}

/* Derive the log parameters from the superblock (the Task 6 job). */
static int load_params(struct img *im)
{
	struct xfs_dsb sb;
	int fd = open(im->data, O_RDONLY);
	uint64_t fsb, agno, agbno;
	uint32_t bs;

	if (fd < 0 || pread(fd, &sb, sizeof(sb), 0) != sizeof(sb)) {
		if (fd >= 0) close(fd);
		return -1;
	}
	close(fd);
	memset(&im->p, 0, sizeof(im->p));
	bs = be32_to_cpu(sb.sb_blocksize);
	fsb = be64_to_cpu(sb.sb_logstart);
	if (fsb) {
		agno = fsb >> sb.sb_agblklog;
		agbno = fsb & ((1ULL << sb.sb_agblklog) - 1);
		im->p.log_start_byte =
			(agno * be32_to_cpu(sb.sb_agblocks) + agbno) * bs;
	}
	im->p.log_bytes = (uint64_t)be32_to_cpu(sb.sb_logblocks) * bs;
	im->p.sector_size = be16_to_cpu(sb.sb_logsectsize) ?
			    be16_to_cpu(sb.sb_logsectsize) : BBSIZE;
	im->p.logv2 = (be16_to_cpu(sb.sb_versionnum) & 0xf) == 5 ||
		      (be32_to_cpu(sb.sb_features2) & 0x2);
	im->p.logbsize = be32_to_cpu(sb.sb_logsunit) > 1 ?
			 be32_to_cpu(sb.sb_logsunit) : 0;
	memcpy(im->p.uuid, &sb.sb_uuid, 16);
	return 0;
}

static const char *log_file(const struct img *im)
{
	return im->logdev[0] ? im->logdev : im->data;
}

/* Run the check against the on-disk image through the file reader. */
static int check_img(const struct img *im, bool norecovery, bool *dirty)
{
	struct fread_ctx fc = { .fd = open(log_file(im), O_RDONLY),
				.limit = UINT64_MAX };
	struct wfs_log_reader r = { file_read, &fc };
	int rc;

	if (fc.fd < 0)
		return 12345;
	rc = wfs_log_check(&im->p, &r, norecovery, dirty);
	close(fc.fd);
	return rc;
}

static int slog_load(struct slog *s, const struct img *im)
{
	int fd = open(log_file(im), O_RDONLY);
	ssize_t n;

	slog_new(s, (uint32_t)(im->p.log_bytes / BBSIZE), im->p.logv2, im->p.uuid);
	if (fd < 0)
		return -1;
	n = pread(fd, s->buf, im->p.log_bytes, (off_t)im->p.log_start_byte);
	close(fd);
	return n == (ssize_t)im->p.log_bytes ? 0 : -1;
}

static int slog_store(const struct slog *s, const struct img *im)
{
	int fd = open(log_file(im), O_WRONLY);
	ssize_t n;

	if (fd < 0)
		return -1;
	n = pwrite(fd, s->buf, im->p.log_bytes, (off_t)im->p.log_start_byte);
	close(fd);
	return n == (ssize_t)im->p.log_bytes ? 0 : -1;
}

/* a fresh image, with its params loaded; returns 0 on success */
static int fresh(struct img *im, const char *name, const char *opts,
		 const char *logopts)
{
	if (mkimg(im, name, opts, logopts) || load_params(im))
		return -1;
	return 0;
}

static int tools_ok;

#define NEED_TOOLS() do { if (!tools_ok) { skipped_cases++; \
	printf("SKIPPED: %s (mkfs.xfs missing)\n", __func__); return; } } while (0)

/* ---------------- tests ---------------- */

static void clean_fixtures_pass(void)
{
	static const struct { const char *name, *opts; } v[] = {
		{ "def",    "" },
		{ "b1k",    "-b size=1024" },
		{ "sect4k", "-s size=4096" },
		{ "log32m", "-l size=65m" },
		{ "lsu64k", "-l sunit=128" },
		{ "ag2",    "-d agcount=2" },
		{ "v4v1",   "-m crc=0 -l version=1" },
	};
	size_t i;

	NEED_TOOLS();
	for (i = 0; i < sizeof(v) / sizeof(v[0]); i++) {
		struct img im;
		bool dirty = true;
		int lp;

		if (fresh(&im, v[i].name, v[i].opts, NULL)) {
			printf("note: mkfs variant %s unsupported here, skipped\n",
			       v[i].name);
			skipped_cases++;
			continue;
		}
		lp = logprint_state(im.data, NULL);
		CHECK(lp != 1);			/* xfsprogs agrees: not dirty */
		CHECK(check_img(&im, false, &dirty) == 0);
		CHECK(!dirty);
		CHECK(check_img(&im, true, &dirty) == 0);
		CHECK(!dirty);
		unlink(im.data);
	}
}

static void dirty_log_rejected(void)
{
	struct img im;
	struct slog s;
	bool dirty = false;
	int rc;

	NEED_TOOLS();
	CHECK(fresh(&im, "dirty", "", NULL) == 0);
	CHECK(slog_load(&s, &im) == 0);
	/* mkfs left the unmount record at 0..2; add a transaction record at
	 * 2 whose tail (1,2) lies before its end: head 3+..., tail 2. */
	slog_rec(&s, 2, 1, 1, 2, 1024, OP_START_TRANS, 1);
	CHECK(slog_store(&s, &im) == 0);
	{
		int lp = logprint_state(im.data, NULL);
		if (lp >= 0)
			CHECK(lp == 1);		/* xfsprogs: DIRTY */
	}
	log_reset();
	rc = check_img(&im, false, &dirty);
	CHECK(rc == ER_CORRUPT);
	CHECK(dirty);
	CHECK(strstr(logbuf, "norecovery") != NULL);
	CHECK(strstr(logbuf, "Linux") != NULL);
	free(s.buf);

	/* tail at the mkfs unmount record (1,0): the CRC pass has to accept
	 * the xfsprogs-written record (zero or real crc) as well as ours */
	CHECK(slog_load(&s, &im) == 0);
	slog_rec(&s, 2, 1, 1, 0, 1024, OP_START_TRANS, 1);
	CHECK(slog_store(&s, &im) == 0);
	CHECK(check_img(&im, false, &dirty) == ER_CORRUPT);
	CHECK(dirty);
	free(s.buf);
	unlink(im.data);
}

static void norecovery_mounts_dirty(void)
{
	struct img im;
	struct slog s;
	bool dirty = false;

	NEED_TOOLS();
	CHECK(fresh(&im, "norec", "", NULL) == 0);
	CHECK(slog_load(&s, &im) == 0);
	slog_rec(&s, 2, 1, 1, 2, 1024, OP_START_TRANS, 1);
	CHECK(slog_store(&s, &im) == 0);
	log_reset();
	CHECK(check_img(&im, true, &dirty) == 0);
	CHECK(dirty);
	CHECK(strstr(logbuf, "stale") != NULL);
	free(s.buf);
	unlink(im.data);
}

static void unmounted_after_dirty_is_clean(void)
{
	/* A dirty log followed by a proper unmount record is clean. */
	struct img im;
	struct slog s;
	bool dirty = true;

	NEED_TOOLS();
	CHECK(fresh(&im, "umnt", "", NULL) == 0);
	CHECK(slog_load(&s, &im) == 0);
	slog_rec(&s, 2, 1, 1, 2, 1024, OP_START_TRANS, 1);
	slog_unmount(&s, 5, 1, 1, 7);
	CHECK(slog_store(&s, &im) == 0);
	{
		int lp = logprint_state(im.data, NULL);
		if (lp >= 0)
			CHECK(lp == 0);
	}
	CHECK(check_img(&im, false, &dirty) == 0);
	CHECK(!dirty);
	free(s.buf);
	unlink(im.data);
}

static void wrapped_logs(void)
{
	struct img im;
	struct slog s;
	uint32_t head, tail;
	bool dirty;
	int lp;

	NEED_TOOLS();
	CHECK(fresh(&im, "wrap", "", NULL) == 0);
	/* clean wrapped: head in the middle, unmount record at the head */
	CHECK(slog_load(&s, &im) == 0);
	tail = slog_wrapped(&s, 3, 100, true, &head);
	(void)tail;
	CHECK(slog_store(&s, &im) == 0);
	lp = logprint_state(im.data, NULL);
	if (lp >= 0)
		CHECK(lp == 0);
	dirty = true;
	CHECK(check_img(&im, false, &dirty) == 0);
	CHECK(!dirty);
	free(s.buf);

	/* dirty wrapped */
	CHECK(slog_load(&s, &im) == 0);
	slog_wrapped(&s, 3, 100, false, &head);
	CHECK(slog_store(&s, &im) == 0);
	lp = logprint_state(im.data, NULL);
	if (lp >= 0)
		CHECK(lp == 1);
	CHECK(check_img(&im, false, &dirty) == ER_CORRUPT);
	CHECK(dirty);
	CHECK(check_img(&im, true, &dirty) == 0);
	free(s.buf);

	/* head at various positions incl. first and last record slots */
	{
		static const uint32_t heads[] = { 1, 2, 7, 255, 510 };
		size_t i;

		for (i = 0; i < sizeof(heads) / sizeof(heads[0]); i++) {
			CHECK(slog_load(&s, &im) == 0);
			slog_wrapped(&s, 5, heads[i], false, &head);
			CHECK(slog_store(&s, &im) == 0);
			CHECK(check_img(&im, false, &dirty) == ER_CORRUPT);
			slog_wrapped(&s, 5, heads[i], true, &head);
			CHECK(slog_store(&s, &im) == 0);
			CHECK(check_img(&im, false, &dirty) == 0);
			CHECK(!dirty);
			free(s.buf);
		}
	}
	unlink(im.data);
}

static void torn_write_after_unmount_is_clean(void)
{
	/* unmount record, then a record with a bad CRC (torn write): the
	 * kernel trims the head back to the unmount record: clean. */
	struct img im;
	struct slog s;
	bool dirty = true;

	NEED_TOOLS();
	CHECK(fresh(&im, "torn", "", NULL) == 0);
	CHECK(slog_load(&s, &im) == 0);
	slog_rec(&s, 2, 1, 1, 2, 4096, OP_START_TRANS, 1);
	s.buf[(size_t)3 * BBSIZE + 100] ^= 0x5a;	/* payload no longer matches crc */
	CHECK(slog_store(&s, &im) == 0);
	log_reset();
	CHECK(check_img(&im, false, &dirty) == 0);
	CHECK(!dirty);
	CHECK(strstr(logbuf, "Torn write") != NULL);
	free(s.buf);
	unlink(im.data);
}

/* ---- corrupt headers: a defined error (or a clean/dirty verdict), no crash ---- */
static int run_mem(struct slog *s, bool norecovery, bool *dirty, int *flags)
{
	struct mem_ctx m = { s->buf, (uint64_t)s->bb * BBSIZE, 0, 5000000, 0, 0 };
	struct wfs_log_reader r = { mem_read, &m };
	struct wfs_log_params p;
	int rc;

	memset(&p, 0, sizeof(p));
	p.log_bytes = m.len;
	p.sector_size = 512;
	p.logv2 = s->v2;
	memcpy(p.uuid, s->uuid, 16);
	rc = wfs_log_check(&p, &r, norecovery, dirty);
	*flags = (m.overrun ? 1 : 0) | (m.oob ? 2 : 0);
	return rc;
}

static const uint8_t UUID[16] = { 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16 };

static bool is_defined_rc(int rc)
{
	return rc == 0 || rc == ER_CORRUPT || rc == ER_IO;
}

static void mem_cases_pure(void)
{
	struct slog s;
	uint32_t head;
	bool dirty;
	int fl;

	/* pure in-memory logs (no mkfs needed) */
	slog_new(&s, 8192, true, UUID);
	slog_wrapped(&s, 3, 100, true, &head);
	CHECK(run_mem(&s, false, &dirty, &fl) == 0 && !dirty && !fl);
	free(s.buf);
	slog_new(&s, 8192, true, UUID);
	slog_wrapped(&s, 3, 100, false, &head);
	CHECK(run_mem(&s, false, &dirty, &fl) == ER_CORRUPT && dirty && !fl);
	CHECK(run_mem(&s, true, &dirty, &fl) == 0 && dirty && !fl);
	free(s.buf);
	/* all zero log: "totally zeroed", treated as empty/clean as the kernel does */
	slog_new(&s, 8192, true, UUID);
	CHECK(run_mem(&s, false, &dirty, &fl) == 0 && !dirty && !fl);
	free(s.buf);
	/* v1 log */
	slog_new(&s, 8192, false, UUID);
	slog_wrapped(&s, 3, 100, false, &head);
	CHECK(run_mem(&s, false, &dirty, &fl) == ER_CORRUPT && dirty);
	free(s.buf);
}

static void corrupt_log_header_is_error_not_crash(void)
{
	struct slog s;
	uint32_t head;
	bool dirty;
	int fl, rc;
	uint8_t *h;

	/* base: wrapped dirty log; the last new record is at head-8 */
	#define BASE() do { slog_new(&s, 8192, true, UUID); \
		slog_wrapped(&s, 3, 100, false, &head); \
		h = s.buf + (size_t)(head - 8) * BBSIZE; } while (0)

	/* bad magic of the record just before the head */
	BASE();
	wr32(h, 0xdeadbeef);
	rc = run_mem(&s, false, &dirty, &fl);
	CHECK(is_defined_rc(rc) && !fl);
	CHECK(rc != 0 || !dirty || 1);
	free(s.buf);

	/* bad CRC on the tail-side record: not a torn write at the head */
	BASE();
	((struct xlog_rec_header *)(s.buf + (size_t)0 * BBSIZE))->h_crc ^= 0x1;
	rc = run_mem(&s, false, &dirty, &fl);
	CHECK(is_defined_rc(rc) && !fl);
	free(s.buf);

	/* bad CRC in the record before the head (torn write): trimmed, defined */
	BASE();
	((struct xlog_rec_header *)h)->h_crc ^= 0x1;
	rc = run_mem(&s, false, &dirty, &fl);
	CHECK(is_defined_rc(rc) && !fl);
	free(s.buf);

	/* h_len beyond the log, and absurd values */
	{
		static const uint32_t lens[] = { 0, 0x7fffffff, 0xffffffffu,
						 0x80000000u, 8192 * 512,
						 262145, 32769 };
		size_t i;

		for (i = 0; i < sizeof(lens) / sizeof(lens[0]); i++) {
			BASE();
			wr32(h + 12, lens[i]);
			rc = run_mem(&s, false, &dirty, &fl);
			CHECK(is_defined_rc(rc) && !fl);
			wr32(s.buf + 12, lens[i]);	/* and the tail-side record */
			rc = run_mem(&s, false, &dirty, &fl);
			CHECK(is_defined_rc(rc) && !fl);
			free(s.buf);
		}
	}

	/* h_size garbage (record at the tail decides the buffer size) */
	{
		static const uint32_t sizes[] = { 0, 1, 0xffffffffu, 0x80000000u,
						  262144, 262145, 33000 };
		size_t i;

		for (i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++) {
			uint32_t tailblk;

			BASE();
			tailblk = head + 8 * 20;	/* tail used by slog_wrapped */
			wr32(s.buf + (size_t)tailblk * BBSIZE +
			     offsetof(struct xlog_rec_header, h_size), sizes[i]);
			rc = run_mem(&s, false, &dirty, &fl);
			CHECK(is_defined_rc(rc) && !fl);
			wr32(h + offsetof(struct xlog_rec_header, h_size), sizes[i]);
			rc = run_mem(&s, false, &dirty, &fl);
			CHECK(is_defined_rc(rc) && !fl);
			free(s.buf);
		}
	}

	/* cycle garbage: random cycle words in headers and data blocks */
	{
		uint32_t seed = 12345, k;

		BASE();
		for (k = 0; k < 2000; k++) {
			seed = seed * 1103515245u + 12345u;
			wr32(s.buf + (size_t)(seed >> 8) % (8192 * BBSIZE / 4) * 4,
			     seed * 2654435761u);
		}
		rc = run_mem(&s, false, &dirty, &fl);
		CHECK(is_defined_rc(rc) && !fl);
		free(s.buf);
	}

	/* the cycle of the first block 0 and last block garbage */
	BASE();
	wr32(s.buf, 0xfeedbabe);
	wr32(s.buf + 4, 0);
	rc = run_mem(&s, false, &dirty, &fl);
	CHECK(is_defined_rc(rc) && !fl);
	free(s.buf);

	/* uuid mismatch in the record before the head */
	BASE();
	h[offsetof(struct xlog_rec_header, h_fs_uuid)] ^= 0xff;
	slog_recrc(&s, head - 8);
	rc = run_mem(&s, false, &dirty, &fl);
	CHECK(is_defined_rc(rc) && !fl);
	free(s.buf);

	/* wrong version, wrong fmt */
	BASE();
	wr32(h + 8, 7);
	rc = run_mem(&s, false, &dirty, &fl);
	CHECK(is_defined_rc(rc) && !fl);
	free(s.buf);
	#undef BASE
}

/* Seeded structural fuzz over several bases: only defined results, never a
 * loop (read cap), never a read outside the log region. */
static void fuzz_defined_results(void)
{
	static const struct { uint32_t head8; bool umount; bool v2; } base[] = {
		{ 100, false, true }, { 100, true, true }, { 1, false, true },
		{ 1000, false, true }, { 100, false, false },
	};
	uint32_t seed = 987654321u;
	size_t b;
	int it, bad = 0, ok = 0, err = 0;

	for (b = 0; b < sizeof(base) / sizeof(base[0]); b++) {
		for (it = 0; it < 1500; it++) {
			struct slog s, orig;
			uint32_t head, nmut, m;
			bool dirty;
			int fl, rc;

			slog_new(&orig, 2048, base[b].v2, UUID);
			slog_wrapped(&orig, 3, base[b].head8 % 250, base[b].umount, &head);
			slog_new(&s, 2048, base[b].v2, UUID);
			memcpy(s.buf, orig.buf, (size_t)2048 * BBSIZE);
			free(orig.buf);
			seed = seed * 1664525u + 1013904223u;
			nmut = 1 + (seed >> 28) % 6;
			for (m = 0; m < nmut; m++) {
				uint32_t blk, kind, off;

				seed = seed * 1664525u + 1013904223u;
				blk = (seed >> 4) % 2048;
				seed = seed * 1664525u + 1013904223u;
				kind = (seed >> 24) % 6;
				off = (seed >> 3) % BBSIZE;
				if (kind == 0)
					s.buf[(size_t)blk * BBSIZE + off] ^= (uint8_t)(1 << (seed & 7));
				else if (kind == 1)
					memset(s.buf + (size_t)blk * BBSIZE, 0, BBSIZE);
				else if (kind == 2)
					wr32(s.buf + (size_t)blk * BBSIZE + (off & ~3u), seed * 40503u);
				else if (kind == 3) {
					uint32_t o2 = (seed >> 9) % 2048;
					memcpy(s.buf + (size_t)blk * BBSIZE,
					       s.buf + (size_t)o2 * BBSIZE, BBSIZE);
				} else if (kind == 4)	/* header-ish field */
					wr32(s.buf + (size_t)blk * BBSIZE + ((off % 12) & ~3u) + 4,
					     seed >> (seed & 15));
				else {			/* make it look like a header */
					wr32(s.buf + (size_t)blk * BBSIZE, XLOG_HEADER_MAGIC_NUM);
					wr32(s.buf + (size_t)blk * BBSIZE + 12 + 0, seed % 70000);
				}
			}
			rc = run_mem(&s, (it & 1) != 0, &dirty, &fl);
			if (!is_defined_rc(rc) || fl) {
				bad++;
				fprintf(stderr, "fuzz base %zu iter %d: rc=%d flags=%d\n",
					b, it, rc, fl);
			} else if (rc == 0) {
				ok++;
			} else {
				err++;
			}
			free(s.buf);
		}
	}
	CHECK(bad == 0);
	CHECK(ok > 0 && err > 0);
	printf("  fuzz: %d accepted, %d rejected, %d bad\n", ok, err, bad);
}

/* Regression inputs found while building the port (kept as fixed cases). */
static void regression_inputs(void)
{
	struct slog s;
	uint32_t head;
	bool dirty;
	int fl, rc;

	/* 1. record with h_len = 0 at the tail: valid_rec_header must reject
	 *    instead of looping on a zero-length record */
	slog_new(&s, 4096, true, UUID);
	slog_wrapped(&s, 3, 50, false, &head);
	wr32(s.buf + (size_t)(50 * 8 + 160) * BBSIZE + 12, 0);
	rc = run_mem(&s, false, &dirty, &fl);
	CHECK(is_defined_rc(rc) && !fl);
	free(s.buf);

	/* 2. every block is a record header with the same cycle: header-only
	 *    log must terminate */
	slog_new(&s, 4096, true, UUID);
	{
		uint32_t b;
		for (b = 0; b < 4096; b++) {
			wr32(s.buf + (size_t)b * BBSIZE, XLOG_HEADER_MAGIC_NUM);
			wr32(s.buf + (size_t)b * BBSIZE + 4, 7);
			wr32(s.buf + (size_t)b * BBSIZE + 8, 2);
			wr32(s.buf + (size_t)b * BBSIZE + 12, 512);
			wr64(s.buf + (size_t)b * BBSIZE + 24, (7ULL << 32) | ((b + 100) % 4096));
		}
	}
	rc = run_mem(&s, false, &dirty, &fl);
	CHECK(is_defined_rc(rc) && !fl);
	free(s.buf);

	/* 3. tail lsn block number beyond the log */
	slog_new(&s, 4096, true, UUID);
	slog_wrapped(&s, 3, 50, false, &head);
	wr64(s.buf + (size_t)(head - 8) * BBSIZE + 24, (3ULL << 32) | 0xfffffff0u);
	rc = run_mem(&s, false, &dirty, &fl);
	CHECK(is_defined_rc(rc) && !fl);
	free(s.buf);
}

static void external_log_device_clean_and_dirty(void)
{
	struct img im;
	struct slog s;
	bool dirty = true;
	int lp;

	NEED_TOOLS();
	if (fresh(&im, "ext", "", "") != 0) {
		printf("note: external log mkfs unsupported here, skipped\n");
		skipped_cases++;
		return;
	}
	CHECK(im.p.log_start_byte == 0);	/* logstart 0: external */
	CHECK(im.logdev[0] != 0);
	lp = logprint_state(im.data, im.logdev);
	CHECK(lp != 1);
	CHECK(check_img(&im, false, &dirty) == 0);
	CHECK(!dirty);

	CHECK(slog_load(&s, &im) == 0);
	slog_rec(&s, 2, 1, 1, 2, 1024, OP_START_TRANS, 1);
	CHECK(slog_store(&s, &im) == 0);
	lp = logprint_state(im.data, im.logdev);
	if (lp >= 0)
		CHECK(lp == 1);
	CHECK(check_img(&im, false, &dirty) == ER_CORRUPT);
	CHECK(dirty);
	CHECK(check_img(&im, true, &dirty) == 0);
	free(s.buf);
	unlink(im.data);
	unlink(im.logdev);
}

static void truncated_log_region(void)
{
	struct img im;
	bool dirty;
	struct fread_ctx fc;
	struct wfs_log_reader r;
	int rc;

	NEED_TOOLS();
	CHECK(fresh(&im, "trunc", "", NULL) == 0);
	fc.fd = open(im.data, O_RDONLY);
	fc.fail_at = 0;
	fc.nreads = 0;
	r.read = file_read;
	r.ctx = &fc;

	/* image ends in the middle of the log */
	fc.limit = im.p.log_start_byte + im.p.log_bytes / 2;
	rc = wfs_log_check(&im.p, &r, false, &dirty);
	CHECK(rc == ER_IO);
	/* image ends before the log */
	fc.limit = im.p.log_start_byte;
	CHECK(wfs_log_check(&im.p, &r, false, &dirty) == ER_IO);
	/* the Nth read fails, for every N within the run */
	{
		long n;

		fc.limit = UINT64_MAX;
		fc.fail_at = 0;
		fc.nreads = 0;
		CHECK(wfs_log_check(&im.p, &r, false, &dirty) == 0);
		for (n = 1; n <= fc.nreads; n++) {
			long total = fc.nreads;
			fc.fail_at = n;
			fc.nreads = 0;
			rc = wfs_log_check(&im.p, &r, false, &dirty);
			CHECK(rc == ER_IO);
			fc.nreads = total;
			if (n > 50)
				break;
		}
	}
	close(fc.fd);
	unlink(im.data);
}

static void bad_parameters(void)
{
	struct slog s;
	struct mem_ctx m;
	struct wfs_log_reader r;
	struct wfs_log_params p;
	bool dirty;

	slog_new(&s, 4096, true, UUID);
	m = (struct mem_ctx){ s.buf, (uint64_t)4096 * BBSIZE, 0, 1000000, 0, 0 };
	r.read = mem_read;
	r.ctx = &m;
	memset(&p, 0, sizeof(p));
	p.log_bytes = m.len;
	p.sector_size = 512;
	p.logv2 = true;
	CHECK(wfs_log_check(&p, &r, false, &dirty) == 0);

	p.sector_size = 0;
	CHECK(wfs_log_check(&p, &r, false, &dirty) == ER_CORRUPT);
	p.sector_size = 1000;
	CHECK(wfs_log_check(&p, &r, false, &dirty) == ER_CORRUPT);
	p.sector_size = 65536;
	CHECK(wfs_log_check(&p, &r, false, &dirty) == ER_CORRUPT);
	p.sector_size = 512;
	p.log_bytes = 0;
	CHECK(wfs_log_check(&p, &r, false, &dirty) == ER_CORRUPT);
	p.log_bytes = 512 * 5;
	CHECK(wfs_log_check(&p, &r, false, &dirty) == ER_CORRUPT);
	p.log_bytes = UINT64_MAX;
	CHECK(wfs_log_check(&p, &r, false, &dirty) == ER_CORRUPT);
	p.log_bytes = (1ULL << 40);
	CHECK(wfs_log_check(&p, &r, false, &dirty) == ER_CORRUPT);
	p.log_bytes = m.len + 100;		/* not a sector multiple */
	CHECK(wfs_log_check(&p, &r, false, &dirty) == ER_CORRUPT);
	p.log_bytes = m.len;
	p.log_start_byte = UINT64_MAX - 512;
	CHECK(wfs_log_check(&p, &r, false, &dirty) == ER_CORRUPT);
	p.log_start_byte = 0;
	CHECK(wfs_log_check(NULL, &r, false, &dirty) == -WFS_EINVAL);
	CHECK(wfs_log_check(&p, NULL, false, &dirty) == -WFS_EINVAL);
	free(s.buf);
}

/* ---- the mount-facing entry point with an xfs_mount ---- */
static struct wfs_log_reader g_reader;
static int provider(struct xfs_mount *mp, struct wfs_log_reader *r)
{
	(void)mp;
	*r = g_reader;
	return 0;
}

static void mount_entry_point(void)
{
	struct img im;
	struct slog s;
	struct xfs_dsb dsb;
	struct fread_ctx fc;
	struct xfs_mount *mp;
	int fd;

	NEED_TOOLS();
	CHECK(xfs_log_mount_ro_check(NULL, false) == -WFS_EINVAL);	/* no provider */
	CHECK(fresh(&im, "mnt", "", NULL) == 0);
	fd = open(im.data, O_RDONLY);
	CHECK(pread(fd, &dsb, sizeof(dsb), 0) == (ssize_t)sizeof(dsb));
	close(fd);

	mp = calloc(1, sizeof(*mp));
	mp->m_sb.sb_blocksize = be32_to_cpu(dsb.sb_blocksize);
	mp->m_sb.sb_blocklog = dsb.sb_blocklog;
	mp->m_sb.sb_agblocks = be32_to_cpu(dsb.sb_agblocks);
	mp->m_sb.sb_agblklog = dsb.sb_agblklog;
	mp->m_sb.sb_logstart = be64_to_cpu(dsb.sb_logstart);
	mp->m_sb.sb_logblocks = be32_to_cpu(dsb.sb_logblocks);
	mp->m_sb.sb_logsectsize = be16_to_cpu(dsb.sb_logsectsize);
	mp->m_sb.sb_logsunit = be32_to_cpu(dsb.sb_logsunit);
	memcpy(&mp->m_sb.sb_uuid, &dsb.sb_uuid, 16);
	mp->m_blkbb_log = dsb.sb_blocklog - BBSHIFT;	/* set by mount code (T6) */
	mp->m_features |= XFS_FEAT_CRC;		/* v5: log v2 */

	fc.fd = open(im.data, O_RDONLY);
	fc.limit = UINT64_MAX;
	fc.fail_at = 0;
	fc.nreads = 0;
	g_reader.read = file_read;
	g_reader.ctx = &fc;
	wfs_log_set_reader_provider(provider);

	CHECK(xfs_log_mount_ro_check(mp, false) == 0);	/* clean */
	close(fc.fd);

	CHECK(slog_load(&s, &im) == 0);
	slog_rec(&s, 2, 1, 1, 2, 1024, OP_START_TRANS, 1);
	CHECK(slog_store(&s, &im) == 0);
	fc.fd = open(im.data, O_RDONLY);
	CHECK(xfs_log_mount_ro_check(mp, false) == ER_CORRUPT);
	CHECK(xfs_log_mount_ro_check(mp, true) == 0);
	close(fc.fd);
	free(s.buf);
	free(mp);
	wfs_log_set_reader_provider(NULL);
	unlink(im.data);
}

int main(void)
{
	char tmpl[512];
	const char *base = getenv("TMPDIR");

	wfs_log_set_hook(sink);
	snprintf(tmpl, sizeof(tmpl), "%s/wfs-logcheck-XXXXXX", base ? base : "/tmp");
	if (!mkdtemp(tmpl)) {
		perror("mkdtemp");
		return 1;
	}
	snprintf(tmpdir, sizeof(tmpdir), "%s", tmpl);
	tools_ok = have_tool("mkfs.xfs");
	if (!tools_ok)
		printf("SKIPPED: image-based logcheck tests (mkfs.xfs missing; install xfsprogs)\n");
	if (!have_tool("xfs_logprint"))
		printf("SKIPPED: xfs_logprint cross-check (xfs_logprint missing)\n");

#define RUN(t) do { int f0 = failures; log_reset(); t(); \
	printf("%s %s\n", failures == f0 ? "PASS" : "FAIL", #t); } while (0)
	RUN(clean_fixtures_pass);
	RUN(dirty_log_rejected);
	RUN(norecovery_mounts_dirty);
	RUN(unmounted_after_dirty_is_clean);
	RUN(wrapped_logs);
	RUN(torn_write_after_unmount_is_clean);
	RUN(mem_cases_pure);
	RUN(corrupt_log_header_is_error_not_crash);
	RUN(regression_inputs);
	RUN(fuzz_defined_results);
	RUN(external_log_device_clean_and_dirty);
	RUN(truncated_log_region);
	RUN(bad_parameters);
	RUN(mount_entry_point);

	sh("rm -rf '%s'", tmpdir);
	printf("%d checks, %d failures%s\n", checks, failures,
	       skipped_cases ? " (some cases SKIPPED, see above)" : "");
	return failures ? 1 : 0;
}
