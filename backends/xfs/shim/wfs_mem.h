/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Kernel allocation API over malloc, with a process-wide total-allocation
 * cap (fuzzing: wfs_mem_set_cap(n) makes any allocation that would push the
 * live total above n fail with NULL; 0 = unlimited). Every block carries a
 * 16-byte header holding its size, so kfree needs no size argument.
 * __GFP_NOFAIL is not honoured: callers keep their NULL checks (design 3.3).
 */
#ifndef WFS_SHIM_MEM_H
#define WFS_SHIM_MEM_H

#include "wfs_types.h"

void *wfs_alloc(size_t size, gfp_t flags);
void wfs_free(void *p);
void wfs_mem_set_cap(size_t cap);	/* 0 = unlimited */
size_t wfs_mem_cap_hits(void);	/* allocations refused because of the cap (fuzz: over-allocation) */
size_t wfs_mem_used(void);		/* live bytes incl. 16-byte headers */

static inline void *kmalloc(size_t n, gfp_t f) { return wfs_alloc(n, f); }
static inline void *kzalloc(size_t n, gfp_t f) { return wfs_alloc(n, f | __GFP_ZERO); }
static inline void *kmalloc_array(size_t n, size_t sz, gfp_t f)
{
	size_t total;
	if (__builtin_mul_overflow(n, sz, &total))
		return NULL;
	return wfs_alloc(total, f);
}
static inline void *kcalloc(size_t n, size_t sz, gfp_t f) { return kmalloc_array(n, sz, f | __GFP_ZERO); }
static inline void *kvmalloc(size_t n, gfp_t f) { return wfs_alloc(n, f); }
static inline void *kvzalloc(size_t n, gfp_t f) { return wfs_alloc(n, f | __GFP_ZERO); }
static inline void *kvmalloc_array(size_t n, size_t sz, gfp_t f) { return kmalloc_array(n, sz, f); }
static inline void *kvcalloc(size_t n, size_t sz, gfp_t f) { return kmalloc_array(n, sz, f | __GFP_ZERO); }
static inline void kfree(const void *p) { wfs_free((void *)p); }
static inline void kvfree(const void *p) { wfs_free((void *)p); }
void *krealloc(const void *p, size_t new_size, gfp_t f);
char *kstrdup(const char *s, gfp_t f);
char *kmemdup_nul(const char *s, size_t n, gfp_t f);
void *kmemdup(const void *p, size_t n, gfp_t f);
__printf(2, 3) char *kasprintf(gfp_t f, const char *fmt, ...);
#define kfree_const(p)	kfree(p)
#define vmalloc(n)	kvmalloc(n, 0)
#define vzalloc(n)	kvzalloc(n, 0)
#define vfree(p)	kvfree(p)

struct kmem_cache {
	const char *name;
	size_t size;
	void (*ctor)(void *);
};
#define SLAB_HWCACHE_ALIGN	0u
#define SLAB_RECLAIM_ACCOUNT	0u
#define SLAB_ACCOUNT		0u
#define SLAB_MEM_SPREAD		0u
#define SLAB_PANIC		0u
struct kmem_cache *kmem_cache_create(const char *name, size_t size, size_t align,
				     unsigned long flags, void (*ctor)(void *));
void kmem_cache_destroy(struct kmem_cache *c);
void *kmem_cache_alloc(struct kmem_cache *c, gfp_t f);
void *kmem_cache_zalloc(struct kmem_cache *c, gfp_t f);
void kmem_cache_free(struct kmem_cache *c, void *p);
#define KMEM_CACHE(s, f) kmem_cache_create(#s, sizeof(struct s), 0, (f), NULL)

#endif /* WFS_SHIM_MEM_H */
