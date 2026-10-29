/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Inode cache (Task 8, design 7.2) and the ino mapping / getattr helpers
 * built on it (inomap.c).
 *
 * References on a cached inode
 *   transient   taken by wfs_xfs_iget(), dropped by xfs_irele()
 *   nlookup     taken by wfs_xfs_ilookup_ref() (host lookup), dropped by
 *               wfs_xfs_iforget(); the root is pinned and never forgotten
 * An entry with neither (and not pinned) sits on its shard's LRU and is the
 * first to go when the shard is over its limit. The limit is per shard
 * (inodecache_entries / WFS_IC_SHARDS, at least 1) and soft: referenced
 * entries are never evicted, so a host holding more inodes than the limit
 * (or one huge extent tree) exceeds it until the references are dropped.
 *
 * Locking: one mutex per shard guards that shard's hash chains, LRU and the
 * reference counts; the mutex is held across the disk read of a miss, so a
 * miss stalls only the other lookups of the same shard (design 7.2 accepts
 * this for M2). The buffer cache has its own locks and never calls back, so
 * shard -> buffer-cache is the only order. Inode fields are immutable after
 * the load; i_lock (xfs_ilock) serialises the lazy extent read only.
 *
 * Error values of wfs_xfs_iget() are kernel-style negative errno (libxfs
 * convention): -EINVAL = number outside the filesystem or not an allocated
 * inode (UNTRUSTED), -ENOENT = inode is free, -EFSCORRUPTED = bad dinode,
 * -ENOMEM, -EIO. Callers map them (wfs_xfs_err and the per-op rules).
 */
#ifndef WFS_XFS_ICACHE_H
#define WFS_XFS_ICACHE_H

#include "wfs_xfs.h"
#include "ops.h"
#include "xfs_inode.h"

#define WFS_IC_SHARDS	64

/* x->icache is created/destroyed by wfs_xfs_load_root()/unload_root(). */
int wfs_xfs_icache_create(struct wfs_xfs *x);
/* All transient references must be gone; nlookup references are dropped. */
void wfs_xfs_icache_destroy(struct wfs_xfs *x);

/* Get inode `ino` (XFS number) with a transient reference. */
int wfs_xfs_iget(struct wfs_xfs *x, xfs_ino_t ino, struct xfs_inode **ipp);

/* Host lookup reference: ip must be held (transient ref). */
void wfs_xfs_ilookup_ref(struct xfs_inode *ip);
/* forget(): drops `n` lookup references; no I/O. Unknown inode: ignored. */
void wfs_xfs_iforget(struct wfs_xfs *x, xfs_ino_t ino, uint64_t n);

/* Pin (root) / unpin. ip must be held. */
void wfs_xfs_ipin(struct xfs_inode *ip);

/* Statistics for tests. */
size_t wfs_xfs_icache_count(struct wfs_xfs *x);		/* cached entries */
uint64_t wfs_xfs_icache_refs(struct wfs_xfs *x);	/* sum of transient refs */
uint64_t wfs_xfs_icache_nlookup(struct wfs_xfs *x);	/* sum of lookup refs */

/* ---- inomap.c ---- */

/* design 8.1; both return 0 or -WFS_ESTALE/-WFS_EINVAL (backend codes). */
wfs_ino_t wfs_xfs_ino_to_wfs(const struct xfs_mount *mp, xfs_ino_t ino);
int wfs_xfs_ino_from_wfs(const struct xfs_mount *mp, wfs_ino_t w, xfs_ino_t *out);

/* design 8.2: fill *st from a held inode. -WFS_EUCLEAN for an unknown type. */
int wfs_xfs_fill_stat(struct xfs_inode *ip, wfs_stat *st);

#endif
