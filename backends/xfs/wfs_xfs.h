/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * struct wfs_xfs: the XFS backend's mount context. Allocated by
 * wfs_xfs_mount(), handed to the host as the opaque wfs_fs*, freed by
 * wfs_xfs_unmount(). Tasks 8-11 extend it (see "Hooks" below); keep every
 * field they need here rather than in file-scope globals so two mounts in
 * one process stay independent.
 *
 * Ownership and lifetime
 *   x->mp          the libxfs xfs_mount (embedded: wfs_xfs_of() is container_of)
 *   x->blk         host devices + sector/size info; the buffer cache reads
 *                  through it and everything must outlive the cache
 *   x->bcache      metadata buffer cache (buf.c), one per mount
 *   x->bt[role]    buftargs of the cache for WFS_DEV_DATA/LOG/RT; mp's
 *                  m_ddev_targp/m_logdev_targp/m_rtdev_targp point into it
 *                  (m_logdev_targp == m_ddev_targp for an internal log, NULL
 *                  when an external log device is absent and norecovery set)
 *   x->icache      Task 8: inode cache (owned by x, created in
 *                  wfs_xfs_load_root(), destroyed in wfs_xfs_unload_root())
 *
 * Hooks (all in mount.c, called by wfs_xfs_mount/unmount in this order)
 *   int  wfs_xfs_load_root(struct wfs_xfs *x)   mount step 7. Task 8 fills it:
 *        create the inode cache (x->opts.inodecache_entries), xfs_iget
 *        sb_rootino with XFS_IGET_UNTRUSTED, require a directory, pin it,
 *        set x->mp.m_rootip. Returns 0 or -errno; the Task 6 placeholder
 *        returns -ENOSYS, which mount tolerates (any other error fails the
 *        mount and mount then undoes steps 1-6 and calls unload).
 *   void wfs_xfs_unload_root(struct wfs_xfs *x) unmount (also on mount
 *        failure after load_root): drop the pin, free the inode cache.
 *        Must be safe if load_root never ran or failed.
 *
 * Error mapping: wfs_xfs_err() converts any kernel-style errno from libxfs
 * or the buffer cache to the -WFS_E* value the ABI wants (design 9).
 * Threading: mount/unmount are single threaded; after mount the context is
 * shared read-only (caches have their own locks).
 */
#ifndef WFS_XFS_WFS_XFS_H
#define WFS_XFS_WFS_XFS_H

#include "xfs_platform.h"
#include "xfs_fs.h"
#include "xfs_format.h"
#include "xfs_log_format.h"
#include "xfs_shared.h"
#include "xfs_trans_resv.h"
#include "xfs_mount.h"
#include "buf.h"
#include "mount.h"

struct wfs_xfs {
	struct xfs_mount	mp;
	struct super_block	sb;		/* mp.m_super */
	const wfs_host_api	*api;
	struct wfs_xfs_blk	blk;
	struct wfs_xfs_bcache	*bcache;
	struct xfs_buftarg	bt[WFS_DEV_COUNT];
	bool			bt_bound[WFS_DEV_COUNT];
	struct wfs_xfs_options	opts;
	int			log_role;	/* WFS_DEV_DATA or WFS_DEV_LOG, -1 none */
	bool			perag_up, rtg_up;	/* groups need freeing */
	bool			root_loaded;	/* Task 8: wfs_xfs_load_root() succeeded */
	void			*icache;	/* Task 8 */
	uint64_t		mem_budget;	/* added to the process allocation cap while mounted */
	uint32_t		shutdown;	/* SHUTDOWN_* flags; nonzero = shut down (atomic) */
};

/*
 * Filesystem shutdown (kernel xfs_force_shutdown): after a metadata write or
 * log error, or in-core corruption, every later change fails and nothing more
 * is written. Reads keep working. The first call logs the reason.
 */
bool wfs_xfs_is_shutdown(struct xfs_mount *mp);
void wfs_xfs_force_shutdown(struct xfs_mount *mp, uint32_t flags);

static inline struct wfs_xfs *wfs_xfs_of(struct xfs_mount *mp)
{
	return container_of(mp, struct wfs_xfs, mp);
}

/* Kernel errno (negative or positive; libxfs/buf cache) -> -WFS_E*. */
static inline int wfs_xfs_err(int e)
{
	if (e > 0)
		e = -e;
	switch (-e) {
	case 0:
		return 0;
	case EFSCORRUPTED:
	case EFSBADCRC:
		return -WFS_EUCLEAN;
	case ENOMEM:		return -WFS_ENOMEM;
	case EIO:		return -WFS_EIO;
	case EOPNOTSUPP:	return -WFS_EOPNOTSUPP;
	case ENODEV:		return -WFS_ENODEV;
	case EINVAL:		return -WFS_EINVAL;
	case ENOENT:		return -WFS_ENOENT;
	case ENOTDIR:		return -WFS_ENOTDIR;
	case EROFS:		return -WFS_EROFS;
	default:		return -WFS_EIO;
	}
}

/* Task 8 hooks, see above. */
int wfs_xfs_load_root(struct wfs_xfs *x);
void wfs_xfs_unload_root(struct wfs_xfs *x);

#endif
