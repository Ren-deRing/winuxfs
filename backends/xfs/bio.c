// SPDX-License-Identifier: GPL-2.0-only
/*
 * struct bio on the host block device, synchronously (shim/wfs_bio.h has
 * the contract). Used by the XFS log (iclog writes, cache flushes) and by
 * log recovery (xfs_rw_bdev).
 */
#include "xfs_platform.h"
#include "blk.h"

struct bio_set fs_bio_set;

void bio_init(struct bio *bio, struct block_device *bdev, struct bio_vec *table,
	      unsigned short max_vecs, blk_opf_t opf)
{
	memset(bio, 0, sizeof(*bio));
	bio->bi_bdev = bdev;
	bio->bi_opf = opf;
	bio->bi_io_vec = table;
	bio->bi_max_vecs = max_vecs;
	atomic_set(&bio->__bi_remaining, 1);
}

struct bio *bio_alloc(struct block_device *bdev, unsigned short nr_vecs, blk_opf_t opf, gfp_t gfp)
{
	/* The kernel backs bio_alloc(GFP_NOIO) with a mempool: it never fails. */
	struct bio *bio = kzalloc(sizeof(*bio) + (size_t)nr_vecs * sizeof(struct bio_vec),
				  gfp | __GFP_NOFAIL);

	bio_init(bio, bdev, nr_vecs ? (struct bio_vec *)(bio + 1) : NULL, nr_vecs, opf);
	bio->wfs_allocated = true;
	return bio;
}

void bio_put(struct bio *bio)
{
	if (bio->wfs_allocated)
		kfree(bio);
}

void bio_add_virt_nofail(struct bio *bio, void *vaddr, unsigned int len)
{
	if (WARN_ON_ONCE(bio->bi_vcnt >= bio->bi_max_vecs))
		return;
	bio->bi_io_vec[bio->bi_vcnt++] = (struct bio_vec){ .bv_buf = vaddr, .bv_len = len };
	bio->bi_iter.bi_size += len;
}

bool bio_add_vmalloc(struct bio *bio, void *vaddr, unsigned int len)
{
	if (bio->bi_vcnt >= bio->bi_max_vecs)
		return false;
	bio_add_virt_nofail(bio, vaddr, len);
	return true;
}

/* Advance the iterator by `bytes` (must not exceed bi_size). */
static void iter_advance(struct bio *bio, unsigned int bytes)
{
	struct bvec_iter *it = &bio->bi_iter;

	it->bi_sector += bytes >> SECTOR_SHIFT;
	it->bi_size -= bytes;
	while (bytes) {
		unsigned int left = bio->bi_io_vec[it->bi_idx].bv_len - it->bi_bvec_done;
		unsigned int n = min(left, bytes);

		it->bi_bvec_done += n;
		bytes -= n;
		if (it->bi_bvec_done == bio->bi_io_vec[it->bi_idx].bv_len) {
			it->bi_idx++;
			it->bi_bvec_done = 0;
		}
	}
}

struct bio *bio_split(struct bio *bio, int sectors, gfp_t gfp, struct bio_set *bs)
{
	unsigned int bytes = (unsigned int)sectors << SECTOR_SHIFT;
	struct bvec_iter it = bio->bi_iter;
	struct bio *split;
	unsigned int n = 0, left = bytes;

	(void)bs;
	if (WARN_ON_ONCE(sectors <= 0 || bytes >= bio->bi_iter.bi_size))
		return ERR_PTR(-EINVAL);
	/* Count the vecs the first `bytes` touch. */
	for (; left; n++) {
		unsigned int avail = bio->bi_io_vec[it.bi_idx + n].bv_len -
				     (n ? 0 : it.bi_bvec_done);

		left -= min(avail, left);
	}
	split = bio_alloc(bio->bi_bdev, (unsigned short)n, bio->bi_opf, gfp);
	split->bi_iter.bi_sector = bio->bi_iter.bi_sector;
	left = bytes;
	for (n = 0; left; n++) {
		struct bio_vec *v = &bio->bi_io_vec[it.bi_idx + n];
		unsigned int skip = n ? 0 : it.bi_bvec_done;
		unsigned int take = min(v->bv_len - skip, left);

		bio_add_virt_nofail(split, (char *)v->bv_buf + skip, take);
		left -= take;
	}
	/*
	 * The kernel's split clears PREFLUSH/FUA from neither half: the log only
	 * splits a write that wraps the device, and each half keeps the flags.
	 */
	iter_advance(bio, bytes);
	return split;
}

static void bio_chain_endio(struct bio *bio)
{
	struct bio *parent = bio->bi_private;

	if (bio->bi_status && !parent->bi_status)
		parent->bi_status = bio->bi_status;
	bio_put(bio);
	bio_endio(parent);
}

void bio_chain(struct bio *child, struct bio *parent)
{
	child->bi_private = parent;
	child->bi_end_io = bio_chain_endio;
	atomic_inc(&parent->__bi_remaining);
}

void bio_endio(struct bio *bio)
{
	if (!atomic_dec_and_test(&bio->__bi_remaining))
		return;
	if (bio->bi_end_io)
		bio->bi_end_io(bio);
}

static int bio_do_io(struct bio *bio)
{
	const struct wfs_xfs_blk *blk = bio->bi_bdev->wfs_blk;
	int role = bio->bi_bdev->wfs_role;
	u64 off = bio->bi_iter.bi_sector << SECTOR_SHIFT;
	struct bvec_iter it = bio->bi_iter;
	int err = 0;

	if (bio_op(bio) == REQ_OP_WRITE && (bio->bi_opf & REQ_PREFLUSH))
		err = wfs_xfs_blk_flush(blk, role);
	while (!err && it.bi_size) {
		struct bio_vec *v = &bio->bi_io_vec[it.bi_idx];
		char *p = (char *)v->bv_buf + it.bi_bvec_done;
		unsigned int n = min(v->bv_len - it.bi_bvec_done, it.bi_size);

		if (bio_op(bio) == REQ_OP_WRITE)
			err = wfs_xfs_blk_write(blk, role, off, p, n);
		else
			err = wfs_xfs_blk_read(blk, role, off, p, n);
		off += n;
		it.bi_size -= n;
		it.bi_idx++;
		it.bi_bvec_done = 0;
	}
	if (!err && bio_op(bio) == REQ_OP_WRITE && (bio->bi_opf & REQ_FUA))
		err = wfs_xfs_blk_flush(blk, role);
	return err;
}

void submit_bio(struct bio *bio)
{
	int err = bio_do_io(bio);

	if (err) {
		xfs_warn(NULL, "block %s error %d at sector %llu (+%u bytes), role %d",
			 bio_op(bio) == REQ_OP_WRITE ? "write" : "read", err,
			 (unsigned long long)bio->bi_iter.bi_sector, bio->bi_iter.bi_size,
			 bio->bi_bdev->wfs_role);
		bio->bi_status = BLK_STS_IOERR;
	}
	bio_endio(bio);
}

static void submit_bio_wait_endio(struct bio *bio)
{
	(void)bio;
}

int submit_bio_wait(struct bio *bio)
{
	bio->bi_end_io = submit_bio_wait_endio;	/* synchronous already */
	submit_bio(bio);
	return blk_status_to_errno(bio->bi_status);
}

int xfs_rw_bdev(struct block_device *bdev, sector_t sector, unsigned int count,
		char *data, enum req_op op)
{
	struct bio_vec v;
	struct bio bio;

	bio_init(&bio, bdev, &v, 1, op | REQ_META | REQ_SYNC);
	bio.bi_iter.bi_sector = sector;
	bio_add_virt_nofail(&bio, data, count);
	return submit_bio_wait(&bio);
}

int blkdev_issue_zeroout(struct block_device *bdev, sector_t sector, sector_t nr_sects,
			 gfp_t gfp, unsigned int flags)
{
	enum { CHUNK = 64 << 10 };
	void *z = kzalloc(CHUNK, gfp);
	int err = 0;

	(void)flags;
	if (!z)
		return -ENOMEM;
	while (!err && nr_sects) {
		unsigned int n = nr_sects > CHUNK / SECTOR_SIZE ? CHUNK : (unsigned int)nr_sects * SECTOR_SIZE;

		err = xfs_rw_bdev(bdev, sector, n, z, REQ_OP_WRITE);
		sector += n / SECTOR_SIZE;
		nr_sects -= n / SECTOR_SIZE;
	}
	kfree(z);
	return err;
}
