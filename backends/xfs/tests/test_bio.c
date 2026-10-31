// SPDX-License-Identifier: GPL-2.0-only
/* bio on the host block layer (bio.c): ordering, split/chain, errors. */
#include "xfs_platform.h"
#include "blk.h"
#include <stdio.h>
#include <string.h>

static int failures, checks;
#define CHECK(c) do { checks++; if (!(c)) { failures++; \
	fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #c); } } while (0)

#define DEV_BYTES (64 * 512)
static unsigned char dev[DEV_BYTES];
static char trace[256];
static int fail_at = -1;	/* write offset that fails */

static void tr(const char *s)
{
	strncat(trace, s, sizeof(trace) - strlen(trace) - 1);
}

static int rec_write(void *ctx, int role, uint64_t off, const void *buf, size_t len)
{
	char t[48];

	(void)ctx; (void)role;
	snprintf(t, sizeof(t), "W%llu+%zu ", (unsigned long long)off / 512, len / 512);
	tr(t);
	if ((int)(off / 512) == fail_at)
		return -WFS_EIO;
	memcpy(dev + off, buf, len);
	return 0;
}

static int rec_read(void *ctx, int role, uint64_t off, void *buf, size_t len)
{
	(void)ctx; (void)role;
	memcpy(buf, dev + off, len);
	return 0;
}

static int rec_flush(void *ctx, int role)
{
	(void)ctx; (void)role;
	tr("F ");
	return 0;
}

static int endio_calls;
static blk_status_t endio_status;
static void endio(struct bio *bio)
{
	endio_calls++;
	endio_status = bio->bi_status;
}

static struct wfs_xfs_blk blk = { .read = rec_read, .write = rec_write, .flush = rec_flush };
static struct block_device bdev = { .wfs_blk = &blk, .wfs_role = WFS_DEV_LOG };

static void preflush_fua_order(void)
{
	static unsigned char buf[1024];
	struct bio_vec v[2];
	struct bio bio;

	memset(buf, 0xAB, sizeof(buf));
	trace[0] = 0; endio_calls = 0;
	bio_init(&bio, &bdev, v, 2, REQ_OP_WRITE | REQ_SYNC | REQ_PREFLUSH | REQ_FUA);
	bio.bi_iter.bi_sector = 3;
	bio.bi_end_io = endio;
	CHECK(bio_add_vmalloc(&bio, buf, 1024));
	submit_bio(&bio);
	CHECK(strcmp(trace, "F W3+2 F ") == 0);
	CHECK(endio_calls == 1 && endio_status == BLK_STS_OK);
	CHECK(dev[3 * 512] == 0xAB && dev[5 * 512 - 1] == 0xAB && dev[5 * 512] == 0);
}

/* The log's wrap: split at the device end, chain, submit split then parent. */
static void split_and_chain_wrap(void)
{
	static unsigned char buf[4096];
	struct bio_vec v[2];
	struct bio bio, *split;
	int i;

	for (i = 0; i < 4096; i++) buf[i] = (unsigned char)i;
	trace[0] = 0; endio_calls = 0;
	bio_init(&bio, &bdev, v, 2, REQ_OP_WRITE | REQ_FUA);
	bio.bi_iter.bi_sector = 60;		/* 4 sectors left before 64 */
	bio.bi_end_io = endio;
	bio_add_virt_nofail(&bio, buf, 1536);	/* two segments, split inside the 2nd */
	bio_add_virt_nofail(&bio, buf + 1536, 4096 - 1536);
	split = bio_split(&bio, 64 - 60, GFP_NOIO, &fs_bio_set);
	CHECK(!IS_ERR(split));
	bio_chain(split, &bio);
	submit_bio(split);
	CHECK(endio_calls == 0);		/* parent waits for the child */
	bio.bi_iter.bi_sector = 0;		/* continue at the log start */
	submit_bio(&bio);
	CHECK(endio_calls == 1 && endio_status == BLK_STS_OK);
	CHECK(strcmp(trace, "W60+3 W63+1 F W0+4 F ") == 0);
	CHECK(memcmp(dev + 60 * 512, buf, 2048) == 0);
	CHECK(memcmp(dev, buf + 2048, 2048) == 0);
}

static void child_error_reaches_parent(void)
{
	static unsigned char buf[2048];
	struct bio_vec v[1];
	struct bio bio, *split;

	trace[0] = 0; endio_calls = 0; fail_at = 62;
	bio_init(&bio, &bdev, v, 1, REQ_OP_WRITE);
	bio.bi_iter.bi_sector = 62;
	bio.bi_end_io = endio;
	bio_add_virt_nofail(&bio, buf, 2048);
	split = bio_split(&bio, 2, GFP_NOIO, &fs_bio_set);
	bio_chain(split, &bio);
	submit_bio(split);
	bio.bi_iter.bi_sector = 0;
	submit_bio(&bio);
	CHECK(endio_calls == 1 && endio_status == BLK_STS_IOERR);
	CHECK(blk_status_to_errno(endio_status) == -EIO);
	fail_at = -1;
}

static void rw_bdev_roundtrip(void)
{
	char out[512], in[512];

	memset(out, 'x', sizeof(out));
	CHECK(xfs_rw_bdev(&bdev, 10, 512, out, REQ_OP_WRITE) == 0);
	CHECK(xfs_rw_bdev(&bdev, 10, 512, in, REQ_OP_READ) == 0);
	CHECK(memcmp(in, out, 512) == 0);
	fail_at = 11;
	CHECK(xfs_rw_bdev(&bdev, 11, 512, out, REQ_OP_WRITE) == -EIO);
	fail_at = -1;
}

int main(void)
{
	preflush_fua_order();
	split_and_chain_wrap();
	child_error_reaches_parent();
	rw_bdev_roundtrip();
	printf("test_bio: %d checks, %d failures\n", checks, failures);
	return failures ? 1 : 0;
}
