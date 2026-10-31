/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * struct wfs_xfs_blk: the XFS backend's view of the host block devices
 * (data, log, rt). Everything in the backend that reads or writes the disk
 * goes through wfs_xfs_blk_read/write/flush(), so tests can swap in a fake
 * (file-backed, or the crash harness that records every write and flush) by
 * setting the hooks. A NULL hook uses the host API (blk_read/write/flush).
 */
#ifndef WFS_XFS_BLK_H
#define WFS_XFS_BLK_H

#include <stddef.h>
#include <stdint.h>
#include <winuxfs/backend.h>
#include <winuxfs/errno.h>

struct wfs_xfs_blk {
	/* Test hook; return 0 or -errno. NULL: use api/dev below. */
	int (*read)(void *ctx, int role, uint64_t off, void *buf, size_t len);
	int (*write)(void *ctx, int role, uint64_t off, const void *buf, size_t len);
	int (*flush)(void *ctx, int role);	/* durable: everything written before */
	void *ctx;
	const wfs_host_api *api;
	wfs_blkdev *dev[WFS_DEV_COUNT];	/* NULL = device absent */
	uint32_t sector_size[WFS_DEV_COUNT];
	uint64_t size_bytes[WFS_DEV_COUNT];	/* 0 = unknown (no range check) */
};

/* Fill from the host API; sizes/sector sizes are queried once. */
static inline void wfs_xfs_blk_init_host(struct wfs_xfs_blk *b,
		const wfs_host_api *api, wfs_blkdev *const devs[WFS_DEV_COUNT])
{
	int i;

	*b = (struct wfs_xfs_blk){ .api = api };
	for (i = 0; i < WFS_DEV_COUNT; i++) {
		b->dev[i] = devs[i];
		if (devs[i]) {
			b->sector_size[i] = api->blk_sector_size(devs[i]);
			b->size_bytes[i] = api->blk_size(devs[i]);
		}
	}
}

/* 0 or -WFS_Exxx (Linux errno values). */
static inline int wfs_xfs_blk_read(const struct wfs_xfs_blk *b, int role,
		uint64_t off, void *buf, size_t len)
{
	if (role < 0 || role >= WFS_DEV_COUNT)
		return -WFS_EIO;
	if (b->read)
		return b->read(b->ctx, role, off, buf, len);
	if (!b->api || !b->dev[role])
		return -WFS_EIO;
	return b->api->blk_read(b->dev[role], off, buf, len);
}

/* The host opened the device read-only (or there is no host: test fakes
 * without a write hook count as read-only). */
static inline bool wfs_xfs_blk_read_only(const struct wfs_xfs_blk *b, int role)
{
	if (role < 0 || role >= WFS_DEV_COUNT)
		return true;
	if (b->write)
		return false;
	if (!b->api || !b->dev[role] || !b->api->blk_read_only)
		return true;
	return b->api->blk_read_only(b->dev[role]) != 0;
}

/* 0 or -WFS_Exxx. Never called on a read-only mount (M2 contract). */
static inline int wfs_xfs_blk_write(const struct wfs_xfs_blk *b, int role,
		uint64_t off, const void *buf, size_t len)
{
	if (role < 0 || role >= WFS_DEV_COUNT)
		return -WFS_EIO;
	if (b->write)
		return b->write(b->ctx, role, off, buf, len);
	if (!b->api || !b->dev[role])
		return -WFS_EIO;
	if (b->size_bytes[role] && (off > b->size_bytes[role] || len > b->size_bytes[role] - off))
		return -WFS_EIO;	/* the log/AIL never write past the device */
	return b->api->blk_write(b->dev[role], off, buf, len);
}

static inline int wfs_xfs_blk_flush(const struct wfs_xfs_blk *b, int role)
{
	if (role < 0 || role >= WFS_DEV_COUNT)
		return -WFS_EIO;
	if (b->flush)
		return b->flush(b->ctx, role);
	if (!b->api || !b->dev[role])
		return -WFS_EIO;
	return b->api->blk_flush(b->dev[role]);
}

#endif
