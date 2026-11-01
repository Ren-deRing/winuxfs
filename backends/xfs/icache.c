// SPDX-License-Identifier: GPL-2.0-only
/*
 * Inode cache and the inode lock functions (Task 8, design 7.2).
 * The load path mirrors xfs_iget_cache_miss() (xfs_icache.c): xfs_imap()
 * with XFS_IGET_UNTRUSTED, xfs_read_icluster(), xfs_inode_from_disk(), the
 * free-state check; teardown mirrors xfs_inode_free_callback(). See icache.h
 * for the reference and locking rules.
 */
#include "icache.h"
#include <stdlib.h>
#include "kinit.h"
#include "xfs_bit.h"
#include "xfs_types.h"
#include "xfs_ag.h"
#include "xfs_ialloc.h"
#include "xfs_btree.h"
#include "xfs_alloc_btree.h"
#include "xfs_ialloc_btree.h"
#include "xfs_bmap_btree.h"
#include "xfs_rmap_btree.h"
#include "xfs_refcount_btree.h"
#include "xfs_inode_buf.h"
#include "xfs_inode_fork.h"
#include "xfs_trans.h"
#include "xfs_da_format.h"
#include "xfs_da_btree.h"
#include "xfs_metafile.h"
#include "xfs_error.h"

#include "xfs_icache.h"
#include "xfs_inode_item.h"

struct ic_shard;

struct ic_ent {
	struct xfs_inode	ip;		/* first: xfs_inode <-> ic_ent */
	struct ic_ent		*hnext;		/* hash chain */
	struct ic_ent		*lprev, *lnext;	/* LRU links (valid if on_lru) */
	struct ic_shard		*sh;
	uint32_t		refs;		/* transient references */
	uint64_t		nlookup;	/* host lookup references */
	int			state;		/* ENT_*; waiters: wait_var_event(&state) */
	int			err;		/* ENT_FAILED: the load error */
	bool			pinned;
	bool			on_lru;
	bool			dead;		/* freed on disk (inactive ran): drop when idle */
};

/*
 * A miss inserts the entry LOADING and drops the shard lock for the disk read:
 * the read takes buffer locks (AGI, inode cluster), and a transaction holding
 * those buffers may call xfs_iget for an inode of the same shard. Lookups that
 * find a LOADING entry take a reference and wait for READY or FAILED, so one
 * read serves all of them. A FAILED entry is unhashed at once (never cached).
 */
enum { ENT_LOADING, ENT_READY, ENT_FAILED };

struct ic_shard {
	spinlock_t		lock;
	struct ic_ent		**bucket;
	size_t			count;
	struct ic_ent		*lru_head;	/* most recently released */
	struct ic_ent		*lru_tail;	/* eviction candidate */
};

struct wfs_xfs_icache {
	size_t			nbuckets;	/* per shard, power of two */
	size_t			shard_limit;
	struct ic_shard		sh[WFS_IC_SHARDS];
};

static inline struct ic_ent *ent_of(struct xfs_inode *ip)
{
	return container_of(ip, struct ic_ent, ip);
}

static inline struct wfs_xfs_icache *ic_of(struct wfs_xfs *x)
{
	return (struct wfs_xfs_icache *)x->icache;
}

/* ---------------------------------------------------------- entries -- */

static struct ic_ent *ent_alloc(struct xfs_mount *mp, xfs_ino_t ino)
{
	struct ic_ent *e = kzalloc(sizeof(*e), GFP_KERNEL);
	struct xfs_inode *ip;

	if (!e)
		return NULL;
	ip = &e->ip;
	ip->i_mount = mp;
	VFS_I(ip)->i_ino = ino;
	VFS_I(ip)->i_mode = 0;
	ip->i_af.if_format = XFS_DINODE_FMT_EXTENTS;
	ip->i_diflags2 = mp->m_ino_geo.new_diflags2;
	ip->i_next_unlinked = NULLAGINO;
	init_rwsem(&ip->i_lock);
	spin_lock_init(&ip->i_flags_lock);
	VFS_I(ip)->i_sb = mp->m_super;
	init_rwsem(&VFS_I(ip)->i_rwsem);
	VFS_I(ip)->i_mapping = &VFS_I(ip)->i_data;
	init_rwsem(&VFS_I(ip)->i_data.invalidate_lock);
	return e;
}

static void ent_free(struct ic_ent *e)
{
	struct xfs_inode *ip = &e->ip;

	switch (VFS_I(ip)->i_mode & S_IFMT) {
	case S_IFREG:
	case S_IFDIR:
	case S_IFLNK:
		xfs_idestroy_fork(&ip->i_df);
		break;
	}
	xfs_ifork_zap_attr(ip);
	if (ip->i_itemp) {
		/* an inode-buffer completion on the AIL thread may still be finishing this
		 * inode's flush (it clears IFLUSHING last): wait it out, which also orders
		 * the frees below after its accesses */
		for (;;) {
			bool flushing;

			spin_lock(&ip->i_flags_lock);
			flushing = ip->i_flags & XFS_IFLUSHING;
			spin_unlock(&ip->i_flags_lock);
			if (!flushing)
				break;
			msleep(1);
		}
		xfs_inode_item_destroy(ip);
	}
	if (ip->i_cowfp) {
		xfs_idestroy_fork(ip->i_cowfp);
		kmem_cache_free(xfs_ifork_cache, ip->i_cowfp);
	}
	destroy_rwsem(&ip->i_lock);
	destroy_rwsem(&VFS_I(ip)->i_rwsem);
	kfree(e);
}

/* At most 8M extents (~128 MiB of iext tree) per fork of one inode. */
#define WFS_XFS_MAX_EXTENTS	((uint64_t)8 << 20)

/* Load the on-disk inode into e. Kernel errno on failure. */
static int ent_load(struct wfs_xfs *x, struct ic_ent *e, struct xfs_trans *tp,
		    unsigned int flags)
{
	struct xfs_mount *mp = &x->mp;
	struct xfs_inode *ip = &e->ip;
	xfs_ino_t ino = I_INO(ip);
	struct xfs_perag *pag;
	struct xfs_buf *bp;
	int error;

	pag = xfs_perag_get(mp, XFS_INO_TO_AGNO(mp, ino));
	if (!pag)
		return -EINVAL;

	error = xfs_imap(pag, tp, ino, &ip->i_imap, flags | XFS_IGET_UNTRUSTED);
	if (error)
		goto out_put;
	/*
	 * A new inode (xfs_dialloc): nothing to read; the caller initializes it
	 * (xfs_iget_cache_miss: a v5 inode gets a random generation).
	 */
	if (flags & XFS_IGET_CREATE) {
		VFS_I(ip)->i_generation = get_random_u32();
		goto out_put;
	}
	error = xfs_read_icluster(pag, tp, ip->i_imap.im_agbno, &bp);
	if (error)
		goto out_put;
	error = xfs_inode_from_disk(ip, xfs_buf_offset(bp, ip->i_imap.im_boffset));
	if (!error)
		xfs_buf_set_ref(bp, XFS_INO_REF);
	xfs_trans_brelse(tp, bp);
	if (error)
		goto out_put;

	/*
	 * Ceiling on the in-memory extent map (design 7.2, review M2-final I2).
	 * di_nextents is only bounded by di_nblocks, so a crafted inode can claim
	 * billions of extents; the iext tree costs ~16 bytes each and the loader
	 * allocates with __GFP_NOFAIL. Refuse such an inode up front.
	 */
	if (!error && ((uint64_t)xfs_ifork_nextents(&ip->i_df) > WFS_XFS_MAX_EXTENTS ||
		       (uint64_t)xfs_ifork_nextents(&ip->i_af) > WFS_XFS_MAX_EXTENTS)) {
		xfs_warn(mp, "inode %llu: %llu extents exceed the %llu extent limit",
			 (unsigned long long)ino,
			 (unsigned long long)xfs_ifork_nextents(&ip->i_df),
			 (unsigned long long)WFS_XFS_MAX_EXTENTS);
		error = -EFSCORRUPTED;
		goto out_put;
	}

	/* should be an allocated inode (xfs_iget_check_free_state) */
	if (VFS_I(ip)->i_mode == 0)
		error = -ENOENT;
	else
		i_size_write(VFS_I(ip), ip->i_disk_size);	/* as xfs_setup_inode */
out_put:
	xfs_perag_put(pag);
	return error;
}

/* ------------------------------------------------------------ shard -- */

static inline uint64_t ino_hash(xfs_ino_t ino)
{
	return (uint64_t)ino * 0x9E3779B97F4A7C15ULL;
}

static inline struct ic_shard *shard_of(struct wfs_xfs_icache *c, uint64_t h)
{
	return &c->sh[(h >> 40) & (WFS_IC_SHARDS - 1)];
}

static inline size_t bucket_of(struct wfs_xfs_icache *c, uint64_t h)
{
	return (size_t)(h >> 8) & (c->nbuckets - 1);
}

static void lru_remove(struct ic_shard *s, struct ic_ent *e)
{
	if (!e->on_lru)
		return;
	if (e->lprev)
		e->lprev->lnext = e->lnext;
	else
		s->lru_head = e->lnext;
	if (e->lnext)
		e->lnext->lprev = e->lprev;
	else
		s->lru_tail = e->lprev;
	e->lprev = e->lnext = NULL;
	e->on_lru = false;
}

static void lru_push(struct ic_shard *s, struct ic_ent *e)
{
	e->lprev = NULL;
	e->lnext = s->lru_head;
	if (s->lru_head)
		s->lru_head->lprev = e;
	else
		s->lru_tail = e;
	s->lru_head = e;
	e->on_lru = true;
}

static inline bool evictable(const struct ic_ent *e)
{
	/* like kernel reclaim: no log item attached to a cluster buffer, not pinned in the log */
	struct xfs_inode *ip = (struct xfs_inode *)&e->ip;
	bool clean = true, flushing;

	if (e->refs || e->nlookup || e->pinned || atomic_read(&ip->i_pincount))
		return false;
	if (ip->i_itemp) {
		/* the flush completion unlinks the item under ili_lock, then clears IFLUSHING
		 * under i_flags_lock and touches nothing of the inode after that: taking both
		 * orders the free of the inode after that completion */
		spin_lock(&ip->i_flags_lock);
		flushing = ip->i_flags & XFS_IFLUSHING;
		spin_unlock(&ip->i_flags_lock);
		if (flushing)
			return false;
		spin_lock(&ip->i_itemp->ili_lock);
		clean = list_empty(&ip->i_itemp->ili_item.li_bio_list);
		spin_unlock(&ip->i_itemp->ili_lock);
	}
	return clean;
}

/* Called with the shard lock held. */
static void hash_unlink(struct wfs_xfs_icache *c, struct ic_shard *s, struct ic_ent *e)
{
	size_t b = bucket_of(c, ino_hash(I_INO(&e->ip)));
	struct ic_ent **pp = &s->bucket[b];

	while (*pp && *pp != e)
		pp = &(*pp)->hnext;
	if (*pp)
		*pp = e->hnext;
	s->count--;
}

/* Evict from the LRU tail while the shard is over its limit. Lock held. */
static void trim(struct wfs_xfs_icache *c, struct ic_shard *s)
{
	while (s->count > c->shard_limit && s->lru_tail) {
		struct ic_ent *e = s->lru_tail;

		lru_remove(s, e);
		hash_unlink(c, s, e);
		ent_free(e);
	}
}

/* Re-file e after a reference dropped. Lock held. */
static void release_locked(struct wfs_xfs_icache *c, struct ic_shard *s, struct ic_ent *e)
{
	if (e->dead && evictable(e)) {
		if (e->on_lru)
			lru_remove(s, e);
		hash_unlink(c, s, e);
		ent_free(e);
		return;
	}
	if (evictable(e) && !e->on_lru) {
		lru_push(s, e);
		trim(c, s);
	}
}

/* -------------------------------------------------------- public API -- */

int wfs_xfs_icache_create(struct wfs_xfs *x)
{
	struct wfs_xfs_icache *c;
	size_t per = x->opts.inodecache_entries / WFS_IC_SHARDS;
	size_t nb = 16, i;
	int error;

	if (per < 1)
		per = 1;
	while (nb < per && nb < ((size_t)1 << 16))
		nb <<= 1;
	error = wfs_xfs_kinit_get();
	if (error)
		return error;
	c = kzalloc(sizeof(*c), GFP_KERNEL);
	if (!c) {
		wfs_xfs_kinit_put();
		return -ENOMEM;
	}
	c->nbuckets = nb;
	c->shard_limit = per;
	for (i = 0; i < WFS_IC_SHARDS; i++) {
		spin_lock_init(&c->sh[i].lock);
		c->sh[i].bucket = kcalloc(nb, sizeof(*c->sh[i].bucket), GFP_KERNEL);
		if (!c->sh[i].bucket) {
			while (i--)
				kfree(c->sh[i].bucket);
			kfree(c);
			wfs_xfs_kinit_put();
			return -ENOMEM;
		}
	}
	init_rwsem(&x->rename_lock);
	x->icache = c;
	return 0;
}

void wfs_xfs_icache_destroy(struct wfs_xfs *x)
{
	struct wfs_xfs_icache *c = ic_of(x);
	size_t i, b;

	if (!c)
		return;
	for (i = 0; i < WFS_IC_SHARDS; i++) {
		struct ic_shard *s = &c->sh[i];

		for (b = 0; b < c->nbuckets; b++) {
			struct ic_ent *e = s->bucket[b];

			while (e) {
				struct ic_ent *next = e->hnext;

				ent_free(e);
				e = next;
			}
		}
		kfree(s->bucket);
	}
	destroy_rwsem(&x->rename_lock);
	kfree(c);
	x->icache = NULL;
	wfs_xfs_kinit_put();
}

/* Wait until e is not LOADING. Shard lock held on entry and on return. */
static void wait_loaded(struct ic_shard *s, struct ic_ent *e)
{
	while (e->state == ENT_LOADING) {
		spin_unlock(&s->lock);
		wait_var_event(&e->state, READ_ONCE(e->state) != ENT_LOADING);
		spin_lock(&s->lock);
	}
}

/* Kernel xfs_iget_check_free_state: CREATE wants a free inode, others an allocated one. */
static int check_free_state(struct xfs_inode *ip, unsigned int flags)
{
	if (flags & XFS_IGET_CREATE)
		return VFS_I(ip)->i_mode != 0 ? -EFSCORRUPTED : 0;
	return VFS_I(ip)->i_mode == 0 ? -ENOENT : 0;
}

static int iget_flags(struct wfs_xfs *x, struct xfs_trans *tp, xfs_ino_t ino,
		      unsigned int flags, struct xfs_inode **ipp)
{
	struct wfs_xfs_icache *c = ic_of(x);
	struct xfs_mount *mp = &x->mp;
	struct ic_ent *e, *nw = NULL;
	struct ic_shard *s;
	bool freeit;
	uint64_t h;
	size_t b;
	int error;

	*ipp = NULL;
	if (ino == 0 || ino == NULLFSINO || ino > XFS_MAXINUMBER ||
	    XFS_INO_TO_AGNO(mp, ino) >= mp->m_sb.sb_agcount)
		return -EINVAL;

	h = ino_hash(ino);
	s = shard_of(c, h);
	b = bucket_of(c, h);

	spin_lock(&s->lock);
	for (e = s->bucket[b]; e; e = e->hnext)
		if (I_INO(&e->ip) == ino)
			break;
	if (!e && !(flags & XFS_IGET_INCORE)) {
		spin_unlock(&s->lock);
		nw = ent_alloc(mp, ino);
		if (!nw)
			return -ENOMEM;
		spin_lock(&s->lock);
		for (e = s->bucket[b]; e; e = e->hnext)	/* lost a race? */
			if (I_INO(&e->ip) == ino)
				break;
	}
	if (e) {				/* hit */
		lru_remove(s, e);
		e->refs++;
		wait_loaded(s, e);
		if (e->state == ENT_FAILED) {
			error = e->err;
			freeit = --e->refs == 0;
		} else {
			error = check_free_state(&e->ip, flags);
			freeit = false;
			if (error) {
				e->refs--;
				release_locked(c, s, e);
			}
		}
		spin_unlock(&s->lock);
		if (freeit)
			ent_free(e);
		if (nw)
			ent_free(nw);
		if (!error)
			*ipp = &e->ip;
		return error;
	}
	if (!nw) {				/* XFS_IGET_INCORE miss */
		spin_unlock(&s->lock);
		return -ENODATA;
	}

	e = nw;					/* miss: load without the shard lock */
	e->sh = s;
	e->refs = 1;
	WRITE_ONCE(e->state, ENT_LOADING);
	e->hnext = s->bucket[b];
	s->bucket[b] = e;
	s->count++;
	spin_unlock(&s->lock);

	error = ent_load(x, e, tp, flags);
	if (!error)
		error = check_free_state(&e->ip, flags);

	spin_lock(&s->lock);
	freeit = false;
	if (error) {
		WRITE_ONCE(e->state, ENT_FAILED);	/* never cached: a corrupt inode is retried (and re-reported) */
		e->err = error;
		hash_unlink(c, s, e);
		freeit = --e->refs == 0;
	} else {
		WRITE_ONCE(e->state, ENT_READY);
		trim(c, s);		/* may evict others; e holds a reference */
	}
	spin_unlock(&s->lock);
	wake_up_var(&e->state);
	if (freeit)
		ent_free(e);
	if (!error)
		*ipp = &e->ip;
	return error;
}

int wfs_xfs_iget(struct wfs_xfs *x, xfs_ino_t ino, struct xfs_inode **ipp)
{
	return iget_flags(x, NULL, ino, XFS_IGET_UNTRUSTED, ipp);
}

/* Kernel xfs_iget (xfs_icache.c) on the winuxfs inode cache. */
int xfs_iget(struct xfs_mount *mp, struct xfs_trans *tp, xfs_ino_t ino, uint flags,
	     uint lock_flags, struct xfs_inode **ipp)
{
	int error = iget_flags(wfs_xfs_of(mp), tp, ino, flags, ipp);

	if (!error && lock_flags)
		xfs_ilock(*ipp, lock_flags);
	return error;
}

/* An inode outside the cache (log recovery: xfs_inode_item_recover.c). */
struct xfs_inode *xfs_inode_alloc(struct xfs_mount *mp, xfs_ino_t ino)
{
	struct ic_ent *e = ent_alloc(mp, ino);

	return e ? &e->ip : NULL;
}

void xfs_inode_free(struct xfs_inode *ip)
{
	ent_free(ent_of(ip));
}

/* Kernel xfs_setup_inode: the VFS size follows the on-disk size. */
void xfs_setup_inode(struct xfs_inode *ip)
{
	i_size_write(VFS_I(ip), ip->i_disk_size);
}

/* pag_ici_root lookups (xfs_iunlink_lookup): a cached inode, no reference. */
void *wfs_xfs_ici_lookup(struct radix_tree_root *root, unsigned long agino)
{
	struct xfs_perag *pag = container_of(root, struct xfs_perag, pag_ici_root);
	struct xfs_mount *mp = pag_mount(pag);
	struct wfs_xfs_icache *c = ic_of(wfs_xfs_of(mp));
	xfs_ino_t ino = XFS_AGINO_TO_INO(mp, pag_agno(pag), (xfs_agino_t)agino);
	uint64_t h = ino_hash(ino);
	struct ic_shard *s = shard_of(c, h);
	struct ic_ent *e;

	spin_lock(&s->lock);
	for (e = s->bucket[bucket_of(c, h)]; e; e = e->hnext)
		if (I_INO(&e->ip) == ino && e->state == ENT_READY)
			break;
	spin_unlock(&s->lock);
	return e ? &e->ip : NULL;
}

int wfs_xfs_icount(struct inode *inode)
{
	struct ic_ent *e = ent_of(XFS_I(inode));
	int n;

	spin_lock(&e->sh->lock);
	n = (int)e->refs;
	spin_unlock(&e->sh->lock);
	return n;
}

/* iput(): drop a transient reference (the kernel xfs_irele calls this). */
void wfs_xfs_iput(struct inode *inode)
{
	struct xfs_inode *ip = XFS_I(inode);
	struct ic_ent *e = ent_of(ip);
	struct ic_shard *s = e->sh;
	struct wfs_xfs_icache *c = ic_of(wfs_xfs_of(ip->i_mount));

	spin_lock(&s->lock);
	ASSERT(e->refs > 0);
	e->refs--;
	release_locked(c, s, e);
	spin_unlock(&s->lock);
}

/* ihold(): another transient reference, released by xfs_irele(). */
void wfs_xfs_ihold(struct inode *inode)
{
	struct ic_ent *e = ent_of(XFS_I(inode));

	spin_lock(&e->sh->lock);
	ASSERT(e->refs > 0);	/* the kernel's ihold also requires a held inode */
	e->refs++;
	spin_unlock(&e->sh->lock);
}

void wfs_xfs_ilookup_ref(struct xfs_inode *ip)
{
	struct ic_ent *e = ent_of(ip);

	spin_lock(&e->sh->lock);
	e->nlookup++;
	spin_unlock(&e->sh->lock);
}

/*
 * Unmount: free every unlinked inode the host still references (the kernel does
 * this when it evicts all inodes). Collects the numbers first; xfs_inactive
 * needs the shard locks free.
 */
void wfs_xfs_icache_inactive_all(struct wfs_xfs *x)
{
	struct wfs_xfs_icache *c = ic_of(x);
	xfs_ino_t *inos = NULL;
	size_t n = 0, cap = 0, i, b, s;

	if (!c || wfs_xfs_is_shutdown(&x->mp))
		return;
	for (s = 0; s < WFS_IC_SHARDS; s++) {
		struct ic_shard *sh = &c->sh[s];

		spin_lock(&sh->lock);
		for (b = 0; b < c->nbuckets; b++) {
			struct ic_ent *e;

			for (e = sh->bucket[b]; e; e = e->hnext) {
				if (e->state != ENT_READY || e->dead || VFS_I(&e->ip)->i_mode == 0 ||
				    VFS_I(&e->ip)->i_nlink != 0)
					continue;
				if (n == cap) {
					xfs_ino_t *t;

					cap = cap ? cap * 2 : 16;
					t = realloc(inos, cap * sizeof(*inos));
					if (!t) {
						spin_unlock(&sh->lock);
						goto done;
					}
					inos = t;
				}
				inos[n++] = I_INO(&e->ip);
			}
		}
		spin_unlock(&sh->lock);
	}
done:
	for (i = 0; i < n; i++) {
		struct xfs_inode *ip;

		if (wfs_xfs_iget(x, inos[i], &ip))
			continue;
		if (VFS_I(ip)->i_nlink == 0 && xfs_inactive(ip) == 0)
			wfs_xfs_idead(ip);
		xfs_irele(ip);
	}
	free(inos);
}

/* Host lookup references of ip (forget path decides inactive on 0). */
uint64_t wfs_xfs_inlookup(struct xfs_inode *ip)
{
	struct ic_ent *e = ent_of(ip);
	uint64_t n;

	spin_lock(&e->sh->lock);
	n = e->nlookup;
	spin_unlock(&e->sh->lock);
	return n;
}

/* The inode was freed on disk: drop the entry as soon as the last reference goes. */
void wfs_xfs_idead(struct xfs_inode *ip)
{
	struct ic_ent *e = ent_of(ip);

	spin_lock(&e->sh->lock);
	e->dead = true;
	spin_unlock(&e->sh->lock);
}

void wfs_xfs_ipin(struct xfs_inode *ip)
{
	struct ic_ent *e = ent_of(ip);

	spin_lock(&e->sh->lock);
	e->pinned = true;
	spin_unlock(&e->sh->lock);
}

void wfs_xfs_iforget(struct wfs_xfs *x, xfs_ino_t ino, uint64_t n)
{
	struct wfs_xfs_icache *c = ic_of(x);
	uint64_t h = ino_hash(ino);
	struct ic_shard *s = shard_of(c, h);
	struct ic_ent *e;

	spin_lock(&s->lock);
	for (e = s->bucket[bucket_of(c, h)]; e; e = e->hnext) {
		if (I_INO(&e->ip) == ino) {
			e->nlookup = n >= e->nlookup ? 0 : e->nlookup - n;
			release_locked(c, s, e);
			break;
		}
	}
	spin_unlock(&s->lock);
}

static void ic_stats(struct wfs_xfs *x, size_t *cnt, uint64_t *refs, uint64_t *nl)
{
	struct wfs_xfs_icache *c = ic_of(x);
	size_t i, b;

	*cnt = 0; *refs = 0; *nl = 0;
	if (!c)
		return;
	for (i = 0; i < WFS_IC_SHARDS; i++) {
		struct ic_shard *s = &c->sh[i];

		spin_lock(&s->lock);
		*cnt += s->count;
		for (b = 0; b < c->nbuckets; b++) {
			struct ic_ent *e;

			for (e = s->bucket[b]; e; e = e->hnext) {
				*refs += e->refs;
				*nl += e->nlookup;
			}
		}
		spin_unlock(&s->lock);
	}
}

size_t wfs_xfs_icache_count(struct wfs_xfs *x)
{
	size_t c; uint64_t r, n;
	ic_stats(x, &c, &r, &n);
	return c;
}

uint64_t wfs_xfs_icache_refs(struct wfs_xfs *x)
{
	size_t c; uint64_t r, n;
	ic_stats(x, &c, &r, &n);
	return r;
}

uint64_t wfs_xfs_icache_nlookup(struct wfs_xfs *x)
{
	size_t c; uint64_t r, n;
	ic_stats(x, &c, &r, &n);
	return n;
}

/* ------------------------------------------------- libxfs-called hooks -- */

void xfs_inode_verifier_error(struct xfs_inode *ip, int error, const char *name,
		const void *buf, size_t bufsz, xfs_failaddr_t failaddr)
{
	(void)buf; (void)bufsz;
	if (error > 0)
		error = -error;
	xfs_alert(ip->i_mount, "Metadata %s detected at %p (%s), inode 0x%llx",
		  error == -EFSBADCRC ? "CRC error" : "corruption", failaddr,
		  name ? name : "?", (unsigned long long)I_INO(ip));
}

void xfs_corruption_error(const char *tag, int level, struct xfs_mount *mp,
		const void *buf, size_t bufsize, const char *filename,
		int linenum, xfs_failaddr_t failaddr)
{
	(void)level; (void)buf; (void)bufsize;
	xfs_alert(mp, "Corruption detected (%s) at %p, %s:%d",
		  tag ? tag : "?", failaddr, filename ? filename : "?", linenum);
}

/* Metadata directory inodes (rt groups): xfs_icache.c, with our iget. */
int xfs_trans_metafile_iget(struct xfs_trans *tp, xfs_ino_t ino,
		enum xfs_metafile_type metafile_type, struct xfs_inode **ipp)
{
	struct xfs_mount *mp = tp->t_mountp;
	struct xfs_inode *ip;
	umode_t mode;
	int error;

	error = wfs_xfs_iget(wfs_xfs_of(mp), ino, &ip);
	if (error == -EFSCORRUPTED || error == -EINVAL)
		goto whine;
	if (error)
		return error;

	if (VFS_I(ip)->i_nlink == 0)
		goto bad_rele;
	mode = metafile_type == XFS_METAFILE_DIR ? S_IFDIR : S_IFREG;
	if ((VFS_I(ip)->i_mode & S_IFMT) != mode)
		goto bad_rele;
	if (xfs_has_metadir(mp)) {
		if (!xfs_is_metadir_inode(ip))
			goto bad_rele;
		if (metafile_type != ip->i_metatype)
			goto bad_rele;
	}
	*ipp = ip;
	return 0;
bad_rele:
	xfs_irele(ip);
whine:
	xfs_err(mp, "metadata inode 0x%llx type %u is corrupt",
		(unsigned long long)ino, metafile_type);
	return -EFSCORRUPTED;
}
