// SPDX-License-Identifier: GPL-2.0-only
/*
 * XFS buffer cache for the read-only port (replaces fs/xfs/xfs_buf.c).
 * Design: docs/superpowers/specs/2026-10-29-m2-xfs-readonly-design.md 7.1.
 *
 * Buffer life cycle (state, protected by the buffer's shard lock):
 *   EMPTY    created by xfs_buf_get_map, no data read yet
 *   READING  one thread is doing I/O and/or running the verifier; every other
 *            thread that wants the buffer sleeps on the shard condvar
 *            (single-flight). The buffer is in the hash but unusable.
 *   VALID    data present and verified (against vops); immutable from here on
 *   FAILED   I/O or verifier failed: removed from the hash at once (never
 *            cached), error kept for the waiters, freed with the last ref
 *
 * Locks (lock order: shard.lock -> lru_lock):
 *   shard.lock  one of NSHARD mutexes; guards its hash chains plus every
 *               field of the buffers hashed in it (state, refs, onlru, hashed,
 *               err, vops) and the shard condvar's predicate
 *   lru_lock    guards the LRU list and its links
 * The evictor holds lru_lock and takes a shard lock only with trylock (the
 * opposite order of the normal one), so it cannot deadlock; a busy shard is
 * skipped. Invariant: a buffer is on the LRU iff hashed, refs == 0 and state
 * is VALID or EMPTY, so a buffer in use is never evicted.
 * usage (bytes of buffer data held, in-use ones included) is atomic; the limit
 * is soft: pinned buffers can push usage above it until they are released.
 * Memory comes from the shim allocator, so wfs_mem_set_cap is honoured.
 */
#include "buf.h"
#include "xfs_format.h"
#include "xfs_log_format.h"
#include "xfs_shared.h"
#include "xfs_trans_resv.h"
#include "xfs_mount.h"

#define NSHARD		64
#define NBUCKET		512		/* per shard */
#define MAX_BUF_BB	8192		/* 4 MiB sanity bound on one buffer */

#ifdef _WIN32
typedef struct { void *p; } wfs_cv_t;
void InitializeConditionVariable(wfs_cv_t *);
void WakeAllConditionVariable(wfs_cv_t *);
int SleepConditionVariableSRW(wfs_cv_t *, wfs_srw_t *, unsigned long, unsigned long);
static void cv_init(wfs_cv_t *c) { InitializeConditionVariable(c); }
static void cv_wait(wfs_cv_t *c, struct mutex *m) { SleepConditionVariableSRW(c, &m->l, 0xFFFFFFFFul, 0); }
static void cv_broadcast(wfs_cv_t *c) { WakeAllConditionVariable(c); }
static void cv_destroy(wfs_cv_t *c) { (void)c; }
#else
typedef pthread_cond_t wfs_cv_t;
static void cv_init(wfs_cv_t *c) { pthread_cond_init(c, NULL); }
static void cv_wait(wfs_cv_t *c, struct mutex *m) { pthread_cond_wait(c, &m->l); }
static void cv_broadcast(wfs_cv_t *c) { pthread_cond_broadcast(c); }
static void cv_destroy(wfs_cv_t *c) { pthread_cond_destroy(c); }
#endif

enum { ST_EMPTY, ST_READING, ST_VALID, ST_FAILED };

struct lru_node { struct lru_node *prev, *next; };

struct wfs_xbuf {
	struct xfs_buf		b;	/* first: xfs_buf * == wfs_xbuf * */
	struct wfs_xbuf		*hnext;
	struct lru_node		lru;
	struct wfs_xfs_bcache	*c;
	const struct xfs_buf_ops *vops;	/* ops the data was verified against */
	size_t			bytes;
	int			state;
	int			refs;
	int			err;		/* valid in FAILED */
	unsigned int		shard;
	bool			hashed;
	bool			onlru;
};

struct bshard {
	struct mutex	lock;
	wfs_cv_t	cv;
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
};

static inline struct wfs_xbuf *WB(struct xfs_buf *bp) { return (struct wfs_xbuf *)bp; }
static inline struct wfs_bdev *BD(struct xfs_buftarg *t) { return t->bt_priv; }

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
	for (i = 0; i < NSHARD; i++) {
		mutex_init(&c->shard[i].lock);
		cv_init(&c->shard[i].cv);
	}
	return c;
}

void wfs_xfs_bcache_destroy(struct wfs_xfs_bcache *c)
{
	int i;

	if (!c)
		return;
	wfs_xfs_bcache_purge(c);
	for (i = 0; i < NSHARD; i++) {
		mutex_destroy(&c->shard[i].lock);
		cv_destroy(&c->shard[i].cv);
	}
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
	__atomic_sub_fetch(&wb->c->usage, wb->bytes, __ATOMIC_RELAXED);
	if (wb->b.b_maps != &wb->b.__b_map)
		kfree(wb->b.b_maps);
	kfree(wb->b.b_addr);
	kfree(wb);
}

/* Evict unreferenced buffers, LRU tail first, until usage <= target. */
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

static struct wfs_xbuf *alloc_buf(struct wfs_xfs_bcache *c,
		struct xfs_buftarg *t, const struct xfs_buf_map *m, int nmaps,
		int len)
{
	struct wfs_xbuf *wb = kzalloc(sizeof(*wb), 0);
	int i;

	if (!wb)
		return NULL;
	wb->bytes = (size_t)len << 9;
	wb->b.b_addr = kzalloc(wb->bytes, 0);
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
	wb->c = c;
	wb->refs = 1;
	wb->state = ST_EMPTY;
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
static struct wfs_xbuf *find_locked(struct bshard *s, unsigned int h,
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
		return wb;
	}
	return NULL;
}

static int buf_get(struct xfs_buftarg *t, const struct xfs_buf_map *m,
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
	wb = find_locked(s, h, t, m, nmaps, len);
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
	wb = find_locked(s, h, t, m, nmaps, len);	/* lost a race? */
	if (!wb) {
		struct wfs_xbuf **head = &s->bucket[bucket_of(h)];

		nw->hnext = *head;
		*head = nw;
		nw->hashed = true;
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

/* ---- I/O and verification ------------------------------------------------- */

static int buf_io(struct wfs_xbuf *wb)
{
	struct xfs_buftarg *t = wb->b.b_target;
	struct wfs_xfs_bcache *c = BD(t)->c;
	size_t pos = 0;
	int i;

	for (i = 0; i < wb->b.b_map_count; i++) {
		size_t n = (size_t)wb->b.b_maps[i].bm_len << 9;

		if (wfs_xfs_blk_read(c->blk, BD(t)->role,
				     (uint64_t)wb->b.b_maps[i].bm_bn << 9,
				     (char *)wb->b.b_addr + pos, n) != 0)
			return -EIO;
		pos += n;
	}
	return 0;
}

/* Runs b_ops->verify_read; the error is whatever the verifier left in b_error. */
static int run_verify(struct wfs_xbuf *wb, const struct xfs_buf_ops *ops)
{
	int err;

	wb->b.b_error = 0;
	if (ops && ops->verify_read) {
		wb->b.b_ops = ops;
		ops->verify_read(&wb->b);
	}
	err = wb->b.b_error;
	wb->b.b_error = 0;
	return err > 0 ? -err : err;
}

/* shard lock held; drops one reference */
static bool put_locked(struct wfs_xbuf *wb)
{
	struct wfs_xfs_bcache *c = wb->c;

	if (--wb->refs > 0)
		return false;
	if (!wb->hashed || wb->state == ST_FAILED ||
	    (wb->b.b_flags & XBF_STALE)) {
		if (wb->hashed)
			unhash(wb);
		return true;			/* caller frees */
	}
	mutex_lock(&c->lru_lock);
	lru_push_mru(c, &wb->lru);
	mutex_unlock(&c->lru_lock);
	wb->onlru = true;
	return false;
}

/*
 * Bring wb to VALID verified against ops, doing at most one read system-wide
 * (single-flight). Consumes the caller's reference on failure.
 */
static int buf_fill(struct wfs_xbuf *wb, const struct xfs_buf_ops *ops)
{
	struct bshard *s = &wb->c->shard[wb->shard];
	bool freeit = false;
	int err;

	mutex_lock(&s->lock);
	for (;;) {
		if (wb->state == ST_READING) {
			cv_wait(&s->cv, &s->lock);
			continue;
		}
		if (wb->state == ST_FAILED) {
			err = wb->err;
			goto drop;
		}
		if (wb->state == ST_VALID && (!ops || wb->vops == ops)) {
			mutex_unlock(&s->lock);
			return 0;
		}
		break;
	}

	if (wb->state == ST_EMPTY) {
		wb->state = ST_READING;
		mutex_unlock(&s->lock);
		err = buf_io(wb);
		if (!err)
			err = run_verify(wb, ops);
		mutex_lock(&s->lock);
		if (err) {
			wb->state = ST_FAILED;	/* never cached */
			wb->err = err;
			if (wb->hashed)
				unhash(wb);
			wb->b.b_ops = NULL;
		} else {
			wb->state = ST_VALID;
			wb->vops = ops;
			wb->b.b_flags |= XBF_DONE;
		}
	} else {
		/* VALID but verified against other ops: re-verify, as the kernel does */
		const struct xfs_buf_ops *old = wb->b.b_ops;

		wb->state = ST_READING;
		mutex_unlock(&s->lock);
		err = run_verify(wb, ops);
		mutex_lock(&s->lock);
		wb->state = ST_VALID;
		if (err)
			wb->b.b_ops = old;	/* data unchanged; stays cached */
		else
			wb->vops = ops;
	}
	cv_broadcast(&s->cv);
	if (!err) {
		mutex_unlock(&s->lock);
		return 0;
	}
drop:
	freeit = put_locked(wb);
	mutex_unlock(&s->lock);
	if (freeit)
		free_buf(wb);
	return err;
}

/* ---- xfs_buf API ------------------------------------------------------------ */

int xfs_buf_get_map(struct xfs_buftarg *target, struct xfs_buf_map *map,
		int nmaps, xfs_buf_flags_t flags, struct xfs_buf **bpp)
{
	struct wfs_xbuf *wb;
	int err = buf_get(target, map, nmaps, flags, &wb);

	*bpp = err ? NULL : &wb->b;
	return err;
}

int xfs_buf_read_map(struct xfs_buftarg *target, struct xfs_buf_map *map,
		int nmaps, xfs_buf_flags_t flags, struct xfs_buf **bpp,
		const struct xfs_buf_ops *ops, xfs_failaddr_t fa)
{
	struct wfs_xbuf *wb;
	int err;

	(void)fa;
	*bpp = NULL;
	err = buf_get(target, map, nmaps, flags & ~XBF_INCORE, &wb);
	if (err)
		return err;
	err = buf_fill(wb, ops);
	if (err)
		return err;
	*bpp = &wb->b;
	return 0;
}

void xfs_buf_readahead_map(struct xfs_buftarg *target, struct xfs_buf_map *map,
		int nmaps, const struct xfs_buf_ops *ops)
{
	(void)target; (void)map; (void)nmaps; (void)ops;	/* design 7.1: no-op */
}

int xfs_buf_get_uncached(struct xfs_buftarg *target, size_t numblks,
		struct xfs_buf **bpp)
{
	struct xfs_buf_map m = { .bm_bn = XFS_BUF_DADDR_NULL, .bm_len = (int)numblks };
	struct wfs_xbuf *wb;
	int len = check_maps(target, &(struct xfs_buf_map){ .bm_bn = 0, .bm_len = (int)numblks },
			     1, false);

	*bpp = NULL;
	if (len < 0)
		return len;
	wb = alloc_buf_retry(BD(target)->c, target, &m, 1, len);
	if (!wb)
		return -ENOMEM;
	wb->shard = (unsigned int)(((uintptr_t)wb >> 6) % NSHARD);
	wb->state = ST_VALID;
	*bpp = &wb->b;
	return 0;
}

int xfs_buf_read_uncached(struct xfs_buftarg *target, xfs_daddr_t daddr,
		size_t numblks, struct xfs_buf **bpp,
		const struct xfs_buf_ops *ops)
{
	struct xfs_buf *bp;
	struct wfs_xbuf *wb;
	int err = check_maps(target, &(struct xfs_buf_map){ .bm_bn = daddr, .bm_len = (int)numblks },
			     1, true);

	*bpp = NULL;
	if (err < 0)
		return err;
	err = xfs_buf_get_uncached(target, numblks, &bp);
	if (err)
		return err;
	wb = WB(bp);
	bp->b_maps[0].bm_bn = daddr;
	bp->b_rhash_key = daddr;
	err = buf_io(wb);
	if (!err)
		err = run_verify(wb, ops);
	if (err) {
		xfs_buf_rele(bp);
		return err;
	}
	wb->vops = ops;
	bp->b_flags |= XBF_DONE;
	*bpp = bp;
	return 0;
}

/* Re-read into an existing buffer (verifying against its b_ops). */
int _xfs_buf_read(struct xfs_buf *bp)
{
	struct wfs_xbuf *wb = WB(bp);
	int err = buf_io(wb);

	if (!err)
		err = run_verify(wb, bp->b_ops);
	if (err)
		bp->b_flags &= ~XBF_DONE;
	else
		bp->b_flags |= XBF_DONE;
	return err;
}

void xfs_buf_hold(struct xfs_buf *bp)
{
	struct wfs_xbuf *wb = WB(bp);
	struct bshard *s = &wb->c->shard[wb->shard];

	mutex_lock(&s->lock);
	wb->refs++;
	mutex_unlock(&s->lock);
}

void xfs_buf_rele(struct xfs_buf *bp)
{
	struct wfs_xbuf *wb = WB(bp);
	struct wfs_xfs_bcache *c = wb->c;
	struct bshard *s = &c->shard[wb->shard];
	bool freeit;

	mutex_lock(&s->lock);
	freeit = put_locked(wb);
	mutex_unlock(&s->lock);
	if (freeit)
		free_buf(wb);
	else
		evict_to_limit(c);
}

void xfs_buf_stale(struct xfs_buf *bp)
{
	struct wfs_xbuf *wb = WB(bp);
	struct bshard *s = &wb->c->shard[wb->shard];

	mutex_lock(&s->lock);
	bp->b_flags |= XBF_STALE;
	if (wb->hashed)
		unhash(wb);	/* later lookups make a fresh buffer */
	mutex_unlock(&s->lock);
}

void xfs_buf_clear_stale(struct xfs_buf *bp) { bp->b_flags &= ~XBF_STALE; }
void xfs_buf_set_uptodate(struct xfs_buf *bp) { bp->b_flags |= XBF_DONE; }
void xfs_buf_set_ref(struct xfs_buf *bp, int lru_ref) { (void)bp; (void)lru_ref; }
void xfs_buf_fail(struct xfs_buf *bp) { (void)bp; }

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

/* Read-only mount: nothing is ever written. */
int xfs_bwrite(struct xfs_buf *bp) { (void)bp; return -EROFS; }
int xfs_buf_delwri_submit(struct list_head *l) { (void)l; return -EROFS; }
bool xfs_buf_delwri_queue(struct xfs_buf *bp, struct list_head *l)
{
	(void)bp; (void)l;
	return false;
}

bool xfs_verify_magic(struct xfs_buf *bp, __be32 dmagic)
{
	int idx = xfs_has_crc(bp->b_mount);

	if (!bp->b_ops || !bp->b_ops->magic[idx])
		return false;
	return dmagic == bp->b_ops->magic[idx];
}

bool xfs_verify_magic16(struct xfs_buf *bp, __be16 dmagic)
{
	int idx = xfs_has_crc(bp->b_mount);

	if (!bp->b_ops || !bp->b_ops->magic16[idx])
		return false;
	return dmagic == bp->b_ops->magic16[idx];
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
	ss = c->blk->sector_size[role] ? c->blk->sector_size[role] : 512;
	bt->bt_priv = bd;
	bt->bt_meta_sectorsize = ss;
	bt->bt_meta_sectormask = ss - 1;
	bt->bt_logical_sectorsize = ss;
	bt->bt_logical_sectormask = ss - 1;
	bt->bt_nr_sectors = (xfs_daddr_t)(c->blk->size_bytes[role] >> 9);
	return 0;
}

void wfs_xfs_buftarg_unbind(struct xfs_buftarg *bt)
{
	kfree(bt->bt_priv);
	bt->bt_priv = NULL;
}
