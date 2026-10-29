// SPDX-License-Identifier: GPL-2.0-only
/* Buffer cache tests over a file-backed fake host (synthetic blocks). */
#include "buf.h"
#include "xfs_format.h"
#include "xfs_log_format.h"
#include "xfs_shared.h"
#include "xfs_trans_resv.h"
#include "xfs_mount.h"
#include "xfs_error.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <time.h>

static int failures, checks;
static pthread_mutex_t chk_lock = PTHREAD_MUTEX_INITIALIZER;
#define CHECK(c) do { pthread_mutex_lock(&chk_lock); checks++; if (!(c)) { failures++; \
	fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #c); } \
	pthread_mutex_unlock(&chk_lock); } while (0)

#define IMG_BYTES	(32u << 20)
#define BAD_DADDR	800		/* first byte 0xFF: verifier rejects */

/* ---- fake host --------------------------------------------------------- */
static int img_fd;
static int reads;		/* atomic: blk_read calls */
static int fail_reads;		/* atomic flag: make reads fail */
static int read_delay_us;

static unsigned char pat(uint64_t off)
{
	return (unsigned char)(((off * 31) ^ (off >> 9)) & 0x7f);
}

static int fake_read(void *ctx, int role, uint64_t off, void *buf, size_t len)
{
	(void)ctx; (void)role;
	__atomic_add_fetch(&reads, 1, __ATOMIC_SEQ_CST);
	if (read_delay_us)
		usleep((useconds_t)read_delay_us);
	if (__atomic_load_n(&fail_reads, __ATOMIC_SEQ_CST))
		return -WFS_EIO;
	return pread(img_fd, buf, len, (off_t)off) == (ssize_t)len ? 0 : -WFS_EIO;
}

static void make_image(const char *path)
{
	unsigned char *m = malloc(IMG_BYTES);
	uint64_t i;
	FILE *f;

	for (i = 0; i < IMG_BYTES; i++)
		m[i] = pat(i);
	m[(uint64_t)BAD_DADDR * 512] = 0xFF;
	f = fopen(path, "wb");
	fwrite(m, 1, IMG_BYTES, f);
	fclose(f);
	free(m);
	img_fd = open(path, O_RDONLY);
}

/* ---- fixture ------------------------------------------------------------- */
static struct wfs_xfs_blk blk;
static struct xfs_mount mp;
static struct xfs_buftarg bt;
static struct wfs_xfs_bcache *cache;
static int verify_calls;

static void verify_read(struct xfs_buf *bp)
{
	__atomic_add_fetch(&verify_calls, 1, __ATOMIC_SEQ_CST);
	if (((unsigned char *)bp->b_addr)[0] == 0xFF)
		bp->b_error = -EFSCORRUPTED;
}
static const struct xfs_buf_ops test_ops = { .name = "test", .verify_read = verify_read };
static void verify_crc(struct xfs_buf *bp) { bp->b_error = -EFSBADCRC; }
static const struct xfs_buf_ops crc_ops = { .name = "crc", .verify_read = verify_crc };

static void setup(size_t limit)
{
	memset(&mp, 0, sizeof(mp));
	memset(&bt, 0, sizeof(bt));
	memset(&blk, 0, sizeof(blk));
	blk.read = fake_read;
	blk.size_bytes[WFS_DEV_DATA] = IMG_BYTES;
	blk.sector_size[WFS_DEV_DATA] = 512;
	cache = wfs_xfs_bcache_create(&blk, limit);
	CHECK(cache != NULL);
	bt.bt_mount = &mp;
	CHECK(wfs_xfs_buftarg_bind(&bt, cache, WFS_DEV_DATA) == 0);
	reads = 0; verify_calls = 0; fail_reads = 0; read_delay_us = 0;
	wfs_mem_set_cap(0);
}

static void teardown(void)
{
	wfs_xfs_bcache_destroy(cache);
	wfs_xfs_buftarg_unbind(&bt);
	CHECK(wfs_mem_used() == 0);
}

static int data_ok(struct xfs_buf *bp, uint64_t daddr, size_t bytes)
{
	size_t i;
	const unsigned char *p = bp->b_addr;

	for (i = 0; i < bytes; i++) {
		if (daddr * 512 + i == (uint64_t)BAD_DADDR * 512)
			continue;
		if (p[i] != pat(daddr * 512 + i))
			return 0;
	}
	return 1;
}

/* ---- tests --------------------------------------------------------------- */
static void hit_does_not_read_again(void)
{
	struct xfs_buf *a, *b;

	setup(WFS_XFS_BUFCACHE_DEFAULT_BYTES);
	CHECK(xfs_buf_read(&bt, 8, 8, 0, &a, &test_ops) == 0);
	CHECK(data_ok(a, 8, 4096));
	CHECK(xfs_buf_daddr(a) == 8 && a->b_length == 8);
	xfs_buf_relse(a);
	CHECK(xfs_buf_read(&bt, 8, 8, 0, &b, &test_ops) == 0);
	CHECK(b == a);
	CHECK(data_ok(b, 8, 4096));
	xfs_buf_relse(b);
	CHECK(reads == 1 && verify_calls == 1);
	/* other length at the same daddr is a different buffer */
	CHECK(xfs_buf_read(&bt, 8, 16, 0, &b, &test_ops) == 0);
	CHECK(b != a && reads == 2);
	xfs_buf_relse(b);
	teardown();
}

static void verifier_failure_not_cached(void)
{
	struct xfs_buf *a = (void *)1;

	setup(WFS_XFS_BUFCACHE_DEFAULT_BYTES);
	CHECK(xfs_buf_read(&bt, BAD_DADDR, 8, 0, &a, &test_ops) == -EFSCORRUPTED);
	CHECK(a == NULL);
	CHECK(wfs_xfs_bcache_usage(cache) == 0);
	CHECK(xfs_buf_read(&bt, BAD_DADDR, 8, 0, &a, &test_ops) == -EFSCORRUPTED);
	CHECK(reads == 2 && verify_calls == 2);
	CHECK(xfs_buf_incore(&bt, BAD_DADDR, 8, 0, &a) == -ENOENT);
	CHECK(xfs_buf_read(&bt, 16, 8, 0, &a, &crc_ops) == -EFSBADCRC);
	CHECK(wfs_xfs_bcache_usage(cache) == 0);
	teardown();
}

static void eviction_respects_limit(void)
{
	struct xfs_buf *pin[4], *bp;
	int i, reads_before;
	size_t lim = 8u << 20;

	setup(lim);
	for (i = 0; i < 4; i++)	/* pinned 64 KiB buffers at the start */
		CHECK(xfs_buf_read(&bt, i * 128, 128, 0, &pin[i], &test_ops) == 0);
	for (i = 4; i < 400; i++) {	/* 24.75 MiB of 64 KiB buffers */
		CHECK(xfs_buf_read(&bt, i * 128, 128, 0, &bp, &test_ops) == 0);
		CHECK(data_ok(bp, (uint64_t)i * 128, 65536));
		xfs_buf_relse(bp);
		CHECK(wfs_xfs_bcache_usage(cache) <= lim);
	}
	reads_before = reads;
	for (i = 0; i < 4; i++) {	/* pinned: still valid and cached */
		CHECK(data_ok(pin[i], (uint64_t)i * 128, 65536));
		CHECK(xfs_buf_read(&bt, i * 128, 128, 0, &bp, &test_ops) == 0 && bp == pin[i]);
		xfs_buf_relse(bp);
	}
	CHECK(reads == reads_before);
	/* shrink below the pinned size: pinned stay, everything else goes */
	wfs_xfs_bcache_set_limit(cache, 65536);
	CHECK(wfs_xfs_bcache_usage(cache) == 4u * 65536);
	CHECK(data_ok(pin[3], 3 * 128, 65536));
	for (i = 0; i < 4; i++)
		xfs_buf_relse(pin[i]);
	CHECK(wfs_xfs_bcache_usage(cache) <= 65536);
	teardown();
}

static void release_makes_evictable(void)
{
	struct xfs_buf *a, *b;

	setup(65536);
	CHECK(xfs_buf_read(&bt, 0, 128, 0, &a, &test_ops) == 0);
	CHECK(xfs_buf_read(&bt, 128, 128, 0, &b, &test_ops) == 0);
	CHECK(wfs_xfs_bcache_usage(cache) == 2 * 65536);	/* both pinned: soft limit */
	xfs_buf_relse(a);
	CHECK(wfs_xfs_bcache_usage(cache) == 65536);		/* a evictable, gone */
	xfs_buf_relse(b);
	CHECK(wfs_xfs_bcache_usage(cache) <= 65536);
	xfs_buf_hold(b);	/* not legal use after rele; only checks the hold path */
	xfs_buf_rele(b);
	wfs_xfs_bcache_purge(cache);
	CHECK(wfs_xfs_bcache_usage(cache) == 0);
	teardown();
}

#define NTHR 16
static pthread_barrier_t bar;
static struct xfs_buf *thr_bp[NTHR];
static int thr_err[NTHR];

static void *same_block_thread(void *arg)
{
	long i = (long)arg;

	pthread_barrier_wait(&bar);
	thr_err[i] = xfs_buf_read(&bt, 4096, 8, 0, &thr_bp[i], &test_ops);
	return NULL;
}

static void concurrent_same_block_single_read(void)
{
	pthread_t t[NTHR];
	long i;

	setup(WFS_XFS_BUFCACHE_DEFAULT_BYTES);
	read_delay_us = 100000;
	pthread_barrier_init(&bar, NULL, NTHR);
	for (i = 0; i < NTHR; i++)
		pthread_create(&t[i], NULL, same_block_thread, (void *)i);
	for (i = 0; i < NTHR; i++)
		pthread_join(t[i], NULL);
	CHECK(reads == 1 && verify_calls == 1);
	for (i = 0; i < NTHR; i++) {
		CHECK(thr_err[i] == 0 && thr_bp[i] == thr_bp[0]);
		if (!thr_err[i]) {
			CHECK(data_ok(thr_bp[i], 4096, 4096));
			xfs_buf_relse(thr_bp[i]);
		}
	}
	pthread_barrier_destroy(&bar);
	teardown();
}

static void read_error_maps_to_eio(void)
{
	struct xfs_buf *a;

	setup(WFS_XFS_BUFCACHE_DEFAULT_BYTES);
	fail_reads = 1;
	CHECK(xfs_buf_read(&bt, 64, 8, 0, &a, &test_ops) == -EIO);
	CHECK(verify_calls == 0 && wfs_xfs_bcache_usage(cache) == 0);
	fail_reads = 0;
	CHECK(xfs_buf_read(&bt, 64, 8, 0, &a, &test_ops) == 0);	/* error not cached */
	xfs_buf_relse(a);
	/* past end of device */
	CHECK(xfs_buf_read(&bt, IMG_BYTES / 512 - 4, 8, 0, &a, &test_ops) == -EFSCORRUPTED);
	teardown();
}

static void discontiguous_map_reads_all_segments(void)
{
	struct xfs_buf_map m[3] = { { .bm_bn = 100, .bm_len = 8 },
				    { .bm_bn = 300, .bm_len = 8 },
				    { .bm_bn = 50, .bm_len = 16 } };
	struct xfs_buf *a, *b;
	char *p;

	setup(WFS_XFS_BUFCACHE_DEFAULT_BYTES);
	CHECK(xfs_buf_read_map(&bt, m, 3, 0, &a, &test_ops, NULL) == 0);
	CHECK(a->b_length == 32 && a->b_map_count == 3 && reads == 3);
	CHECK(verify_calls == 1);
	p = a->b_addr;
	CHECK(pat(100 * 512) == (unsigned char)p[0] && pat(300 * 512 + 7) == (unsigned char)p[4096 + 7]);
	CHECK(pat(50 * 512 + 8191) == (unsigned char)p[8192 + 8191]);
	CHECK(xfs_buf_daddr(a) == 100);
	CHECK(xfs_buf_offset(a, 4096) == p + 4096);
	xfs_buf_relse(a);
	CHECK(xfs_buf_read_map(&bt, m, 3, 0, &b, &test_ops, NULL) == 0 && b == a && reads == 3);
	xfs_buf_relse(b);
	/* the contiguous buffer with the same first daddr and length is distinct */
	CHECK(xfs_buf_read(&bt, 100, 32, 0, &b, &test_ops) == 0 && b != a);
	xfs_buf_relse(b);
	teardown();
}

static void cap_exceeded_returns_enomem(void)
{
	struct xfs_buf *a;

	setup(WFS_XFS_BUFCACHE_DEFAULT_BYTES);
	wfs_mem_set_cap(wfs_mem_used() + 100);
	CHECK(xfs_buf_read(&bt, 8, 8, 0, &a, &test_ops) == -ENOMEM);
	CHECK(reads == 0);
	wfs_mem_set_cap(0);
	CHECK(xfs_buf_read(&bt, 8, 8, 0, &a, &test_ops) == 0);
	xfs_buf_relse(a);
	teardown();
}

static void get_uncached_and_incore(void)
{
	struct xfs_buf *a, *b;

	setup(WFS_XFS_BUFCACHE_DEFAULT_BYTES);
	CHECK(xfs_buf_incore(&bt, 8, 8, 0, &a) == -ENOENT);
	CHECK(xfs_buf_get(&bt, 8, 8, &a) == 0 && reads == 0);
	CHECK(!(a->b_flags & XBF_DONE));
	xfs_buf_relse(a);
	CHECK(xfs_buf_incore(&bt, 8, 8, 0, &b) == 0 && b == a);
	xfs_buf_relse(b);
	CHECK(xfs_buf_read(&bt, 8, 8, 0, &b, &test_ops) == 0 && b == a && reads == 1);
	xfs_buf_relse(b);
	CHECK(xfs_buf_read_uncached(&bt, 40, 1, &a, &test_ops) == 0);
	CHECK(data_ok(a, 40, 512));
	xfs_buf_relse(a);
	CHECK(xfs_buf_get_uncached(&bt, 2, &a) == 0 && a->b_length == 2);
	xfs_buf_relse(a);
	/* re-verify against different ops on a hit */
	CHECK(xfs_buf_read(&bt, 8, 8, 0, &b, &crc_ops) == -EFSBADCRC);
	CHECK(xfs_buf_read(&bt, 8, 8, 0, &b, &test_ops) == 0);
	xfs_buf_relse(b);
	teardown();
}


/* ---- fix round 1 tests ---------------------------------------------------- */
static void vfy_err_badcrc(struct xfs_buf *bp) { xfs_verifier_error(bp, -EFSBADCRC, __this_address); }
static void vfy_err_corrupt(struct xfs_buf *bp) { xfs_verifier_error(bp, -EFSCORRUPTED, __this_address); }
static void vfy_err_pos(struct xfs_buf *bp) { xfs_buf_verifier_error(bp, EFSBADCRC, "t", bp->b_addr, 16, __this_address); }
static const struct xfs_buf_ops ops_badcrc = { .name = "badcrc", .verify_read = vfy_err_badcrc };
static const struct xfs_buf_ops ops_corrupt = { .name = "corrupt", .verify_read = vfy_err_corrupt };
static const struct xfs_buf_ops ops_pos = { .name = "pos", .verify_read = vfy_err_pos };

static void verifier_error_reporting_fails_read(void)
{
	struct xfs_buf *a = (void *)1;

	setup(WFS_XFS_BUFCACHE_DEFAULT_BYTES);
	CHECK(xfs_buf_read(&bt, 8, 8, 0, &a, &ops_badcrc) == -EFSBADCRC && a == NULL);
	CHECK(xfs_buf_read(&bt, 8, 8, 0, &a, &ops_corrupt) == -EFSCORRUPTED);
	CHECK(xfs_buf_read(&bt, 8, 8, 0, &a, &ops_pos) == -EFSBADCRC);
	CHECK(wfs_xfs_bcache_usage(cache) == 0 && reads == 3);
	CHECK(xfs_buf_incore(&bt, 8, 8, 0, &a) == -ENOENT);
	/* reverify on a cached buffer fails the call but keeps the good data */
	CHECK(xfs_buf_read(&bt, 8, 8, 0, &a, &test_ops) == 0);
	xfs_buf_relse(a);
	CHECK(xfs_buf_read(&bt, 8, 8, 0, &a, &ops_badcrc) == -EFSBADCRC);
	CHECK(xfs_buf_read(&bt, 8, 8, 0, &a, &test_ops) == 0 && reads == 4);
	xfs_buf_relse(a);
	teardown();
}

static void slow_fail_verify(struct xfs_buf *bp) { usleep(50000); xfs_verifier_error(bp, -EFSBADCRC, __this_address); }
static void slow_ok_verify(struct xfs_buf *bp) { (void)bp; usleep(50000); }
static const struct xfs_buf_ops ops_slowfail = { .name = "slowfail", .verify_read = slow_fail_verify };
static const struct xfs_buf_ops ops_slowok = { .name = "slowok", .verify_read = slow_ok_verify };

static int mo_err[NTHR];
static void *multi_op_thread(void *arg)
{
	long i = (long)arg;
	struct xfs_buf *bp;

	pthread_barrier_wait(&bar);
	mo_err[i] = xfs_buf_read(&bt, 5000, 8, 0, &bp, (i & 1) ? &ops_slowok : &ops_slowfail);
	if (!mo_err[i]) {
		CHECK(data_ok(bp, 5000, 4096));
		xfs_buf_relse(bp);
	}
	return NULL;
}

static void failed_read_not_inherited_by_other_ops(void)
{
	pthread_t t[NTHR];
	long i;

	setup(WFS_XFS_BUFCACHE_DEFAULT_BYTES);
	pthread_barrier_init(&bar, NULL, NTHR);
	for (i = 0; i < NTHR; i++)
		pthread_create(&t[i], NULL, multi_op_thread, (void *)i);
	for (i = 0; i < NTHR; i++)
		pthread_join(t[i], NULL);
	pthread_barrier_destroy(&bar);
	for (i = 0; i < NTHR; i++)
		CHECK(mo_err[i] == ((i & 1) ? 0 : -EFSBADCRC));
	teardown();
}

static int stop_readers;
static const struct xfs_buf_ops *seen_ops;
static void *ops_reader_thread(void *arg)
{
	struct xfs_buf *bp = arg;
	while (!__atomic_load_n(&stop_readers, __ATOMIC_ACQUIRE)) {
		seen_ops = bp->b_ops;	/* unlocked read, as libxfs does */
		CHECK(bp->b_error == 0);
	}
	return NULL;
}

static void reverify_does_not_write_shared_fields(void)
{
	struct xfs_buf *a, *b;
	pthread_t t;
	int i;

	setup(WFS_XFS_BUFCACHE_DEFAULT_BYTES);
	CHECK(xfs_buf_read(&bt, 8, 8, 0, &a, &test_ops) == 0);
	stop_readers = 0;
	pthread_create(&t, NULL, ops_reader_thread, a);
	for (i = 0; i < 200; i++) {
		CHECK(xfs_buf_read(&bt, 8, 8, 0, &b, (i & 1) ? &ops_slowok : &crc_ops) == ((i & 1) ? 0 : -EFSBADCRC));
		if (i & 1)
			xfs_buf_relse(b);
		if (i > 3)
			break;
	}
	__atomic_store_n(&stop_readers, 1, __ATOMIC_RELEASE);
	pthread_join(t, NULL);
	CHECK(a->b_ops == &test_ops);
	xfs_buf_relse(a);
	teardown();
}

static void fast_ok_verify(struct xfs_buf *bp) { (void)bp; }
static const struct xfs_buf_ops ops_fastok = { .name = "fastok", .verify_read = fast_ok_verify };
#define TNT 12
static void *tiny_thread(void *arg)
{
	unsigned seed = (unsigned)(long)arg * 7919u + 1;
	int it;

	for (it = 0; it < 400; it++) {
		struct xfs_buf *bp[3];
		int k, n = 1 + (int)(rand_r(&seed) % 3);

		for (k = 0; k < n; k++) {
			int blk = (int)(rand_r(&seed) % 48);

			if (blk == 47) {	/* a bad block now and then */
				CHECK(xfs_buf_read(&bt, BAD_DADDR, 8, 0, &bp[k], &test_ops) == -EFSCORRUPTED);
				bp[k] = NULL;
				continue;
			}
			CHECK(xfs_buf_read(&bt, (xfs_daddr_t)blk * 128, 128, 0, &bp[k],
					   (it & 1) ? &test_ops : &ops_fastok) == 0);
			if (bp[k])
				CHECK(data_ok(bp[k], (uint64_t)blk * 128, 65536));
		}
		for (k = 0; k < n; k++)
			if (bp[k])
				xfs_buf_relse(bp[k]);
	}
	return NULL;
}

static void tiny_limit_concurrency(void)
{
	pthread_t t[TNT];
	long i;

	setup(256u << 10);
	for (i = 0; i < TNT; i++)
		pthread_create(&t[i], NULL, tiny_thread, (void *)i);
	for (i = 0; i < TNT; i++)
		pthread_join(t[i], NULL);
	CHECK(wfs_xfs_bcache_usage(cache) <= (256u << 10));
	wfs_xfs_bcache_purge(cache);
	CHECK(wfs_xfs_bcache_usage(cache) == 0);
	teardown();
}

static void range_guards(void)
{
	struct xfs_buf *a;

	setup(WFS_XFS_BUFCACHE_DEFAULT_BYTES);
	CHECK(xfs_buf_get_uncached(&bt, ((size_t)1 << 32) + 8, &a) == -EINVAL && a == NULL);
	CHECK(xfs_buf_read_uncached(&bt, 8, ((size_t)1 << 32) + 8, &a, &test_ops) == -EINVAL);
	CHECK(xfs_buf_get_uncached(&bt, 0, &a) == -EINVAL);
	CHECK(reads == 0);
	/* unknown device size (bt_nr_sectors == 0): offsets must not overflow */
	bt.bt_nr_sectors = 0;
	CHECK(xfs_buf_read(&bt, (xfs_daddr_t)1 << 56, 8, 0, &a, &test_ops) == -EFSCORRUPTED);
	CHECK(xfs_buf_read(&bt, INT64_MAX, 8, 0, &a, &test_ops) == -EFSCORRUPTED);
	CHECK(xfs_buf_read(&bt, -5, 8, 0, &a, &test_ops) == -EINVAL);
	CHECK(reads == 0);
	teardown();
}

static void *slow_reader(void *arg)
{
	struct xfs_buf *bp;

	(void)arg;
	CHECK(xfs_buf_read(&bt, 6000, 8, 0, &bp, &test_ops) == 0);
	xfs_buf_relse(bp);
	return NULL;
}

static void incore_waits_for_reading(void)
{
	pthread_t t;
	struct xfs_buf *bp;

	setup(WFS_XFS_BUFCACHE_DEFAULT_BYTES);
	read_delay_us = 200000;
	pthread_create(&t, NULL, slow_reader, NULL);
	usleep(50000);
	CHECK(xfs_buf_incore(&bt, 6000, 8, 0, &bp) == 0);
	CHECK(reads == 1 && (bp->b_flags & XBF_DONE) && data_ok(bp, 6000, 4096));
	xfs_buf_relse(bp);
	pthread_join(t, NULL);
	teardown();
}

int main(void)
{
	char path[] = "/tmp/wfs_test_buf_XXXXXX";
	int fd = mkstemp(path);

	close(fd);
	make_image(path);
	hit_does_not_read_again();
	verifier_failure_not_cached();
	eviction_respects_limit();
	release_makes_evictable();
	concurrent_same_block_single_read();
	read_error_maps_to_eio();
	discontiguous_map_reads_all_segments();
	cap_exceeded_returns_enomem();
	get_uncached_and_incore();
	verifier_error_reporting_fails_read();
	failed_read_not_inherited_by_other_ops();
	reverify_does_not_write_shared_fields();
	tiny_limit_concurrency();
	range_guards();
	incore_waits_for_reading();
	close(img_fd);
	unlink(path);
	printf("test_buf: %d checks, %d failures\n", checks, failures);
	return failures ? 1 : 0;
}
