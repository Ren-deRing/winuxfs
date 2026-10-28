/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * struct wfs_xfs_blk: the XFS backend's view of the host block devices
 * (data, log, rt). Everything in the backend that reads the disk goes
 * through wfs_xfs_blk_read(), so tests can swap in a file-backed fake by
 * setting `read`. With read == NULL the host API (blk_read) is used.
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

#endif
