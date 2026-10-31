/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * XFS backend mount entry points (Task 6). Plain C: no kernel headers, so the
 * ABI glue (entry_stub.c) and tests can include it. The mount context itself
 * (struct wfs_xfs) is in wfs_xfs.h.
 */
#ifndef WFS_XFS_MOUNT_H
#define WFS_XFS_MOUNT_H

#include <stdbool.h>
#include <stdint.h>
#include <winuxfs/backend.h>

struct wfs_xfs;

/* Mount options (comma separated list, see options.c). */
#define WFS_XFS_BUFCACHE_MIN		((uint64_t)1 << 20)	/* 1 MiB */
#define WFS_XFS_BUFCACHE_MAX		((uint64_t)64 << 30)	/* 64 GiB */
#define WFS_XFS_BUFCACHE_DEF		((uint64_t)64 << 20)	/* 64 MiB */
#define WFS_XFS_INODECACHE_MIN		64u
#define WFS_XFS_INODECACHE_MAX		(1u << 24)
#define WFS_XFS_INODECACHE_DEF		16384u
#define WFS_XFS_MAX_AGCOUNT		(1u << 20)	/* hostile-sb guard, see prevalidate_sb */

struct wfs_xfs_options {
	bool		norecovery;		/* mount a dirty log without replay */
	bool		ro;			/* read-only even on a writable device */
	uint64_t	bufcache_bytes;		/* WFS_XFS_BUFCACHE_MIN..MAX */
	uint32_t	inodecache_entries;	/* WFS_XFS_INODECACHE_MIN..MAX */
};

/*
 * Parse `str` (NULL or "" = defaults). Unknown keys, a value on `norecovery`,
 * a missing or non-decimal value, overflow and out-of-range values return
 * -WFS_EINVAL with one host log message (level 0) naming the offending token.
 * Empty list items ("a,,b") are skipped. A later item overrides an earlier
 * one. `api` may be NULL (then nothing is logged). *o is always initialised.
 */
int wfs_xfs_parse_options(const wfs_host_api *api, const char *str,
			  struct wfs_xfs_options *o);

/* 0 = XFSB magic and version 5 (reads one sector); -WFS_EINVAL = not that;
 * -WFS_EIO = the read failed. Never claims v4. */
wfs_err_t wfs_xfs_probe(const wfs_host_api *api, wfs_blkdev *const devs[WFS_DEV_COUNT]);

/*
 * Mount (M2 design 5.5 up to the log check, the root inode via the Task 8
 * hook wfs_xfs_load_root() and the summary counters). Errors, all negative
 * WFS_E*: -EOPNOTSUPP unsupported feature (v4, ZONED, unknown incompat),
 * -EUCLEAN corrupt / needs repair / dirty log, -ENODEV a needed RT or log
 * device is absent, -EINVAL bad option or device sector larger than the
 * filesystem sector, -EIO host read failure, -ENOMEM.
 */
wfs_err_t wfs_xfs_mount(const wfs_host_api *api, wfs_blkdev *const devs[WFS_DEV_COUNT],
			const char *options, struct wfs_xfs **out);
wfs_err_t wfs_xfs_unmount(struct wfs_xfs *x);

#endif
