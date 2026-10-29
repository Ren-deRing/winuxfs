// SPDX-License-Identifier: GPL-2.0-only
/*
 * Inode cache and the inode lock functions (Task 8, design 7.2).
 * The load path mirrors xfs_iget_cache_miss() (xfs_icache.c): xfs_imap()
 * with XFS_IGET_UNTRUSTED, xfs_read_icluster(), xfs_inode_from_disk(), the
 * free-state check; teardown mirrors xfs_inode_free_callback(). See icache.h
 * for the reference and locking rules.
 */
#include "icache.h"
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

#define XFS_IGET_UNTRUSTED	(1U << 1)	/* xfs_icache.h is not compiled */

struct ic_shard;

struct ic_ent {
	struct xfs_inode	ip;		/* first: xfs_inode <-> ic_ent */
	struct ic_ent		*hnext;		/* hash chain */
	struct ic_ent		*lprev, *lnext;	/* LRU links (valid if on_lru) */
	struct ic_shard		*sh;
	uint32_t		refs;		/* transient references */
	uint64_t		nlookup;	/* host lookup references */
	bool			pinned;
	bool			on_lru;
};

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

/* ------------------------------------- global libxfs caches (refcounted) -- */

/*
 * libxfs allocates the COW fork from the global xfs_ifork_cache and btree
 * cursors from the btree cursor caches (the kernel creates both in
 * xfs_init_caches()). Shared by all mounts of the process, created by the
 * first icache, destroyed by the last. Init-time only, so a bare spin on a builtin test-and-set is enough
 * (also builds with mingw, where there is no static mutex initialiser here).
 */
static volatile int ifork_gate;
static int ifork_users;

/*
 * The data-device cursor caches only: the rt rmap/refcount ones are
 * write-path stubs here (rt groups with those btrees are not read in M2).
 */
static void btree_caches_destroy(void)
{
	xfs_refcountbt_destroy_cur_cache();
	xfs_rmapbt_destroy_cur_cache();
	xfs_bmbt_destroy_cur_cache();
	xfs_inobt_destroy_cur_cache();
	xfs_allocbt_destroy_cur_cache();
}

static int btree_caches_init(void)
{
	int error;

	if ((error = xfs_allocbt_init_cur_cache()) ||
	    (error = xfs_inobt_init_cur_cache()) ||
	    (error = xfs_bmbt_init_cur_cache()) ||
	    (error = xfs_rmapbt_init_cur_cache()) ||
	    (error = xfs_refcountbt_init_cur_cache())) {
		btree_caches_destroy();
		return error;
	}
	return 0;
}

static void gate_lock(void)
{
	while (__sync_lock_test_and_set(&ifork_gate, 1))
		;
}
static void gate_unlock(void) { __sync_lock_release(&ifork_gate); }

static int ifork_cache_get(void)
{
	int ret = 0;

	gate_lock();
	if (!ifork_users++ && !xfs_ifork_cache) {
		xfs_ifork_cache = kmem_cache_create("xfs_ifork",
				sizeof(struct xfs_ifork), 0, 0, NULL);
		/* directory code (lookup, node-format readdir) allocates da state */
		xfs_da_state_cache = kmem_cache_create("xfs_da_state",
				sizeof(struct xfs_da_state), 0, 0, NULL);
		if (xfs_ifork_cache && xfs_da_state_cache)
			ret = btree_caches_init();
		else
			ret = -ENOMEM;
		if (ret) {
			if (xfs_ifork_cache)
				kmem_cache_destroy(xfs_ifork_cache);
			if (xfs_da_state_cache)
				kmem_cache_destroy(xfs_da_state_cache);
			xfs_ifork_cache = NULL;
			xfs_da_state_cache = NULL;
			ifork_users--;
		}
	}
	gate_unlock();
	return ret;
}

static void ifork_cache_put(void)
{
	gate_lock();
	if (!--ifork_users && xfs_ifork_cache) {
		btree_caches_destroy();
		kmem_cache_destroy(xfs_da_state_cache);
		kmem_cache_destroy(xfs_ifork_cache);
		xfs_da_state_cache = NULL;
		xfs_ifork_cache = NULL;
	}
	gate_unlock();
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
	ip->i_priv = e;
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
	if (ip->i_cowfp) {
		xfs_idestroy_fork(ip->i_cowfp);
		kmem_cache_free(xfs_ifork_cache, ip->i_cowfp);
	}
	destroy_rwsem(&ip->i_lock);
	kfree(e);
}

/* Load the on-disk inode into e. Kernel errno on failure. */
static int ent_load(struct wfs_xfs *x, struct ic_ent *e)
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

	error = xfs_imap(pag, NULL, ino, &ip->i_imap, XFS_IGET_UNTRUSTED);
	if (error)
		goto out_put;
	error = xfs_read_icluster(pag, NULL, ip->i_imap.im_agbno, &bp);
	if (error)
		goto out_put;
	error = xfs_inode_from_disk(ip, xfs_buf_offset(bp, ip->i_imap.im_boffset));
	if (!error)
		xfs_buf_set_ref(bp, XFS_INO_REF);
	xfs_trans_brelse(NULL, bp);
	if (error)
		goto out_put;

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
	return !e->refs && !e->nlookup && !e->pinned;
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
	error = ifork_cache_get();
	if (error)
		return error;
	c = kzalloc(sizeof(*c), GFP_KERNEL);
	if (!c) {
		ifork_cache_put();
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
			ifork_cache_put();
			return -ENOMEM;
		}
	}
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
	kfree(c);
	x->icache = NULL;
	ifork_cache_put();
}

int wfs_xfs_iget(struct wfs_xfs *x, xfs_ino_t ino, struct xfs_inode **ipp)
{
	struct wfs_xfs_icache *c = ic_of(x);
	struct xfs_mount *mp = &x->mp;
	uint64_t h;
	struct ic_shard *s;
	struct ic_ent *e;
	size_t b;
	int error;

	if (ino == 0 || ino == NULLFSINO || ino > XFS_MAXINUMBER ||
	    XFS_INO_TO_AGNO(mp, ino) >= mp->m_sb.sb_agcount)
		return -EINVAL;

	h = ino_hash(ino);
	s = shard_of(c, h);
	b = bucket_of(c, h);

	spin_lock(&s->lock);
	for (e = s->bucket[b]; e; e = e->hnext) {
		if (I_INO(&e->ip) == ino) {
			lru_remove(s, e);
			e->refs++;
			spin_unlock(&s->lock);
			*ipp = &e->ip;
			return 0;
		}
	}

	e = ent_alloc(mp, ino);
	if (!e) {
		spin_unlock(&s->lock);
		return -ENOMEM;
	}
	error = ent_load(x, e);
	if (error) {
		spin_unlock(&s->lock);
		ent_free(e);	/* never cached: a corrupt inode is retried (and re-reported) */
		return error;
	}
	e->sh = s;
	e->refs = 1;
	e->hnext = s->bucket[b];
	s->bucket[b] = e;
	s->count++;
	trim(c, s);	/* may evict others; e holds a reference */
	spin_unlock(&s->lock);
	*ipp = &e->ip;
	return 0;
}

void xfs_irele(struct xfs_inode *ip)
{
	struct ic_ent *e = ent_of(ip);
	struct ic_shard *s = e->sh;
	struct wfs_xfs_icache *c = ic_of(wfs_xfs_of(ip->i_mount));

	spin_lock(&s->lock);
	ASSERT(e->refs > 0);
	e->refs--;
	release_locked(c, s, e);
	spin_unlock(&s->lock);
}

void wfs_xfs_ilookup_ref(struct xfs_inode *ip)
{
	struct ic_ent *e = ent_of(ip);

	spin_lock(&e->sh->lock);
	e->nlookup++;
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

/*
 * Inode locks. Only ILOCK is real: the read-only port has no IOLOCK or
 * MMAPLOCK users, and those flags are accepted and ignored. Exclusive is
 * taken only to read the extent list lazily (xfs_ilock_data_map_shared).
 */
void xfs_ilock(struct xfs_inode *ip, uint lock_flags)
{
	if (lock_flags & XFS_ILOCK_EXCL)
		down_write(&ip->i_lock);
	else if (lock_flags & XFS_ILOCK_SHARED)
		down_read(&ip->i_lock);
}

int xfs_ilock_nowait(struct xfs_inode *ip, uint lock_flags)
{
	if (lock_flags & XFS_ILOCK_EXCL)
		return down_write_trylock(&ip->i_lock);
	if (lock_flags & XFS_ILOCK_SHARED)
		return down_read_trylock(&ip->i_lock);
	return 1;
}

void xfs_iunlock(struct xfs_inode *ip, uint lock_flags)
{
	if (lock_flags & XFS_ILOCK_EXCL)
		up_write(&ip->i_lock);
	else if (lock_flags & XFS_ILOCK_SHARED)
		up_read(&ip->i_lock);
}

/*
 * The pthread/SRW lock cannot report its state, so this is a no-op; the
 * callers (libxfs ASSERTs) are debug checks only.
 */
void xfs_assert_ilocked(struct xfs_inode *ip, uint lock_flags)
{
	(void)ip; (void)lock_flags;
}

static uint ilock_map_shared(struct xfs_inode *ip, const struct xfs_ifork *ifp)
{
	uint lock_mode = XFS_ILOCK_SHARED;

	if (ifp && xfs_need_iread_extents(ifp))
		lock_mode = XFS_ILOCK_EXCL;
	xfs_ilock(ip, lock_mode);
	return lock_mode;
}

uint xfs_ilock_data_map_shared(struct xfs_inode *ip)
{
	return ilock_map_shared(ip, &ip->i_df);
}

uint xfs_ilock_attr_map_shared(struct xfs_inode *ip)
{
	return ilock_map_shared(ip, xfs_inode_has_attr_fork(ip) ? &ip->i_af : NULL);
}

bool xfs_is_always_cow_inode(const struct xfs_inode *ip)
{
	return ip->i_mount->m_always_cow && xfs_has_reflink(ip->i_mount);
}

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
