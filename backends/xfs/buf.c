// SPDX-License-Identifier: GPL-2.0-only
/*
 * XFS buffer cache (replaces fs/xfs/xfs_buf.c). Design: M2 design 7.1 for the
 * cache (sharded hash, byte-limited LRU), kernel xfs_buf.c for the buffer
 * contract (M3a T2):
 *
 *   - lookups return a referenced buffer; get/read return it locked
 *     (b_sema), and the caller unlocks and releases (xfs_buf_relse);
 *   - XBF_DONE means the data is valid; a read happens under the lock, so two
 *     threads never read the same block at once (single flight);
 *   - a failed read stales the buffer: it stays hashed until its last
 *     release, and a later lookup recycles it and reads again (no negative
 *     caching); a stale buffer is freed at its last release;
 *   - b_pin_count (incremented by the log while a buffer's changes are only
 *     in the in-core log) holds off writes: xfs_buf_submit waits for unpin;
 *   - delwri lists queue locked buffers for later write (xfs_buf_delwri_*);
 *   - I/O is synchronous: an async write completes (buf_ioend) before
 *     xfs_buf_submit returns, a sync one in xfs_buf_iowait;
 *   - a metadata write error shuts the filesystem down (no retries).
 *
 * Hardening kept from M2: a valid buffer requested with other verifier ops
 * is verified again, on a private copy, so a block cannot pass as two types.
 *
 * Locks (order: b_sema -> shard.lock -> lru_lock):
 *   b_sema      the buffer lock: contents, b_flags, b_ops, b_error
 *   shard.lock  one of NSHARD mutexes; hash chains and refs/onlru/hashed of
 *               the buffers hashed in it
 *   lru_lock    the LRU list
 * The evictor holds lru_lock and takes a shard lock only with trylock, so it
 * cannot deadlock; a busy shard is skipped. Only unreferenced buffers are on
 * the LRU, and every lock owner, delwri list and log item holds a reference,
 * so nothing in use, queued, dirty or pinned is ever evicted. usage (bytes of
 * buffer data held) is atomic; the limit is soft. Memory comes from the shim
 * allocator, so wfs_mem_set_cap is honoured.
 */
#include "buf.h"
#include "xfs_format.h"
#include "xfs_log_format.h"
#include "xfs_shared.h"
#include "xfs_trans_resv.h"
#include "xfs_mount.h"
#include "xfs_error.h"
#include "xfs_trans.h"
#include "xfs_ag.h"
#include "xfs_buf_item.h"
#include "xfs_log.h"
#include "wfs_xfs.h"

/* test_buf links buf.c without libxfs; perag_up is never set there. */
#pragma weak xfs_group_get
#pragma weak xfs_group_put

/* Write-path hooks (backends/xfs: mount.c, the log); weak so test_buf links buf.c alone. */
#pragma weak wfs_xfs_is_shutdown
#pragma weak wfs_xfs_force_shutdown
#pragma weak xfs_buf_item_done
#pragma weak xfs_log_force
#pragma weak xfs_log_in_recovery

#define NSHARD		64
#define NBUCKET		512		/* per shard */
#define MAX_BUF_BB	8192		/* 4 MiB sanity bound on one buffer */

struct lru_node { struct lru_node *prev, *next; };

struct wfs_xbuf {
	struct xfs_buf		b;	/* first: xfs_buf * == wfs_xbuf * */
	struct wfs_xbuf		*hnext;
	struct lru_node		lru;
	struct wfs_xfs_bcache	*c;
	/*
	 * Ops the data was last verified against as requested by a reader
	 * (buffer lock). A dispatching verifier (xfs_da3_node_buf_ops) switches
	 * b_ops to the concrete type, so readers asking for the dispatcher again
	 * must not count as "other ops" and pay a CRC each time.
	 */
	const struct xfs_buf_ops *vops;
	size_t			bytes;
	int			refs;		/* b_hold */
	unsigned int		shard;
	bool			hashed;		/* false: uncached, or evicted/killed */
	bool			onlru;
};

struct bshard {
	struct mutex	lock;
	struct wfs_xbuf	*bucket[NBUCKET];
};

struct wfs_xfs_bcache {
	const struct wfs_xfs_blk *blk;
	size_t		limit;		/* atomic */
	size_t		usage;		/* atomic */
	struct mutex	lru_lock;
	struct lru_node	lru;		/* sentinel: next = MRU, prev = LRU tail */
	struct bshard	shard[NSHARD];
};

struct wfs_bdev {
	struct wfs_xfs_bcache	*c;
	int			role;
	struct block_device	bdev;	/* bt_bdev: bio I/O to the same device */
};

/* b_priv, not a cast: a verifier may be handed a private copy of the xfs_buf */
static inline struct wfs_xbuf *WB(struct xfs_buf *bp) { return bp->b_priv; }
static inline struct wfs_bdev *BD(struct xfs_buftarg *t) { return t->bt_priv; }

static bool buf_shutdown(struct xfs_buf *bp)
{
	return bp->b_mount && wfs_xfs_is_shutdown && wfs_xfs_is_shutdown(bp->b_mount);
}

static void buf_force_shutdown(struct xfs_buf *bp, uint32_t flags)
{
	if (bp->b_mount && wfs_xfs_force_shutdown)
		wfs_xfs_force_shutdown(bp->b_mount, flags);
}

static inline void set_flags(struct xfs_buf *bp, unsigned int f)
{
	WRITE_ONCE(bp->b_flags, bp->b_flags | f);
}

static inline void clear_flags(struct xfs_buf *bp, unsigned int f)
{
	WRITE_ONCE(bp->b_flags, bp->b_flags & ~f);
}

/* ---- cache object ------------------------------------------------------ */

struct wfs_xfs_bcache *wfs_xfs_bcache_create(const struct wfs_xfs_blk *blk,
		size_t limit_bytes)
{
	struct wfs_xfs_bcache *c = kzalloc(sizeof(*c), 0);
	int i;

	if (!c)
		return NULL;
	c->blk = blk;
	c->limit = limit_bytes;
	mutex_init(&c->lru_lock);
	c->lru.prev = c->lru.next = &c->lru;
	for (i = 0; i < NSHARD; i++)
		mutex_init(&c->shard[i].lock);
	return c;
}

void wfs_xfs_bcache_destroy(struct wfs_xfs_bcache *c)
{
	int i;

	if (!c)
		return;
	wfs_xfs_bcache_purge(c);
	for (i = 0; i < NSHARD; i++)
		mutex_destroy(&c->shard[i].lock);
	mutex_destroy(&c->lru_lock);
	kfree(c);
}

size_t wfs_xfs_bcache_limit(struct wfs_xfs_bcache *c)
{
	return __atomic_load_n(&c->limit, __ATOMIC_RELAXED);
}

size_t wfs_xfs_bcache_usage(struct wfs_xfs_bcache *c)
{
	return __atomic_load_n(&c->usage, __ATOMIC_RELAXED);
}

/* ---- LRU (caller holds lru_lock) ---------------------------------------- */

static void lru_unlink(struct lru_node *n)
{
	n->prev->next = n->next;
	n->next->prev = n->prev;
	n->prev = n->next = NULL;
}

static void lru_push_mru(struct wfs_xfs_bcache *c, struct lru_node *n)
{
	n->next = c->lru.next;
	n->prev = &c->lru;
	c->lru.next->prev = n;
	c->lru.next = n;
}

static struct wfs_xbuf *lru_owner(struct lru_node *n)
{
	return (struct wfs_xbuf *)((char *)n - offsetof(struct wfs_xbuf, lru));
}

/* ---- hashing ------------------------------------------------------------ */

static unsigned int key_hash(const struct xfs_buftarg *t, xfs_daddr_t bn, int len)
{
	uint64_t h = (uint64_t)bn * 0x9E3779B97F4A7C15ull;

	h ^= (uint64_t)(uintptr_t)t * 0xC2B2AE3D27D4EB4Full;
	h ^= (uint64_t)len * 0x165667B19E3779F9ull;
	h ^= h >> 29;
	return (unsigned int)(h >> 7);
}

static unsigned int shard_of(unsigned int h) { return h % NSHARD; }
static unsigned int bucket_of(unsigned int h) { return (h / NSHARD) % NBUCKET; }

static bool key_match(const struct wfs_xbuf *wb, const struct xfs_buftarg *t,
		const struct xfs_buf_map *m, int nmaps, int len)
{
	int i;

	if (wb->b.b_target != t || wb->b.b_length != len ||
	    wb->b.b_map_count != nmaps)
		return false;
	for (i = 0; i < nmaps; i++)
		if (wb->b.b_maps[i].bm_bn != m[i].bm_bn ||
		    wb->b.b_maps[i].bm_len != m[i].bm_len)
			return false;
	return true;
}

/* shard lock held */
static void unhash(struct wfs_xbuf *wb)
{
	struct bshard *s = &wb->c->shard[wb->shard];
	unsigned int h = key_hash(wb->b.b_target, wb->b.b_maps[0].bm_bn,
				  wb->b.b_length);
	struct wfs_xbuf **pp = &s->bucket[bucket_of(h)];

	for (; *pp; pp = &(*pp)->hnext) {
		if (*pp == wb) {
			*pp = wb->hnext;
			break;
		}
	}
	wb->hnext = NULL;
	wb->hashed = false;
}

/* ---- allocation / eviction ---------------------------------------------- */

static void free_buf(struct wfs_xbuf *wb)
{
	ASSERT(!(wb->b.b_flags & _XBF_DELWRI_Q));
	ASSERT(atomic_read(&wb->b.b_pin_count) == 0);
	if (wb->b.b_pag && xfs_group_put)
		xfs_perag_put(wb->b.b_pag);
	__atomic_sub_fetch(&wb->c->usage, wb->bytes, __ATOMIC_RELAXED);
	if (wb->b.b_maps != &wb->b.__b_map)
		kfree(wb->b.b_maps);
	kfree(wb->b.b_addr);
	kfree(wb);
}

/*
 * Evict unreferenced buffers, LRU tail first, until usage <= target. Only
 * buffers with no holder are on the LRU, and every lock owner, delwri queue
 * and log item holds a reference, so nothing locked, queued, dirty or pinned
 * is ever evicted.
 */
static void evict_to(struct wfs_xfs_bcache *c, size_t target)
{
	while (__atomic_load_n(&c->usage, __ATOMIC_RELAXED) > target) {
		struct wfs_xbuf *victim = NULL;
		struct lru_node *n;

		mutex_lock(&c->lru_lock);
		for (n = c->lru.prev; n != &c->lru; n = n->prev) {
			struct wfs_xbuf *wb = lru_owner(n);
			struct bshard *s = &c->shard[wb->shard];

			if (!mutex_trylock(&s->lock))
				continue;	/* busy shard: skip, stay soft */
			lru_unlink(n);
			wb->onlru = false;
			unhash(wb);
			mutex_unlock(&s->lock);
			victim = wb;
			break;
		}
		mutex_unlock(&c->lru_lock);
		if (!victim)
			return;
		free_buf(victim);
	}
}

static void evict_to_limit(struct wfs_xfs_bcache *c)
{
	evict_to(c, wfs_xfs_bcache_limit(c));
}

void wfs_xfs_bcache_set_limit(struct wfs_xfs_bcache *c, size_t limit_bytes)
{
	__atomic_store_n(&c->limit, limit_bytes, __ATOMIC_RELAXED);
	evict_to_limit(c);
}

void wfs_xfs_bcache_purge(struct wfs_xfs_bcache *c)
{
	evict_to(c, 0);
}

/* the fuzz builds set 0 so that an over-read is reported exactly */
#ifndef WFS_BUF_SLACK
#define WFS_BUF_SLACK 1024u
#endif

static struct wfs_xbuf *alloc_buf(struct wfs_xfs_bcache *c,
		struct xfs_buftarg *t, const struct xfs_buf_map *m, int nmaps,
		int len)
{
	struct wfs_xbuf *wb = kzalloc(sizeof(*wb), 0);
	int i;

	if (!wb)
		return NULL;
	wb->bytes = (size_t)len << 9;
	/*
	 * Zeroed slack behind every buffer (defence in depth): libxfs forms
	 * pointers from on-disk offsets and may read a few hundred bytes past a
	 * bad one (a directory entry near the end of a block). The slack is not
	 * counted in b_length or in the cache usage, and never written to disk.
	 */
	wb->b.b_addr = kzalloc(wb->bytes + WFS_BUF_SLACK, 0);
	if (!wb->b.b_addr)
		goto fail;
	if (nmaps == 1) {
		wb->b.b_maps = &wb->b.__b_map;
	} else {
		wb->b.b_maps = kmalloc_array(nmaps, sizeof(*m), 0);
		if (!wb->b.b_maps)
			goto fail;
	}
	for (i = 0; i < nmaps; i++)
		wb->b.b_maps[i] = m[i];
	wb->b.b_map_count = nmaps;
	wb->b.b_length = len;
	wb->b.b_rhash_key = m[0].bm_bn;
	wb->b.b_target = t;
	wb->b.b_mount = t->bt_mount;
	wb->b.b_priv = wb;
	sema_init(&wb->b.b_sema, 1);	/* unlocked (kernel xfs_buf_alloc) */
	atomic_set(&wb->b.b_pin_count, 0);
	init_waitqueue_head(&wb->b.b_waiters);
	INIT_LIST_HEAD(&wb->b.b_list);
	INIT_LIST_HEAD(&wb->b.b_li_list);
	INIT_LIST_HEAD(&wb->b.b_lru);
	wb->c = c;
	wb->refs = 1;
	__atomic_add_fetch(&c->usage, wb->bytes, __ATOMIC_RELAXED);
	return wb;
fail:
	kfree(wb->b.b_addr);
	kfree(wb);
	return NULL;
}

/* Allocate; on failure drop the cache (all unreferenced buffers) and retry. */
static struct wfs_xbuf *alloc_buf_retry(struct wfs_xfs_bcache *c,
		struct xfs_buftarg *t, const struct xfs_buf_map *m, int nmaps,
		int len)
{
	struct wfs_xbuf *wb = alloc_buf(c, t, m, nmaps, len);

	if (!wb) {
		evict_to(c, 0);
		wb = alloc_buf(c, t, m, nmaps, len);
	}
	return wb;
}

/*
 * The AG of a data-device buffer, held for the buffer's life like the
 * kernel's b_pag (write paths use agbp->b_pag). Buffers made before the
 * perags exist (mount) get theirs on a later lookup. Shard lock held.
 */
static void attach_pag(struct wfs_xbuf *wb)
{
	struct xfs_buf *bp = &wb->b;
	struct wfs_xfs *x;

	if (bp->b_pag || !bp->b_mount || !xfs_group_get || BD(bp->b_target)->role != WFS_DEV_DATA)
		return;
	x = wfs_xfs_of(bp->b_mount);
	if (x && x->perag_up)
		bp->b_pag = xfs_perag_get(bp->b_mount,
				xfs_daddr_to_agno(bp->b_mount, xfs_buf_daddr(bp)));
}

/* ---- lookup / creation ---------------------------------------------------- */

/* Validate maps; returns total length in BB or a negative errno. */
static int check_maps(struct xfs_buftarg *t, const struct xfs_buf_map *m,
		int nmaps, bool range)
{
	long long total = 0;
	int i;

	if (nmaps < 1 || !t || !t->bt_priv)
		return -EINVAL;
	for (i = 0; i < nmaps; i++) {
		if (m[i].bm_len <= 0 || m[i].bm_bn < 0)
			return -EINVAL;
		if ((uint64_t)m[i].bm_bn > (uint64_t)(INT64_MAX >> 9) - MAX_BUF_BB)
			return -EFSCORRUPTED;	/* byte offset would overflow */
		total += m[i].bm_len;
		if (total > MAX_BUF_BB)
			return -EINVAL;
		if (range && t->bt_nr_sectors &&
		    (unsigned long long)m[i].bm_bn + m[i].bm_len >
		    (unsigned long long)t->bt_nr_sectors)
			return -EFSCORRUPTED;	/* past end of device */
	}
	return (int)total;
}

/* shard lock held; takes a reference */
static struct wfs_xbuf *find_ref(struct bshard *s, unsigned int h,
		struct xfs_buftarg *t, const struct xfs_buf_map *m, int nmaps,
		int len)
{
	struct wfs_xbuf *wb;

	for (wb = s->bucket[bucket_of(h)]; wb; wb = wb->hnext) {
		if (!key_match(wb, t, m, nmaps, len))
			continue;
		if (wb->onlru) {
			mutex_lock(&wb->c->lru_lock);
			lru_unlink(&wb->lru);
			mutex_unlock(&wb->c->lru_lock);
			wb->onlru = false;
		}
		wb->refs++;
		attach_pag(wb);
		return wb;
	}
	return NULL;
}

/* Kernel xfs_find_get_buf: a referenced, unlocked buffer. */
static int buf_find(struct xfs_buftarg *t, const struct xfs_buf_map *m,
		int nmaps, xfs_buf_flags_t flags, struct wfs_xbuf **out)
{
	int len = check_maps(t, m, nmaps, true);
	struct wfs_xfs_bcache *c;
	unsigned int h, si;
	struct bshard *s;
	struct wfs_xbuf *wb, *nw;

	if (len < 0)
		return len;
	c = BD(t)->c;
	h = key_hash(t, m[0].bm_bn, len);
	si = shard_of(h);
	s = &c->shard[si];

	mutex_lock(&s->lock);
	wb = find_ref(s, h, t, m, nmaps, len);
	mutex_unlock(&s->lock);
	if (wb) {
		*out = wb;
		return 0;
	}
	if (flags & XBF_INCORE)
		return -ENOENT;

	nw = alloc_buf_retry(c, t, m, nmaps, len);
	if (!nw)
		return -ENOMEM;
	nw->shard = si;

	mutex_lock(&s->lock);
	wb = find_ref(s, h, t, m, nmaps, len);	/* lost a race? */
	if (!wb) {
		struct wfs_xbuf **head = &s->bucket[bucket_of(h)];

		nw->hnext = *head;
		*head = nw;
		nw->hashed = true;
		attach_pag(nw);
		wb = nw;
		nw = NULL;
	}
	mutex_unlock(&s->lock);
	if (nw)
		free_buf(nw);
	evict_to_limit(c);
	*out = wb;
	return 0;
}

/* Kernel xfs_buf_find_lock: lock, and recycle a stale buffer for reuse. */
static int buf_find_lock(struct xfs_buf *bp, xfs_buf_flags_t flags)
{
	if (flags & XBF_TRYLOCK) {
		if (!xfs_buf_trylock(bp))
			return -EAGAIN;
	} else {
		xfs_buf_lock(bp);
	}
	if (bp->b_flags & XBF_STALE) {
		if (flags & XBF_LIVESCAN) {
			xfs_buf_unlock(bp);
			return -ENOENT;
		}
		ASSERT(!(bp->b_flags & _XBF_DELWRI_Q));
		clear_flags(bp, ~0u);
		bp->b_ops = NULL;
		WB(bp)->vops = NULL;
	}
	return 0;
}

/* ---- reference counting --------------------------------------------------- */

void xfs_buf_hold(struct xfs_buf *bp)
{
	struct wfs_xbuf *wb = WB(bp);
	struct bshard *s = &wb->c->shard[wb->shard];

	mutex_lock(&s->lock);
	ASSERT(wb->refs > 0);
	wb->refs++;
	mutex_unlock(&s->lock);
}

/* Kernel xfs_buf_rele: the last reference parks the buffer on the LRU, or
 * frees it when it is uncached or stale. */
void xfs_buf_rele(struct xfs_buf *bp)
{
	struct wfs_xbuf *wb = WB(bp);
	struct wfs_xfs_bcache *c = wb->c;
	struct bshard *s = &c->shard[wb->shard];
	bool freeit = false;

	mutex_lock(&s->lock);
	ASSERT(wb->refs > 0);
	if (--wb->refs == 0) {
		if (!wb->hashed || (bp->b_flags & XBF_STALE)) {
			if (wb->hashed)
				unhash(wb);
			freeit = true;
		} else {
			mutex_lock(&c->lru_lock);
			lru_push_mru(c, &wb->lru);
			mutex_unlock(&c->lru_lock);
			wb->onlru = true;
		}
	}
	mutex_unlock(&s->lock);
	if (freeit)
		free_buf(wb);
	else
		evict_to_limit(c);
}

/* ---- locking and pinning (kernel semantics) ------------------------------ */

int xfs_buf_trylock(struct xfs_buf *bp)
{
	return down_trylock(&bp->b_sema) == 0;
}

/*
 * A stale, pinned buffer is being reallocated and stays locked until the log
 * is pushed: push it ourselves rather than wait for someone else (kernel).
 */
void xfs_buf_lock(struct xfs_buf *bp)
{
	if (atomic_read(&bp->b_pin_count) && (READ_ONCE(bp->b_flags) & XBF_STALE) && xfs_log_force)
		xfs_log_force(bp->b_mount, 0);
	down(&bp->b_sema);
}

void xfs_buf_unlock(struct xfs_buf *bp)
{
	ASSERT(xfs_buf_islocked(bp));
	up(&bp->b_sema);
}

static void buf_wait_unpin(struct xfs_buf *bp)
{
	DECLARE_WAITQUEUE(wait, current);

	if (atomic_read(&bp->b_pin_count) == 0)
		return;
	add_wait_queue(&bp->b_waiters, &wait);
	for (;;) {
		set_current_state(TASK_UNINTERRUPTIBLE);
		if (atomic_read(&bp->b_pin_count) == 0)
			break;
		io_schedule();
	}
	remove_wait_queue(&bp->b_waiters, &wait);
	set_current_state(TASK_RUNNING);
}

/* ---- I/O ------------------------------------------------------------------ */

static int buf_rw(struct xfs_buf *bp, bool write)
{
	struct xfs_buftarg *t = bp->b_target;
	const struct wfs_xfs_blk *blk = BD(t)->c->blk;
	int role = BD(t)->role;
	size_t pos = 0;
	int i;

	for (i = 0; i < bp->b_map_count; i++) {
		size_t n = (size_t)bp->b_maps[i].bm_len << 9;
		uint64_t off = (uint64_t)bp->b_maps[i].bm_bn << 9;
		int err = write ? wfs_xfs_blk_write(blk, role, off, (char *)bp->b_addr + pos, n)
				: wfs_xfs_blk_read(blk, role, off, (char *)bp->b_addr + pos, n);

		if (err)
			return -EIO;
		pos += n;
	}
	return 0;
}

/*
 * Run ops->verify_read on bp. AG btree verifiers read bp->b_pag (sibling
 * range checks): a data-device block outside every AG fails here. Returns the
 * verifier's result (negative errno) and leaves b_error clear.
 */
static int run_verify(struct xfs_buf *bp, const struct xfs_buf_ops *ops)
{
	int err;

	bp->b_error = 0;
	if (ops && ops->verify_read) {
		struct wfs_xfs *x = bp->b_mount ? wfs_xfs_of(bp->b_mount) : NULL;
		struct xfs_buf_ops const *saved = bp->b_ops;

		bp->b_ops = ops;
		if (x && x->perag_up && BD(bp->b_target)->role == WFS_DEV_DATA && !bp->b_pag &&
		    bp->b_rhash_key != XFS_BUF_DADDR_NULL)	/* not the uncached sb re-read */
			bp->b_error = -EFSCORRUPTED;	/* block outside every AG */
		else
			ops->verify_read(bp);
		if (bp->b_error)
			bp->b_ops = saved;
	}
	err = bp->b_error;
	bp->b_error = 0;
	return err > 0 ? -err : err;
}

/*
 * Verify a valid buffer against other ops than it was read with, on a
 * private copy (M2 hardening: the kernel trusts the first verifier; a hostile
 * image could otherwise get one block type through as another).
 */
static int verify_copy(struct xfs_buf *bp, const struct xfs_buf_ops *ops)
{
	/* Only what verifiers read: the lock and wait queues are live elsewhere. */
	struct xfs_buf tmp = {
		.b_rhash_key	= bp->b_rhash_key,
		.b_length	= bp->b_length,
		.b_flags	= bp->b_flags,
		.b_pag		= bp->b_pag,
		.b_mount	= bp->b_mount,
		.b_target	= bp->b_target,
		.b_addr		= bp->b_addr,
		.b_maps		= bp->b_maps,
		.b_map_count	= bp->b_map_count,
		.b_ops		= bp->b_ops,
		.b_priv		= bp->b_priv,
	};

	return run_verify(&tmp, ops);
}

static bool buf_verify_write(struct xfs_buf *bp)
{
	if (bp->b_ops) {
		bp->b_error = 0;
		bp->b_ops->verify_write(bp);
		if (bp->b_error)
			return false;
	} else if (bp->b_rhash_key != XFS_BUF_DADDR_NULL) {
		/* v5 only: every cached metadata write must carry ops */
		xfs_warn(bp->b_mount, "no buf ops on daddr 0x%llx len %d",
			 (unsigned long long)xfs_buf_daddr(bp), bp->b_length);
		bp->b_error = -EFSCORRUPTED;
		return false;
	}
	return true;
}

static void buf_ioend(struct xfs_buf *bp);

/*
 * Kernel xfs_buf_submit. I/O is synchronous here: an async buffer is
 * completed (buf_ioend) before this returns; a sync one is completed by
 * xfs_buf_iowait, as in the kernel.
 */
static void buf_submit(struct xfs_buf *bp)
{
	ASSERT(!(bp->b_flags & _XBF_DELWRI_Q));
	if (buf_shutdown(bp)) {
		xfs_buf_ioerror(bp, -EIO);
		goto ioerror;
	}
	if (bp->b_flags & XBF_WRITE)
		buf_wait_unpin(bp);
	bp->b_error = 0;
	if (bp->b_flags & XBF_WRITE) {
		if (!buf_verify_write(bp)) {
			buf_force_shutdown(bp, SHUTDOWN_CORRUPT_INCORE);
			goto ioerror;
		}
		if (buf_rw(bp, true))
			xfs_buf_ioerror(bp, -EIO);
	} else {
		if (buf_rw(bp, false))
			xfs_buf_ioerror(bp, -EIO);
	}
	goto end_io;
ioerror:
	clear_flags(bp, XBF_DONE);
	xfs_buf_stale(bp);
end_io:
	if (bp->b_flags & XBF_ASYNC)
		buf_ioend(bp);
}

/*
 * A failed write. Policy (stricter than the kernel's configurable retries):
 * any metadata write error shuts the filesystem down; the buffer is staled so
 * its contents are never written later. Returns false: ioend continues.
 */
static bool buf_ioend_handle_error(struct xfs_buf *bp)
{
	if (!buf_shutdown(bp)) {
		xfs_buf_ioerror_alert(bp, __this_address);
		buf_force_shutdown(bp, SHUTDOWN_META_IO_ERROR);
	}
	if (bp->b_flags & XBF_ASYNC)
		set_flags(bp, XBF_WRITE_FAIL);
	xfs_buf_stale(bp);
	set_flags(bp, XBF_DONE);
	clear_flags(bp, XBF_WRITE);
	return false;
}

/* Kernel xfs_buf_ioend. */
static void buf_ioend(struct xfs_buf *bp)
{
	bool async = bp->b_flags & XBF_ASYNC;

	if (bp->b_flags & XBF_READ) {
		if (!bp->b_error && bp->b_ops) {
			int err = run_verify(bp, bp->b_ops);

			if (err)
				xfs_buf_ioerror(bp, err);
		}
		if (!bp->b_error)
			set_flags(bp, XBF_DONE);
	} else {
		if (unlikely(bp->b_error)) {
			buf_ioend_handle_error(bp);
		} else {
			clear_flags(bp, XBF_WRITE_FAIL);
			set_flags(bp, XBF_DONE);
		}
		if (bp->b_log_item && xfs_buf_item_done)
			xfs_buf_item_done(bp);
		if (bp->b_iodone)
			bp->b_iodone(bp);
	}
	clear_flags(bp, XBF_READ | XBF_WRITE | XBF_READ_AHEAD);
	if (async)
		xfs_buf_relse(bp);
}

int xfs_buf_iowait(struct xfs_buf *bp)
{
	ASSERT(!(bp->b_flags & XBF_ASYNC));
	buf_ioend(bp);
	return bp->b_error;
}

void xfs_buf_submit(struct xfs_buf *bp)
{
	buf_submit(bp);
}

int _xfs_buf_read(struct xfs_buf *bp)
{
	ASSERT(bp->b_maps[0].bm_bn != XFS_BUF_DADDR_NULL);
	clear_flags(bp, XBF_WRITE | XBF_ASYNC | XBF_READ_AHEAD | XBF_DONE);
	set_flags(bp, XBF_READ);
	buf_submit(bp);
	return xfs_buf_iowait(bp);
}

int xfs_bwrite(struct xfs_buf *bp)
{
	int error;

	ASSERT(xfs_buf_islocked(bp));
	set_flags(bp, XBF_WRITE);
	clear_flags(bp, XBF_ASYNC | XBF_READ | _XBF_DELWRI_Q | XBF_DONE);
	buf_submit(bp);
	error = xfs_buf_iowait(bp);
	if (error)
		buf_force_shutdown(bp, SHUTDOWN_META_IO_ERROR);
	return error;
}

/* Fail a locked buffer's pending I/O (log shutdown paths). */
void xfs_buf_fail(struct xfs_buf *bp)
{
	ASSERT(xfs_buf_islocked(bp));
	set_flags(bp, XBF_ASYNC);
	clear_flags(bp, XBF_DONE);
	xfs_buf_stale(bp);
	xfs_buf_ioerror(bp, -EIO);
	buf_ioend(bp);
}

/* ---- xfs_buf API ------------------------------------------------------------ */

int xfs_buf_get_map(struct xfs_buftarg *target, struct xfs_buf_map *map,
		int nmaps, xfs_buf_flags_t flags, struct xfs_buf **bpp)
{
	struct wfs_xbuf *wb;
	int err;

	*bpp = NULL;
	err = buf_find(target, map, nmaps, flags, &wb);
	if (err)
		return err;
	err = buf_find_lock(&wb->b, flags);
	if (err) {
		xfs_buf_rele(&wb->b);
		return err;
	}
	xfs_buf_ioerror(&wb->b, 0);
	*bpp = &wb->b;
	return 0;
}

int xfs_buf_read_map(struct xfs_buftarg *target, struct xfs_buf_map *map,
		int nmaps, xfs_buf_flags_t flags, struct xfs_buf **bpp,
		const struct xfs_buf_ops *ops, xfs_failaddr_t fa)
{
	struct wfs_xbuf *wb;
	struct xfs_buf *bp;
	int error;

	(void)fa;
	*bpp = NULL;
	flags &= XBF_TRYLOCK;
	error = buf_find(target, map, nmaps, flags, &wb);
	if (error)
		return error;
	bp = &wb->b;
	error = buf_find_lock(bp, flags);
	if (error) {
		xfs_buf_rele(bp);
		return error;
	}
	if (bp->b_flags & XBF_DONE) {
		if (ops && !bp->b_ops) {
			error = run_verify(bp, ops);
			if (error)
				clear_flags(bp, XBF_DONE);
			else
				wb->vops = ops;
		} else if (ops && bp->b_ops != ops && wb->vops != ops && !bp->b_log_item &&
			   !(bp->b_flags & _XBF_DELWRI_Q)) {
			/*
			 * A logged or queued buffer may be modified in core: its CRC is only
			 * set when it is written, so (like the kernel's xfs_buf_reverify,
			 * which checks only a buffer without ops) it is not verified here.
			 */
			error = verify_copy(bp, ops);
			if (error) {	/* the cached copy stays good for its own type */
				xfs_buf_relse(bp);
				return error == -EFSBADCRC ? -EFSCORRUPTED : error;
			}
			wb->vops = ops;
		}
	} else {
		bp->b_ops = ops;
		error = _xfs_buf_read(bp);
		if (!error)
			wb->vops = ops;
	}
	if (error)
		goto out_ioerror;
	*bpp = bp;
	return 0;

out_ioerror:
	if (!buf_shutdown(bp) && error == -EIO)
		xfs_buf_ioerror_alert(bp, fa);
	clear_flags(bp, XBF_DONE);
	bp->b_ops = NULL;
	wb->vops = NULL;
	xfs_buf_stale(bp);
	xfs_buf_relse(bp);
	return error == -EFSBADCRC ? -EFSCORRUPTED : error;
}

void xfs_buf_readahead_map(struct xfs_buftarg *target, struct xfs_buf_map *map,
		int nmaps, const struct xfs_buf_ops *ops)
{
	(void)target; (void)map; (void)nmaps; (void)ops;	/* design 7.1: no-op */
}

/* An uncached buffer, returned locked (kernel). */
int xfs_buf_get_uncached(struct xfs_buftarg *target, size_t numblks,
		struct xfs_buf **bpp)
{
	struct xfs_buf_map m = { .bm_bn = XFS_BUF_DADDR_NULL, .bm_len = (int)numblks };
	struct wfs_xbuf *wb;
	int len;

	*bpp = NULL;
	if (numblks == 0 || numblks > MAX_BUF_BB)
		return -EINVAL;	/* before any narrowing to int */
	len = check_maps(target, &(struct xfs_buf_map){ .bm_bn = 0, .bm_len = (int)numblks },
			 1, false);
	if (len < 0)
		return len;
	wb = alloc_buf_retry(BD(target)->c, target, &m, 1, len);
	if (!wb)
		return -ENOMEM;
	wb->shard = (unsigned int)(((uintptr_t)wb >> 6) % NSHARD);
	xfs_buf_lock(&wb->b);
	*bpp = &wb->b;
	return 0;
}

int xfs_buf_read_uncached(struct xfs_buftarg *target, xfs_daddr_t daddr,
		size_t numblks, struct xfs_buf **bpp,
		const struct xfs_buf_ops *ops)
{
	struct xfs_buf *bp;
	int err;

	*bpp = NULL;
	if (numblks == 0 || numblks > MAX_BUF_BB)
		return -EINVAL;
	err = check_maps(target, &(struct xfs_buf_map){ .bm_bn = daddr, .bm_len = (int)numblks },
			 1, true);
	if (err < 0)
		return err;
	err = xfs_buf_get_uncached(target, numblks, &bp);
	if (err)
		return err;
	bp->b_rhash_key = XFS_BUF_DADDR_NULL;
	bp->b_maps[0].bm_bn = daddr;
	set_flags(bp, XBF_READ);
	bp->b_ops = ops;
	buf_submit(bp);
	err = xfs_buf_iowait(bp);
	if (err) {
		xfs_buf_relse(bp);
		return err == -EFSBADCRC ? -EFSCORRUPTED : err;
	}
	*bpp = bp;
	return 0;
}

/* ---- state ------------------------------------------------------------------ */

void xfs_buf_set_uptodate(struct xfs_buf *bp) { set_flags(bp, XBF_DONE); }

/* Kernel: stale buffers stay hashed (a lookup recycles them) and are freed
 * at their last release instead of going to the LRU. */
void xfs_buf_stale(struct xfs_buf *bp)
{
	struct wfs_xbuf *wb = WB(bp);
	struct bshard *s = &wb->c->shard[wb->shard];

	ASSERT(xfs_buf_islocked(bp));
	set_flags(bp, XBF_STALE);
	clear_flags(bp, _XBF_DELWRI_Q);
	mutex_lock(&s->lock);
	if (wb->onlru) {	/* cannot be: the caller holds a reference */
		mutex_lock(&wb->c->lru_lock);
		lru_unlink(&wb->lru);
		mutex_unlock(&wb->c->lru_lock);
		wb->onlru = false;
	}
	mutex_unlock(&s->lock);
}

void xfs_buf_clear_stale(struct xfs_buf *bp)
{
	ASSERT(bp->b_flags & XBF_STALE);
	clear_flags(bp, XBF_STALE);
}

void xfs_buf_set_ref(struct xfs_buf *bp, int lru_ref) { (void)bp; (void)lru_ref; }

void __xfs_buf_ioerror(struct xfs_buf *bp, int error, xfs_failaddr_t failaddr)
{
	(void)failaddr;
	bp->b_error = error;
}

void xfs_buf_ioerror_alert(struct xfs_buf *bp, xfs_failaddr_t fa)
{
	(void)fa;
	xfs_alert(bp->b_mount, "metadata I/O error at daddr 0x%llx len %d error %d",
		  (unsigned long long)xfs_buf_daddr(bp), bp->b_length, bp->b_error);
}

void __xfs_buf_mark_corrupt(struct xfs_buf *bp, xfs_failaddr_t fa)
{
	(void)fa;
	bp->b_error = -EFSCORRUPTED;
	xfs_buf_stale(bp);
}

void xfs_buf_corruption_error(struct xfs_buf *bp, xfs_failaddr_t fa)
{
	xfs_alert(bp->b_mount, "Metadata corruption detected at %p, block 0x%llx",
		  fa, (unsigned long long)xfs_buf_daddr(bp));
	bp->b_error = -EFSCORRUPTED;
}

/*
 * Real libxfs verifiers report corruption only through these and rely on them
 * to leave the error in b_error, which run_verify() turns into a failed read.
 */
void xfs_verifier_error(struct xfs_buf *bp, int error, xfs_failaddr_t failaddr)
{
	if (error > 0)
		error = -error;
	xfs_alert(bp->b_mount, "Metadata %s detected at %p, block 0x%llx",
		  error == -EFSBADCRC ? "CRC error" : "corruption", failaddr,
		  (unsigned long long)xfs_buf_daddr(bp));
	if (error)
		bp->b_error = error;
}

void xfs_buf_verifier_error(struct xfs_buf *bp, int error, const char *name,
		const void *buf, size_t bufsz, xfs_failaddr_t failaddr)
{
	(void)buf; (void)bufsz;
	if (error > 0)
		error = -error;
	xfs_alert(bp->b_mount, "Metadata %s detected at %p (%s), block 0x%llx",
		  error == -EFSBADCRC ? "CRC error" : "corruption", failaddr,
		  name ? name : "?", (unsigned long long)xfs_buf_daddr(bp));
	if (error)
		bp->b_error = error;
}

/* A buftarg without a mount is only used for v5 metadata here: assume CRC. */
static int buf_magic_idx(const struct xfs_buf *bp)
{
	return bp->b_mount ? xfs_has_crc(bp->b_mount) : 1;
}

bool xfs_verify_magic(struct xfs_buf *bp, __be32 dmagic)
{
	int idx = buf_magic_idx(bp);

	if (!bp->b_ops || !bp->b_ops->magic[idx])
		return false;
	return dmagic == bp->b_ops->magic[idx];
}

bool xfs_verify_magic16(struct xfs_buf *bp, __be16 dmagic)
{
	int idx = buf_magic_idx(bp);

	if (!bp->b_ops || !bp->b_ops->magic16[idx])
		return false;
	return dmagic == bp->b_ops->magic16[idx];
}

/* ---- delayed write lists (kernel semantics) ------------------------------- */

static void buf_list_del(struct xfs_buf *bp)
{
	list_del_init(&bp->b_list);
	wake_up_var(&bp->b_list);
}

void xfs_buf_delwri_cancel(struct list_head *list)
{
	struct xfs_buf *bp;

	while (!list_empty(list)) {
		bp = list_first_entry(list, struct xfs_buf, b_list);
		xfs_buf_lock(bp);
		clear_flags(bp, _XBF_DELWRI_Q);
		buf_list_del(bp);
		xfs_buf_relse(bp);
	}
}

bool xfs_buf_delwri_queue(struct xfs_buf *bp, struct list_head *list)
{
	ASSERT(xfs_buf_islocked(bp));
	ASSERT(!(bp->b_flags & XBF_READ));
	if (bp->b_flags & _XBF_DELWRI_Q)
		return false;
	set_flags(bp, _XBF_DELWRI_Q);
	if (list_empty(&bp->b_list)) {
		xfs_buf_hold(bp);
		list_add_tail(&bp->b_list, list);
	}
	return true;
}

void xfs_buf_delwri_queue_here(struct xfs_buf *bp, struct list_head *buffer_list)
{
	while (!list_empty(&bp->b_list)) {
		xfs_buf_unlock(bp);
		wait_var_event(&bp->b_list, list_empty(&bp->b_list));
		xfs_buf_lock(bp);
	}
	ASSERT(!(bp->b_flags & _XBF_DELWRI_Q));
	xfs_buf_delwri_queue(bp, buffer_list);
}

static int buf_cmp(void *priv, const struct list_head *a, const struct list_head *b)
{
	struct xfs_buf *ap = container_of(a, struct xfs_buf, b_list);
	struct xfs_buf *bp = container_of(b, struct xfs_buf, b_list);
	xfs_daddr_t diff = ap->b_maps[0].bm_bn - bp->b_maps[0].bm_bn;

	(void)priv;
	return diff < 0 ? -1 : diff > 0;
}

static bool delwri_submit_prep(struct xfs_buf *bp)
{
	if (!(bp->b_flags & _XBF_DELWRI_Q)) {	/* cancelled (staled) meanwhile */
		buf_list_del(bp);
		xfs_buf_relse(bp);
		return false;
	}
	clear_flags(bp, _XBF_DELWRI_Q);
	set_flags(bp, XBF_WRITE);
	return true;
}

int xfs_buf_delwri_submit_nowait(struct list_head *buffer_list)
{
	struct xfs_buf *bp, *n;
	int pinned = 0;

	list_sort(NULL, buffer_list, buf_cmp);
	list_for_each_entry_safe(bp, n, buffer_list, b_list) {
		if (!xfs_buf_trylock(bp))
			continue;
		if (xfs_buf_ispinned(bp)) {
			xfs_buf_unlock(bp);
			pinned++;
			continue;
		}
		if (!delwri_submit_prep(bp))
			continue;
		set_flags(bp, XBF_ASYNC);
		buf_list_del(bp);
		buf_submit(bp);
	}
	return pinned;
}

int xfs_buf_delwri_submit(struct list_head *buffer_list)
{
	LIST_HEAD(wait_list);
	int error = 0, error2;
	struct xfs_buf *bp, *n;

	list_sort(NULL, buffer_list, buf_cmp);
	list_for_each_entry_safe(bp, n, buffer_list, b_list) {
		xfs_buf_lock(bp);
		if (!delwri_submit_prep(bp))
			continue;
		clear_flags(bp, XBF_ASYNC);
		list_move_tail(&bp->b_list, &wait_list);
		buf_submit(bp);
	}
	while (!list_empty(&wait_list)) {
		bp = list_first_entry(&wait_list, struct xfs_buf, b_list);
		buf_list_del(bp);
		error2 = xfs_buf_iowait(bp);
		xfs_buf_relse(bp);
		if (!error)
			error = error2;
	}
	return error;
}

/* ---- buftarg ---------------------------------------------------------------- */

/* No read-ahead and no I/O completion work queue: nothing is in flight. */
void xfs_buftarg_wait(struct xfs_buftarg *btp) { (void)btp; }

/* Drop every unreferenced buffer of the cache (log quiesce, unmount). */
void xfs_buftarg_drain(struct xfs_buftarg *btp)
{
	wfs_xfs_bcache_purge(BD(btp)->c);
}

/* ---- buftarg binding ---------------------------------------------------------- */

int wfs_xfs_buftarg_bind(struct xfs_buftarg *bt, struct wfs_xfs_bcache *c,
		int role)
{
	struct wfs_bdev *bd;
	uint32_t ss;

	if (role < 0 || role >= WFS_DEV_COUNT)
		return -EINVAL;
	bd = kzalloc(sizeof(*bd), 0);
	if (!bd)
		return -ENOMEM;
	bd->c = c;
	bd->role = role;
	bd->bdev.wfs_blk = c->blk;
	bd->bdev.wfs_role = role;
	bd->bdev.wfs_read_only = wfs_xfs_blk_read_only(c->blk, role);
	ss = c->blk->sector_size[role] ? c->blk->sector_size[role] : 512;
	bt->bt_priv = bd;
	bt->bt_bdev = &bd->bdev;
	bt->bt_meta_sectorsize = ss;
	bt->bt_meta_sectormask = ss - 1;
	bt->bt_logical_sectorsize = ss;
	bt->bt_logical_sectormask = ss - 1;
	bt->bt_nr_sectors = (xfs_daddr_t)(c->blk->size_bytes[role] >> 9);
	return 0;
}

void wfs_xfs_buftarg_set_read_only(struct xfs_buftarg *bt)
{
	BD(bt)->bdev.wfs_read_only = true;
}

void wfs_xfs_buftarg_unbind(struct xfs_buftarg *bt)
{
	kfree(bt->bt_priv);
	bt->bt_priv = NULL;
	bt->bt_bdev = NULL;
}
