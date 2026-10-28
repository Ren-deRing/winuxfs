/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef __XFS_MESSAGE_H
#define __XFS_MESSAGE_H 1
#include "wfs_log.h"
void xfs_buf_alert_ratelimited(struct xfs_buf *bp, const char *rlmsg, const char *fmt, ...);
#endif
