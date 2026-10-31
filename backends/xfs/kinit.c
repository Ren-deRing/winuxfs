// SPDX-License-Identifier: GPL-2.0-only
/*
 * Process-wide kernel state, created by the first mount and destroyed by the
 * last (the kernel creates it once at module load: init_xfs_fs,
 * xfs_init_caches, xfs_init_workqueues in xfs_super.c). Same caches and
 * sizes as the kernel, minus the ones of features not compiled here (rt
 * btree cursors, exchange-range, parent pointers, xfs_buf/xfs_inode, which
 * the buffer and inode caches allocate themselves).
 */
#include "xfs_platform.h"
#include "xfs_fs.h"
#include "xfs_shared.h"
#include "xfs_format.h"
#include "xfs_log_format.h"
#include "xfs_trans_resv.h"
#include "xfs_mount.h"
#include "xfs_defer.h"
#include "xfs_inode.h"
#include "xfs_da_format.h"
#include "xfs_da_btree.h"
#include "xfs_btree.h"
#include "xfs_alloc_btree.h"
#include "xfs_ialloc_btree.h"
#include "xfs_bmap_btree.h"
#include "xfs_rmap_btree.h"
#include "xfs_refcount_btree.h"
#include "xfs_alloc.h"
#include "xfs_trans.h"
#include "xfs_trans_priv.h"
#include "xfs_log.h"
#include "xfs_log_priv.h"
#include "xfs_buf_item.h"
#include "xfs_inode_item.h"
#include "xfs_icreate_item.h"
#include "xfs_iunlink_item.h"
#include "xfs_extfree_item.h"
#include "xfs_rmap_item.h"
#include "xfs_refcount_item.h"
#include "xfs_bmap_item.h"
#include "xfs_attr_item.h"
#include "kinit.h"

/* Init-time only, so a bare spin on a builtin test-and-set is enough (also
 * builds with mingw, where there is no static mutex initialiser here). */
static volatile int gate;
static int users;

static void gate_lock(void)
{
	while (__sync_lock_test_and_set(&gate, 1))
		;
}
static void gate_unlock(void) { __sync_lock_release(&gate); }

/* The data-device cursor caches (the rt rmap/refcount btrees are not compiled). */
static void btree_caches_destroy(void)
{
	xfs_refcountbt_destroy_cur_cache();
	xfs_rmapbt_destroy_cur_cache();
	xfs_bmbt_destroy_cur_cache();
	xfs_inobt_destroy_cur_cache();
	xfs_allocbt_destroy_cur_cache();
}

static int btree_caches_init(void)
{
	int error;

	if ((error = xfs_allocbt_init_cur_cache()) ||
	    (error = xfs_inobt_init_cur_cache()) ||
	    (error = xfs_bmbt_init_cur_cache()) ||
	    (error = xfs_rmapbt_init_cur_cache()) ||
	    (error = xfs_refcountbt_init_cur_cache())) {
		btree_caches_destroy();
		return error;
	}
	return 0;
}

struct cache_def {
	struct kmem_cache	**cache;
	const char		*name;
	size_t			size;
};

static struct cache_def caches[] = {
	{ &xfs_log_ticket_cache, "xfs_log_ticket", sizeof(struct xlog_ticket) },
	{ &xfs_da_state_cache,	"xfs_da_state",	sizeof(struct xfs_da_state) },
	{ &xfs_ifork_cache,	"xfs_ifork",	sizeof(struct xfs_ifork) },
	{ &xfs_trans_cache,	"xfs_trans",	sizeof(struct xfs_trans) },
	{ &xfs_buf_item_cache,	"xfs_buf_item",	sizeof(struct xfs_buf_log_item) },
	{ &xfs_ili_cache,	"xfs_ili",	sizeof(struct xfs_inode_log_item) },
	{ &xfs_icreate_cache,	"xfs_icr",	sizeof(struct xfs_icreate_item) },
	{ &xfs_rud_cache,	"xfs_rud_item",	sizeof(struct xfs_rud_log_item) },
	{ &xfs_cud_cache,	"xfs_cud_item",	sizeof(struct xfs_cud_log_item) },
	{ &xfs_bud_cache,	"xfs_bud_item",	sizeof(struct xfs_bud_log_item) },
	{ &xfs_attrd_cache,	"xfs_attrd_item", sizeof(struct xfs_attrd_log_item) },
	{ &xfs_attri_cache,	"xfs_attri_item", sizeof(struct xfs_attri_log_item) },
	{ &xfs_iunlink_cache,	"xfs_iul_item",	sizeof(struct xfs_iunlink_item) },
};

static void caches_destroy(void)
{
	size_t i;

	kmem_cache_destroy(xfs_bui_cache);
	kmem_cache_destroy(xfs_cui_cache);
	kmem_cache_destroy(xfs_rui_cache);
	kmem_cache_destroy(xfs_efi_cache);
	kmem_cache_destroy(xfs_efd_cache);
	xfs_bui_cache = xfs_cui_cache = xfs_rui_cache = xfs_efi_cache = xfs_efd_cache = NULL;
	for (i = 0; i < ARRAY_SIZE(caches); i++) {
		kmem_cache_destroy(*caches[i].cache);
		*caches[i].cache = NULL;
	}
}

static int caches_init(void)
{
	size_t i;

	for (i = 0; i < ARRAY_SIZE(caches); i++) {
		*caches[i].cache = kmem_cache_create(caches[i].name, caches[i].size, 0, 0, NULL);
		if (!*caches[i].cache)
			goto fail;
	}
	/* intent items with inline extent arrays (sizes as in xfs_init_caches) */
	xfs_efd_cache = kmem_cache_create("xfs_efd_item",
			xfs_efd_log_item_sizeof(XFS_EFD_MAX_FAST_EXTENTS), 0, 0, NULL);
	xfs_efi_cache = kmem_cache_create("xfs_efi_item",
			xfs_efi_log_item_sizeof(XFS_EFI_MAX_FAST_EXTENTS), 0, 0, NULL);
	xfs_rui_cache = kmem_cache_create("xfs_rui_item",
			xfs_rui_log_item_sizeof(XFS_RUI_MAX_FAST_EXTENTS), 0, 0, NULL);
	xfs_cui_cache = kmem_cache_create("xfs_cui_item",
			xfs_cui_log_item_sizeof(XFS_CUI_MAX_FAST_EXTENTS), 0, 0, NULL);
	xfs_bui_cache = kmem_cache_create("xfs_bui_item",
			xfs_bui_log_item_sizeof(XFS_BUI_MAX_FAST_EXTENTS), 0, 0, NULL);
	if (xfs_efd_cache && xfs_efi_cache && xfs_rui_cache && xfs_cui_cache && xfs_bui_cache)
		return 0;
fail:
	caches_destroy();
	return -ENOMEM;
}

static void kinit_destroy(void)
{
	if (xfs_alloc_wq) {
		destroy_workqueue(xfs_alloc_wq);
		xfs_alloc_wq = NULL;
	}
	xfs_defer_destroy_item_caches();
	btree_caches_destroy();
	caches_destroy();
}

static int kinit_create(void)
{
	int error;

	error = caches_init();
	if (error)
		return error;
	error = btree_caches_init();
	if (error)
		goto fail;
	error = xfs_defer_init_item_caches();
	if (error)
		goto fail;
	/* bmbt splits run here on a fresh stack (xfs_btree_split) */
	xfs_alloc_wq = alloc_workqueue("xfsalloc", XFS_WQFLAGS(WQ_MEM_RECLAIM | WQ_FREEZABLE), 0);
	if (!xfs_alloc_wq) {
		error = -ENOMEM;
		goto fail;
	}
	return 0;
fail:
	kinit_destroy();
	return error;
}

int wfs_xfs_kinit_get(void)
{
	int ret = 0;

	gate_lock();
	if (!users) {
		ret = kinit_create();
		if (!ret)
			users = 1;
	} else {
		users++;
	}
	gate_unlock();
	return ret;
}

void wfs_xfs_kinit_put(void)
{
	gate_lock();
	if (users && !--users)
		kinit_destroy();
	gate_unlock();
}
