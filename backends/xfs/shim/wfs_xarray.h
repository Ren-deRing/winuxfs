/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Minimal xarray: a growable index -> pointer table with 3 mark bits per
 * entry. Used for the per-AG/RT-group trees only (design 3.3), where indexes
 * are small dense group numbers; indexes >= WFS_XA_MAX_INDEX are rejected.
 * Callers serialize writers (mount time); readers need no lock afterwards.
 */
#ifndef WFS_SHIM_XARRAY_H
#define WFS_SHIM_XARRAY_H

#include "wfs_types.h"

typedef unsigned int xa_mark_t;
#define XA_MARK_0	((xa_mark_t)0)
#define XA_MARK_1	((xa_mark_t)1)
#define XA_MARK_2	((xa_mark_t)2)
#define XA_FLAGS_LOCK_IRQ 0u
#define WFS_XA_MAX_INDEX	(1ul << 24)

/* struct xarray itself is declared in wfs_types.h (opaque head). */
void xa_init(struct xarray *xa);
#define xa_init_flags(xa, f)	xa_init(xa)
void xa_destroy(struct xarray *xa);
void *xa_load(struct xarray *xa, unsigned long index);
int xa_insert(struct xarray *xa, unsigned long index, void *entry, gfp_t gfp);
void *xa_erase(struct xarray *xa, unsigned long index);
void xa_set_mark(struct xarray *xa, unsigned long index, xa_mark_t mark);
void xa_clear_mark(struct xarray *xa, unsigned long index, xa_mark_t mark);
bool xa_marked(struct xarray *xa, xa_mark_t mark);
void *xa_find(struct xarray *xa, unsigned long *indexp, unsigned long max, xa_mark_t mark);
void *xa_find_after(struct xarray *xa, unsigned long *indexp, unsigned long max, xa_mark_t mark);
#define xa_for_each(xa, index, entry) \
	for ((index) = 0, (entry) = xa_find(xa, &(index), ULONG_MAX, XA_PRESENT); \
	     (entry); (entry) = xa_find_after(xa, &(index), ULONG_MAX, XA_PRESENT))
#define XA_PRESENT	((xa_mark_t)8)
#define xa_for_each_marked(xa, index, entry, mark) \
	for ((index) = 0, (entry) = xa_find(xa, &(index), ULONG_MAX, mark); \
	     (entry); (entry) = xa_find_after(xa, &(index), ULONG_MAX, mark))

#endif
