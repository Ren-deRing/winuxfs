/* SPDX-License-Identifier: GPL-2.0-only */
/* Buffer cache control API (the xfs_buf_* contract itself is shim/xfs_buf.h). */
#ifndef WFS_XFS_BUF_H
#define WFS_XFS_BUF_H

#include "xfs_platform.h"
#include "blk.h"

#define WFS_XFS_BUFCACHE_DEFAULT_BYTES	((size_t)64 << 20)

struct wfs_xfs_bcache;

/* One cache per mount. `blk` must outlive the cache. NULL on ENOMEM. */
struct wfs_xfs_bcache *wfs_xfs_bcache_create(const struct wfs_xfs_blk *blk,
		size_t limit_bytes);
/* All buffers must have been released. */
void wfs_xfs_bcache_destroy(struct wfs_xfs_bcache *c);
/* Option `bufcache_bytes` (Task 6 parses and range-checks). Shrinking evicts. */
void wfs_xfs_bcache_set_limit(struct wfs_xfs_bcache *c, size_t limit_bytes);
size_t wfs_xfs_bcache_limit(struct wfs_xfs_bcache *c);
size_t wfs_xfs_bcache_usage(struct wfs_xfs_bcache *c);	/* data bytes held */
/* Drop every unreferenced buffer. */
void wfs_xfs_bcache_purge(struct wfs_xfs_bcache *c);
/*
 * Bind a buftarg to a device role (WFS_DEV_DATA/LOG/RT) of the cache's blk.
 * Sets bt_priv, sector sizes (from blk->sector_size, 512 if unknown) and
 * bt_nr_sectors (from blk->size_bytes). `bt` must stay alive while buffers of
 * it exist. The caller sets bt->bt_mount.
 */
int wfs_xfs_buftarg_bind(struct xfs_buftarg *bt, struct wfs_xfs_bcache *c,
		int role);
void wfs_xfs_buftarg_unbind(struct xfs_buftarg *bt);
/* A read-only mount: the kernel code sees a read-only device (xfs_readonly_buftarg). */
void wfs_xfs_buftarg_set_read_only(struct xfs_buftarg *bt);

#endif
