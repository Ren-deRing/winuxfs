/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * xfs_buf contract for the read-only port (Task 5 implements the cache).
 * Function signatures and field names follow the kernel's xfs_buf.h so
 * libxfs compiles unchanged; kernel-only members (hash node, LRU, workqueue,
 * log items, delwri lists) are dropped. Buffer locks are no-ops: a verified
 * buffer is immutable on a read-only mount (design 7.1).
 */
#ifndef __XFS_BUF_H__
#define __XFS_BUF_H__

#include "wfs_shim.h"

struct xfs_buf;
struct xfs_mount;
struct xfs_perag;
struct xfs_buf_log_item;
struct xfs_trans;

#define XFS_BUF_DADDR_MAX	((xfs_daddr_t) S64_MAX)
#define XFS_BUF_DADDR_NULL	((xfs_daddr_t) (-1LL))

#define XBF_READ	 (1u << 0)
#define XBF_WRITE	 (1u << 1)
#define XBF_READ_AHEAD	 (1u << 2)
#define XBF_ASYNC	 (1u << 4)
#define XBF_DONE	 (1u << 5)
#define XBF_STALE	 (1u << 6)
#define XBF_WRITE_FAIL	 (1u << 7)
#define _XBF_KMEM	 (1u << 21)
#define _XBF_DELWRI_Q	 (1u << 22)
#define XBF_LIVESCAN	 (1u << 28)
#define XBF_INCORE	 (1u << 29)
#define XBF_TRYLOCK	 (1u << 30)

typedef unsigned int xfs_buf_flags_t;

/* A device the filesystem reads: data, RT or (never read here) log. */
struct xfs_buftarg {
	struct xfs_mount	*bt_mount;
	unsigned int		bt_meta_sectorsize;
	size_t			bt_meta_sectormask;
	size_t			bt_logical_sectorsize;
	size_t			bt_logical_sectormask;
	xfs_daddr_t		bt_nr_sectors;
	void			*bt_priv;	/* backend: host device handle */
};

struct xfs_buf_map {
	xfs_daddr_t		bm_bn;	/* block number for I/O */
	int			bm_len;	/* size of I/O */
	unsigned int		bm_flags;
};
#define XBM_LIVESCAN		(1U << 0)

#define DEFINE_SINGLE_BUF_MAP(map, blkno, numblk) \
	struct xfs_buf_map (map) = { .bm_bn = (blkno), .bm_len = (numblk) };

struct xfs_buf_ops {
	char *name;
	union {
		__be32 magic[2];	/* v4 and v5 on disk magic values */
		__be16 magic16[2];	/* v4 and v5 on disk magic values */
	};
	void (*verify_read)(struct xfs_buf *);
	void (*verify_write)(struct xfs_buf *);
	xfs_failaddr_t (*verify_struct)(struct xfs_buf *bp);
};

struct xfs_buf {
	xfs_daddr_t		b_rhash_key;	/* cache key (daddr) */
	int			b_length;	/* size of buffer in BBs */
	xfs_buf_flags_t		b_flags;
	struct semaphore	b_sema;		/* unused: locks are no-ops */
	struct list_head	b_lru;
	struct list_head	b_list;
	struct xfs_perag	*b_pag;
	struct xfs_mount	*b_mount;
	struct xfs_buftarg	*b_target;
	void			*b_addr;	/* data, b_length * 512 bytes */
	struct xfs_buf_log_item	*b_log_item;	/* always NULL */
	struct list_head	b_li_list;
	struct xfs_trans	*b_transp;	/* always NULL */
	struct xfs_buf_map	*b_maps;
	struct xfs_buf_map	__b_map;
	int			b_map_count;
	int			b_error;
	void			(*b_iodone)(struct xfs_buf *bp);
	const struct xfs_buf_ops *b_ops;
	/* Task 5 (buf.c) adds its own cache bookkeeping behind this. */
	void			*b_priv;
};

int xfs_buf_get_map(struct xfs_buftarg *target, struct xfs_buf_map *map,
		int nmaps, xfs_buf_flags_t flags, struct xfs_buf **bpp);
int xfs_buf_read_map(struct xfs_buftarg *target, struct xfs_buf_map *map,
		int nmaps, xfs_buf_flags_t flags, struct xfs_buf **bpp,
		const struct xfs_buf_ops *ops, xfs_failaddr_t fa);
void xfs_buf_readahead_map(struct xfs_buftarg *target,
		struct xfs_buf_map *map, int nmaps,
		const struct xfs_buf_ops *ops);

static inline int
xfs_buf_incore(struct xfs_buftarg *target, xfs_daddr_t blkno, size_t numblks,
	       xfs_buf_flags_t flags, struct xfs_buf **bpp)
{
	DEFINE_SINGLE_BUF_MAP(map, blkno, (int)numblks);
	return xfs_buf_get_map(target, &map, 1, XBF_INCORE | flags, bpp);
}

static inline int
xfs_buf_get(struct xfs_buftarg *target, xfs_daddr_t blkno, size_t numblks,
	    struct xfs_buf **bpp)
{
	if (numblks > INT_MAX)	/* a hostile length must not wrap into a valid one */
		return -EINVAL;
	{
	DEFINE_SINGLE_BUF_MAP(map, blkno, (int)numblks);
	return xfs_buf_get_map(target, &map, 1, 0, bpp);
	}
}

static inline int
xfs_buf_read(struct xfs_buftarg *target, xfs_daddr_t blkno, size_t numblks,
	     xfs_buf_flags_t flags, struct xfs_buf **bpp,
	     const struct xfs_buf_ops *ops)
{
	if (numblks > INT_MAX)
		return -EINVAL;
	{
	DEFINE_SINGLE_BUF_MAP(map, blkno, (int)numblks);
	return xfs_buf_read_map(target, &map, 1, flags, bpp, ops,
			__builtin_return_address(0));
	}
}

static inline void
xfs_buf_readahead(struct xfs_buftarg *target, xfs_daddr_t blkno, size_t numblks,
		  const struct xfs_buf_ops *ops)
{
	DEFINE_SINGLE_BUF_MAP(map, blkno, (int)numblks);
	xfs_buf_readahead_map(target, &map, 1, ops);
}

int xfs_buf_get_uncached(struct xfs_buftarg *target, size_t numblks,
		struct xfs_buf **bpp);
int xfs_buf_read_uncached(struct xfs_buftarg *target, xfs_daddr_t daddr,
		size_t numblks, struct xfs_buf **bpp,
		const struct xfs_buf_ops *ops);
int _xfs_buf_read(struct xfs_buf *bp);
void xfs_buf_hold(struct xfs_buf *bp);
void xfs_buf_rele(struct xfs_buf *);

/* Read-only: locking a verified buffer is a no-op. */
static inline int xfs_buf_trylock(struct xfs_buf *bp) { (void)bp; return 1; }
static inline void xfs_buf_lock(struct xfs_buf *bp) { (void)bp; }
static inline void xfs_buf_unlock(struct xfs_buf *bp) { (void)bp; }
#define xfs_buf_islocked(bp)	((void)(bp), 1)

static inline void xfs_buf_relse(struct xfs_buf *bp)
{
	xfs_buf_unlock(bp);
	xfs_buf_rele(bp);
}

int xfs_bwrite(struct xfs_buf *bp);	/* stub: -EROFS */

void __xfs_buf_ioerror(struct xfs_buf *bp, int error, xfs_failaddr_t failaddr);
#define xfs_buf_ioerror(bp, err) __xfs_buf_ioerror((bp), (err), __this_address)
void xfs_buf_ioerror_alert(struct xfs_buf *bp, xfs_failaddr_t fa);
void xfs_buf_fail(struct xfs_buf *bp);
void __xfs_buf_mark_corrupt(struct xfs_buf *bp, xfs_failaddr_t fa);
#define xfs_buf_mark_corrupt(bp) __xfs_buf_mark_corrupt((bp), __this_address)

static inline void *xfs_buf_offset(struct xfs_buf *bp, size_t offset)
{
	return (char *)bp->b_addr + offset;
}

static inline void xfs_buf_zero(struct xfs_buf *bp, size_t boff, size_t bsize)
{
	memset((char *)bp->b_addr + boff, 0, bsize);
}

void xfs_buf_set_uptodate(struct xfs_buf *bp);
void xfs_buf_stale(struct xfs_buf *bp);
void xfs_buf_clear_stale(struct xfs_buf *bp);

int xfs_buf_delwri_submit(struct list_head *);
bool xfs_buf_delwri_queue(struct xfs_buf *, struct list_head *);
static inline xfs_daddr_t xfs_buf_daddr(struct xfs_buf *bp)
{
	return bp->b_maps[0].bm_bn;
}

void xfs_buf_set_ref(struct xfs_buf *bp, int lru_ref);
static inline void xfs_buf_oneshot(struct xfs_buf *bp) { (void)bp; }
static inline int xfs_buf_ispinned(struct xfs_buf *bp) { (void)bp; return 0; }

static inline int
xfs_buf_verify_cksum(struct xfs_buf *bp, unsigned long cksum_offset)
{
	return xfs_verify_cksum(bp->b_addr, BBTOB(bp->b_length), cksum_offset);
}

static inline void
xfs_buf_update_cksum(struct xfs_buf *bp, unsigned long cksum_offset)
{
	xfs_update_cksum(bp->b_addr, BBTOB(bp->b_length), cksum_offset);
}

bool xfs_verify_magic(struct xfs_buf *bp, __be32 dmagic);
bool xfs_verify_magic16(struct xfs_buf *bp, __be16 dmagic);

#endif	/* __XFS_BUF_H__ */
