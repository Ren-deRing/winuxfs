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

#endif
