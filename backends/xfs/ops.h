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
/* Task 11: sb counters (trusted or AG sums, fixed at mount) -> wfs_statfs. */
wfs_err_t wfs_xfs_op_statfs(struct wfs_xfs *x, wfs_statfs *st);
void wfs_xfs_op_forget(struct wfs_xfs *x, wfs_ino_t ino, uint64_t nlookup);
/* read.c (Task 10, design 8.5/8.6) */
wfs_err_t wfs_xfs_op_read(struct wfs_xfs *x, wfs_ino_t ino, uint64_t off, void *buf,
			  size_t len, size_t *done);
wfs_err_t wfs_xfs_op_readlink(struct wfs_xfs *x, wfs_ino_t ino, char *buf, size_t cap,
			      size_t *len);

/* Task 9 (dir.c) */
wfs_err_t wfs_xfs_op_lookup(struct wfs_xfs *x, wfs_ino_t dir, const char *name, uint32_t len,
			    wfs_ino_t *out, wfs_stat *st);
wfs_err_t wfs_xfs_op_readdir(struct wfs_xfs *x, wfs_ino_t dir, uint64_t cookie,
			     wfs_dirent_cb cb, void *ctx);

/* wops.c (M3a T6) */
wfs_err_t wfs_xfs_op_create(struct wfs_xfs *x, wfs_ino_t dir, const char *name, uint32_t len,
			    uint32_t ftype, uint32_t mode, uint32_t uid, uint32_t gid,
			    wfs_ino_t *out, wfs_stat *st);
wfs_err_t wfs_xfs_op_unlink(struct wfs_xfs *x, wfs_ino_t dir, const char *name, uint32_t len);
wfs_err_t wfs_xfs_op_rmdir(struct wfs_xfs *x, wfs_ino_t dir, const char *name, uint32_t len);
wfs_err_t wfs_xfs_op_setattr(struct wfs_xfs *x, wfs_ino_t ino, const wfs_stat *st, uint32_t mask);
/* wdata.c (M3a T7) */
wfs_err_t wfs_xfs_op_write(struct wfs_xfs *x, wfs_ino_t ino, uint64_t off, const void *buf,
			   size_t len, size_t *done);
wfs_err_t wfs_xfs_op_fsync(struct wfs_xfs *x, wfs_ino_t ino, int datasync);
wfs_err_t wfs_xfs_op_sync(struct wfs_xfs *x);
void wfs_xfs_op_forget_w(struct wfs_xfs *x, uint64_t xino, uint64_t n);

#endif
