// SPDX-License-Identifier: GPL-2.0-only
/*
 * Weak placeholders for libxfs-called functions that later tasks implement
 * with strong definitions elsewhere. Each is tagged PENDING(Tn); the task
 * named deletes the stub (the whole #if block) when it adds the real code.
 *
 * Why per-symbol objects: PE/COFF weak definitions in a separate object are
 * NOT honoured by GNU ld (mingw reports the symbol undefined), so the Makefile
 * compiles this file once per stub (-DWFS_ONLY_<symbol>) into libxfs_pending.a.
 * A strong definition in a directly linked object (buf.o, ...) wins because the
 * archive member is only pulled for a symbol that is still undefined; the
 * attribute below additionally keeps ELF semantics if the file is linked whole
 * (-DWFS_PENDING_ALL, used by the stub test). A stub returns an error
 * (-ENOSYS / false / NULL) and logs its name once. Nothing here aborts.
 */
#include "xfs_platform.h"
#include "xfs_fs.h"
#include "xfs_format.h"
#include "xfs_log_format.h"
#include "xfs_shared.h"
#include "xfs_trans_resv.h"
#include "xfs_bit.h"
#include "xfs_mount.h"
#include "xfs_inode.h"
#include "xfs_buf.h"
#include "xfs_da_format.h"
#include "xfs_da_btree.h"
#include "xfs_defer.h"
#include "xfs_btree.h"
#include "xfs_btree_staging.h"
#include "xfs_rmap.h"
#include "xfs_alloc.h"
#include "xfs_error.h"
#include "xfs_trans.h"
#include "xfs_ag.h"
#include "xfs_ag_resv.h"
#include "xfs_health.h"
#include "xfs_attr.h"
#include "xfs_attr_remote.h"
#include "xfs_bmap.h"
#include "xfs_bmap_item.h"
#include "xfs_bmap_util.h"
#include "xfs_extent_busy.h"
#include "xfs_extfree_item.h"
#include "xfs_filestream.h"
#include "xfs_icreate_item.h"
#include "xfs_inode_util.h"
#include "xfs_iomap.h"
#include "xfs_iunlink_item.h"
#include "xfs_log.h"
#include "xfs_metadir.h"
#include "xfs_metafile.h"
#include "xfs_parent.h"
#include "xfs_refcount.h"
#include "xfs_rtbitmap.h"
#include "xfs_rtgroup.h"
#include "xfs_rtrefcount_btree.h"
#include "xfs_rtrmap_btree.h"
#include "xfs_zone_alloc.h"
#include "stubs.h"

#ifdef _WIN32
#define WFS_WEAK
#else
#define WFS_WEAK __attribute__((weak))
#endif

/* Stub signatures follow the kernel prototypes (the headers above declare
 * them, so a mismatch is a compile error). Stubs never ASSERT or abort. */
#pragma GCC diagnostic ignored "-Wunused-parameter"

/* ---- PENDING(T5): none left, buf.c defines them ---- */

/* ---- PENDING(T6): none left, mount.c defines them ---- */

/* ---- PENDING(T7) ---- */

/* No PENDING stubs are left: xfs_log_check_lsn is a real definition in mount.c. */
