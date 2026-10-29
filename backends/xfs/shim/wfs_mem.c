// SPDX-License-Identifier: GPL-2.0-only
#include "wfs_shim.h"
#include <stdio.h>

#define HDR 16u
static size_t g_used;
static size_t g_cap;
static size_t g_cap_hits;

void wfs_mem_set_cap(size_t cap) { __atomic_store_n(&g_cap, cap, __ATOMIC_SEQ_CST); }
size_t wfs_mem_cap_hits(void) { return __atomic_load_n(&g_cap_hits, __ATOMIC_SEQ_CST); }
size_t wfs_mem_used(void) { return __atomic_load_n(&g_used, __ATOMIC_SEQ_CST); }

void *wfs_alloc(size_t size, gfp_t flags)
{
	size_t total, cap, cur;
	unsigned char *p;

	if (__builtin_add_overflow(size, HDR, &total))
		return NULL;
	cap = __atomic_load_n(&g_cap, __ATOMIC_SEQ_CST);
	cur = __atomic_load_n(&g_used, __ATOMIC_SEQ_CST);
	do {	/* reserve first so concurrent allocators cannot overshoot */
		if (cap && (total > cap || cur > cap - total)) {
			__atomic_fetch_add(&g_cap_hits, 1, __ATOMIC_SEQ_CST);
			return NULL;
		}
	} while (!__atomic_compare_exchange_n(&g_used, &cur, cur + total, 0,
					      __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST));
	p = (flags & __GFP_ZERO) ? calloc(1, total) : malloc(total);
	if (!p) {
		__atomic_fetch_sub(&g_used, total, __ATOMIC_SEQ_CST);
		return NULL;
	}
	memcpy(p, &total, sizeof(total));
	return p + HDR;
}

void wfs_free(void *ptr)
{
	unsigned char *p = ptr;
	size_t total;

	if (!p)
		return;
	p -= HDR;
	memcpy(&total, p, sizeof(total));
	__atomic_fetch_sub(&g_used, total, __ATOMIC_SEQ_CST);
	free(p);
}

static size_t block_size(const void *ptr)
{
	size_t total;

	memcpy(&total, (const unsigned char *)ptr - HDR, sizeof(total));
	return total - HDR;
}

void *krealloc(const void *p, size_t new_size, gfp_t f)
{
	void *n;
	size_t old;

	if (!p)
		return wfs_alloc(new_size, f);
	old = block_size(p);
	n = wfs_alloc(new_size, f);
	if (!n)
		return NULL;
	memcpy(n, p, old < new_size ? old : new_size);
	wfs_free((void *)p);
	return n;
}

void *kmemdup(const void *p, size_t n, gfp_t f)
{
	void *d = wfs_alloc(n, f);

	if (d)
		memcpy(d, p, n);
	return d;
}

char *kmemdup_nul(const char *s, size_t n, gfp_t f)
{
	char *d = wfs_alloc(n + 1, f);

	if (d) {
		memcpy(d, s, n);
		d[n] = 0;
	}
	return d;
}

char *kstrdup(const char *s, gfp_t f) { return s ? kmemdup_nul(s, strlen(s), f) : NULL; }

struct kmem_cache *kmem_cache_create(const char *name, size_t size, size_t align,
				     unsigned long flags, void (*ctor)(void *))
{
	struct kmem_cache *c = wfs_alloc(sizeof(*c), __GFP_ZERO);

	(void)align; (void)flags;
	if (c) {
		c->name = name;
		c->size = size;
		c->ctor = ctor;
	}
	return c;
}

void kmem_cache_destroy(struct kmem_cache *c) { wfs_free(c); }

void *kmem_cache_alloc(struct kmem_cache *c, gfp_t f)
{
	void *p = wfs_alloc(c->size, f);

	if (p && c->ctor)
		c->ctor(p);
	return p;
}

void *kmem_cache_zalloc(struct kmem_cache *c, gfp_t f) { return kmem_cache_alloc(c, f | __GFP_ZERO); }
void kmem_cache_free(struct kmem_cache *c, void *p) { (void)c; wfs_free(p); }

char *kasprintf(gfp_t f, const char *fmt, ...)
{
	va_list ap, ap2;
	int n;
	char *s;

	va_start(ap, fmt);
	va_copy(ap2, ap);
	n = vsnprintf(NULL, 0, fmt, ap);
	va_end(ap);
	s = n < 0 ? NULL : wfs_alloc((size_t)n + 1, f);
	if (s)
		vsnprintf(s, (size_t)n + 1, fmt, ap2);
	va_end(ap2);
	return s;
}
