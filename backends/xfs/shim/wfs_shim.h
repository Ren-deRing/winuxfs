/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Umbrella of the standalone shim base (no libxfs types needed): this is what
 * tests/test_shim.c includes. xfs_platform.h builds on it for libxfs.
 */
#ifndef WFS_SHIM_H
#define WFS_SHIM_H

#include "wfs_types.h"
#include "wfs_errno.h"
#include "wfs_byteorder.h"
#include "wfs_bitops.h"
#include "wfs_sync.h"
#include "wfs_sched.h"
#include "wfs_mem.h"
#include "wfs_kcompat.h"
#include "wfs_xarray.h"
#include "wfs_crc32c.h"
#include "wfs_log.h"
#include "wfs_trace_base.h"

#endif
