/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * The slice of the block layer the XFS log uses (M3a T2): struct bio over
 * virtually addressed segments, submitted synchronously to the host device
 * (backends/xfs/bio.c). Semantics kept from the kernel:
 *   - REQ_PREFLUSH: everything written to the device before this bio is
 *     durable before its data is written (host blk_flush first);
 *   - REQ_FUA: the bio's data is durable when it completes (blk_flush after);
 *   - bio_split()/bio_chain(): the parent completes after all its children;
 *   - bi_end_io runs once per bio when its I/O (and children) are done.
 * Completion is called on the submitting thread before submit_bio() returns.
 * The XFS log's end_io only queues work, so this never re-enters log locks.
 */
#ifndef WFS_SHIM_BIO_H
#define WFS_SHIM_BIO_H

#include "wfs_types.h"
#include "wfs_errno.h"
#include "wfs_sync.h"

typedef u64 sector_t;
#define SECTOR_SHIFT		9
#define SECTOR_SIZE		(1u << SECTOR_SHIFT)

typedef u8 blk_status_t;
#define BLK_STS_OK		0
#define BLK_STS_IOERR		10
static inline int blk_status_to_errno(blk_status_t s) { return s ? -EIO : 0; }
static inline blk_status_t errno_to_blk_status(int e) { return e ? BLK_STS_IOERR : BLK_STS_OK; }

/* Host device of one role (WFS_DEV_DATA/LOG/RT); owned by the buffer cache. */
struct block_device {
	const void		*wfs_blk;	/* struct wfs_xfs_blk */
	int			wfs_role;
	bool			wfs_read_only;	/* host device opened read-only */
};
static inline bool bdev_write_cache(struct block_device *b) { (void)b; return true; }
static inline bool bdev_read_only(struct block_device *b) { return b->wfs_read_only; }

typedef unsigned int blk_opf_t;
enum req_op { REQ_OP_READ = 0, REQ_OP_WRITE = 1 };
#define REQ_OP_MASK		0xffu
#define REQ_SYNC		(1u << 11)
#define REQ_META		(1u << 12)
#define REQ_IDLE		(1u << 15)
#define REQ_FUA			(1u << 17)
#define REQ_PREFLUSH		(1u << 18)
#define bio_op(bio)		((bio)->bi_opf & REQ_OP_MASK)

struct bio_vec {
	void			*bv_buf;	/* virtual address (no pages here) */
	unsigned int		bv_len;
};
struct bvec_iter {
	sector_t		bi_sector;
	unsigned int		bi_size;	/* bytes left */
	unsigned int		bi_idx;		/* current vec */
	unsigned int		bi_bvec_done;	/* bytes done in the current vec */
};
struct bio;
typedef void (bio_end_io_t)(struct bio *);
struct bio {
	struct block_device	*bi_bdev;
	blk_opf_t		bi_opf;
	blk_status_t		bi_status;
	struct bvec_iter	bi_iter;
	bio_end_io_t		*bi_end_io;
	void			*bi_private;
	struct bio_vec		*bi_io_vec;
	unsigned short		bi_vcnt, bi_max_vecs;
	atomic_t		__bi_remaining;	/* 1 + chained children in flight */
	bool			wfs_allocated;	/* bio_alloc/bio_split: freed by bio_put */
};
struct bio_set { int unused; };
extern struct bio_set fs_bio_set;

void bio_init(struct bio *bio, struct block_device *bdev, struct bio_vec *table,
	      unsigned short max_vecs, blk_opf_t opf);
static inline void bio_uninit(struct bio *bio) { (void)bio; }
struct bio *bio_alloc(struct block_device *bdev, unsigned short nr_vecs, blk_opf_t opf, gfp_t gfp);
void bio_put(struct bio *bio);
void bio_add_virt_nofail(struct bio *bio, void *vaddr, unsigned int len);
bool bio_add_vmalloc(struct bio *bio, void *vaddr, unsigned int len);
/* First `sectors` sectors as a new bio; `bio` keeps the rest. */
struct bio *bio_split(struct bio *bio, int sectors, gfp_t gfp, struct bio_set *bs);
void bio_chain(struct bio *child, struct bio *parent);
void bio_endio(struct bio *bio);
void submit_bio(struct bio *bio);
int submit_bio_wait(struct bio *bio);
/* Number of vecs a virtually contiguous buffer needs (always 1 here). */
static inline unsigned int bio_add_max_vecs(void *kaddr, unsigned int len) { (void)kaddr; (void)len; return 1; }

/* Write zeroes to a sector range (synchronous). */
int blkdev_issue_zeroout(struct block_device *bdev, sector_t sector, sector_t nr_sects,
			 gfp_t gfp, unsigned int flags);
#define BLKDEV_ZERO_NOUNMAP	(1u << 0)
#define BLKDEV_ZERO_NOFALLBACK	(1u << 1)

/* Synchronous I/O of a virtually contiguous buffer (kernel xfs_bio_io.c). */
int xfs_rw_bdev(struct block_device *bdev, sector_t sector, unsigned int count,
		char *data, enum req_op op);

#endif /* WFS_SHIM_BIO_H */
