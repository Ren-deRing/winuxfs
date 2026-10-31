// SPDX-License-Identifier: GPL-2.0-only
#include "wfs_shim.h"

void xa_init(struct xarray *xa) { memset(xa, 0, sizeof(*xa)); }

void xa_destroy(struct xarray *xa)
{
	kvfree(xa->xa_slots);
	kvfree(xa->xa_marks);
	xa_init(xa);
}

void *xa_load(struct xarray *xa, unsigned long i) { return i < xa->xa_n ? xa->xa_slots[i] : NULL; }

static int grow(struct xarray *xa, unsigned long need)
{
	void **s;
	unsigned char *m;
	unsigned long n = xa->xa_n ? xa->xa_n : 16;

	while (n < need)
		n *= 2;
	s = kvzalloc(n * sizeof(*s), GFP_KERNEL);
	m = kvzalloc(n, GFP_KERNEL);
	if (!s || !m) {
		kvfree(s);
		kvfree(m);
		return -ENOMEM;
	}
	if (xa->xa_n) {
		memcpy(s, xa->xa_slots, xa->xa_n * sizeof(*s));
		memcpy(m, xa->xa_marks, xa->xa_n);
	}
	kvfree(xa->xa_slots);
	kvfree(xa->xa_marks);
	xa->xa_slots = s;
	xa->xa_marks = m;
	xa->xa_n = n;
	return 0;
}

int xa_alloc(struct xarray *xa, u32 *id, void *entry, struct xa_limit limit, gfp_t gfp)
{
	unsigned long i, max = min_t(unsigned long, limit.max, WFS_XA_MAX_INDEX - 1);

	for (i = limit.min; i <= max; i++) {
		if (i < xa->xa_n && xa->xa_slots[i])
			continue;
		if (xa_insert(xa, i, entry, gfp))
			return -ENOMEM;
		*id = (u32)i;
		return 0;
	}
	return -EBUSY;
}

int xa_insert(struct xarray *xa, unsigned long i, void *entry, gfp_t gfp)
{
	(void)gfp;
	if (i >= WFS_XA_MAX_INDEX)
		return -EINVAL;
	if (i >= xa->xa_n && grow(xa, i + 1))
		return -ENOMEM;
	if (xa->xa_slots[i])
		return -EBUSY;
	xa->xa_slots[i] = entry;
	return 0;
}

void *xa_erase(struct xarray *xa, unsigned long i)
{
	void *old = xa_load(xa, i);

	if (old) {
		xa->xa_slots[i] = NULL;
		xa->xa_marks[i] = 0;
	}
	return old;
}

void xa_set_mark(struct xarray *xa, unsigned long i, xa_mark_t m)
{ if (xa_load(xa, i)) xa->xa_marks[i] |= (unsigned char)(1u << m); }
void xa_clear_mark(struct xarray *xa, unsigned long i, xa_mark_t m)
{ if (i < xa->xa_n) xa->xa_marks[i] &= (unsigned char)~(1u << m); }

bool xa_marked(struct xarray *xa, xa_mark_t m)
{
	unsigned long i;

	for (i = 0; i < xa->xa_n; i++)
		if (xa->xa_slots[i] && (m == XA_PRESENT || (xa->xa_marks[i] & (1u << m))))
			return true;
	return false;
}

void *xa_find(struct xarray *xa, unsigned long *ip, unsigned long max, xa_mark_t m)
{
	unsigned long i;

	for (i = *ip; i < xa->xa_n && i <= max; i++)
		if (xa->xa_slots[i] && (m == XA_PRESENT || (xa->xa_marks[i] & (1u << m)))) {
			*ip = i;
			return xa->xa_slots[i];
		}
	return NULL;
}

void *xa_find_after(struct xarray *xa, unsigned long *ip, unsigned long max, xa_mark_t m)
{
	unsigned long i = *ip + 1;

	return xa_find(xa, &i, max, m) ? (*ip = i, xa->xa_slots[i]) : NULL;
}
