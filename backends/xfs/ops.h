/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Backend operations implemented on top of the mount context (Tasks 8-11).
 * Plain C like mount.h: the ABI glue (entry_stub.c) includes this without
 * the kernel shim.
 */
#ifndef WFS_XFS_OPS_H
#define WFS_XFS_OPS_H

#include <stdint.h>
#include <winuxfs/backend.h>

struct wfs_xfs;

wfs_err_t wfs_xfs_op_getattr(struct wfs_xfs *x, wfs_ino_t ino, wfs_stat *st);
void wfs_xfs_op_forget(struct wfs_xfs *x, wfs_ino_t ino, uint64_t nlookup);

/* Task 9 (dir.c) */
wfs_err_t wfs_xfs_op_lookup(struct wfs_xfs *x, wfs_ino_t dir, const char *name, uint32_t len,
			    wfs_ino_t *out, wfs_stat *st);
wfs_err_t wfs_xfs_op_readdir(struct wfs_xfs *x, wfs_ino_t dir, uint64_t cookie,
			     wfs_dirent_cb cb, void *ctx);

#endif
