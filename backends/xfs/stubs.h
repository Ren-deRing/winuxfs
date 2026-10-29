/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Helpers for stubs_ro.c / stubs_pending.c: log the stub's name through the
 * shim log sink (which the backend points at the host log) and never abort.
 */
#ifndef WFS_XFS_STUBS_H
#define WFS_XFS_STUBS_H

#include "wfs_shim.h"

/* Logs the first n calls of the calling stub. */
#define WFS_STUB_LOG_N(n, what)						\
do {									\
	static int _wfs_stub_count;					\
	if (__atomic_fetch_add(&_wfs_stub_count, 1, __ATOMIC_RELAXED) < (n)) \
		xfs_warn(NULL, "%s: %s called, not supported in the read-only port", \
			 __func__, (what));				\
} while (0)

/* Write-path / never-used function: log the first 10 calls. */
#define WFS_STUB_RO()		WFS_STUB_LOG_N(10, "write-path stub")
/* Function a later task implements: log once. */
#define WFS_STUB_PENDING()	WFS_STUB_LOG_N(1, "pending stub")

#endif
