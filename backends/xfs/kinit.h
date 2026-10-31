/* SPDX-License-Identifier: GPL-2.0-only */
/* Process-wide kernel caches and work queues (kinit.c), refcounted by mounts. */
#ifndef WFS_XFS_KINIT_H
#define WFS_XFS_KINIT_H
int wfs_xfs_kinit_get(void);
void wfs_xfs_kinit_put(void);
#endif
