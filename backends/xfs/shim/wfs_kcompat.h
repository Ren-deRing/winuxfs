/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Small kernel APIs the write/log path uses (M3a T2): extra atomics, static
 * keys, a one-CPU per-cpu model, lockref, iovecs, VFS/page-cache answers for
 * a backend without a page cache, and tunables.
 */
#ifndef WFS_SHIM_KCOMPAT_H
#define WFS_SHIM_KCOMPAT_H

#include "wfs_types.h"
#include "wfs_sync.h"
#include "wfs_sched.h"

/* ---- atomics -------------------------------------------------------- */
#define cmpxchg(p, o, n) ({						\
	__typeof__(*(p)) __o = (o);					\
	__atomic_compare_exchange_n((p), &__o, (n), 0,			\
				    __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST); \
	__o; })
#define xchg(p, v)		__atomic_exchange_n((p), (v), __ATOMIC_SEQ_CST)
#define try_cmpxchg(p, po, n)	__atomic_compare_exchange_n((p), (po), (n), 0, \
				    __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST)
#define smp_mb__after_atomic()	smp_mb()
#define smp_mb__before_atomic()	smp_mb()
static inline bool atomic_add_unless(atomic_t *a, int i, int u)
{
	int c = atomic_read(a);

	do {
		if (c == u)
			return false;
	} while (!__atomic_compare_exchange_n(&a->counter, &c, c + i, 0,
					      __ATOMIC_SEQ_CST, __ATOMIC_RELAXED));
	return true;
}
static inline bool atomic_sub_and_test(int i, atomic_t *a) { return __atomic_sub_fetch(&a->counter, i, __ATOMIC_SEQ_CST) == 0; }
static inline int atomic_xchg(atomic_t *a, int v) { return __atomic_exchange_n(&a->counter, v, __ATOMIC_SEQ_CST); }
/* Decrement; if it hits zero return true with the lock held. */
static inline bool atomic_dec_and_lock(atomic_t *a, spinlock_t *l)
{
	if (atomic_add_unless(a, -1, 1))
		return false;
	spin_lock(l);
	if (atomic_dec_and_test(a))
		return true;
	spin_unlock(l);
	return false;
}
static inline bool refcount_inc_not_zero(refcount_t *r) { return atomic_inc_not_zero(&r->refs); }
static inline void refcount_dec(refcount_t *r) { atomic_dec(&r->refs); }
static inline long long atomic64_sub_return(long long i, atomic64_t *a) { return __atomic_sub_fetch(&a->counter, i, __ATOMIC_SEQ_CST); }
static inline long long atomic64_cmpxchg(atomic64_t *a, long long o, long long n)
{
	__atomic_compare_exchange_n(&a->counter, &o, n, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
	return o;
}
static inline long long atomic64_xchg(atomic64_t *a, long long v) { return __atomic_exchange_n(&a->counter, v, __ATOMIC_SEQ_CST); }

struct lockref { spinlock_t lock; int count; };
static inline void lockref_init(struct lockref *l) { spin_lock_init(&l->lock); l->count = 1; }
static inline void lockref_get(struct lockref *l)
{
	spin_lock(&l->lock);
	l->count++;
	spin_unlock(&l->lock);
}
#define __lockref_is_dead(l)	((l)->count < 0)

/* ---- static keys ---------------------------------------------------- */
struct static_key_false { atomic_t enabled; };
#define DEFINE_STATIC_KEY_FALSE(name)	struct static_key_false name = { ATOMIC_INIT(0) }
#define static_branch_unlikely(k)	(atomic_read(&(k)->enabled) > 0)
#define static_branch_likely(k)		(atomic_read(&(k)->enabled) > 0)
#define static_branch_inc(k)		atomic_inc(&(k)->enabled)
#define static_branch_dec(k)		atomic_dec(&(k)->enabled)

/* ---- per-cpu: one CPU --------------------------------------------------
 * Kernel code protects per-cpu data by disabling preemption (get_cpu()).
 * Here every thread maps to CPU 0, so get_cpu() takes a process-wide lock
 * and per-cpu data has a single instance: correct, not scalable.
 */
#define NR_CPUS			1
#define nr_cpu_ids		1u
#define num_online_cpus()	1u
#define num_possible_cpus()	1u
#define raw_smp_processor_id()	0
#define smp_processor_id()	0
void wfs_cpu_lock(void);
void wfs_cpu_unlock(void);
#define get_cpu()		(wfs_cpu_lock(), 0)
#define put_cpu()		wfs_cpu_unlock()
#define preempt_disable()	wfs_cpu_lock()
#define preempt_enable()	wfs_cpu_unlock()
#define alloc_percpu(type)	((type *)kzalloc(sizeof(type), GFP_KERNEL))
#define alloc_percpu_gfp(type, gfp)	((type *)kzalloc(sizeof(type), (gfp)))
#define free_percpu(p)		kfree(p)
#define per_cpu_ptr(p, cpu)	((void)(cpu), (p))
#define this_cpu_ptr(p)		(p)
#define raw_cpu_ptr(p)		(p)
struct cpumask { unsigned long bits[1]; };
typedef struct cpumask cpumask_t;
#define cpu_possible_mask	(&wfs_cpu_one_mask)
#define cpu_online_mask		(&wfs_cpu_one_mask)
extern const struct cpumask wfs_cpu_one_mask;
#define for_each_cpu(cpu, mask)	\
	for ((cpu) = 0; (cpu) < 1; (cpu)++) if (cpumask_test_cpu((cpu), (mask)))
#define for_each_possible_cpu(cpu)	for ((cpu) = 0; (cpu) < 1; (cpu)++)
#define for_each_online_cpu(cpu)	for ((cpu) = 0; (cpu) < 1; (cpu)++)
static inline bool cpumask_test_cpu(int cpu, const struct cpumask *m)
{
	return (__atomic_load_n(&m->bits[0], __ATOMIC_ACQUIRE) >> cpu) & 1;
}
static inline bool cpumask_test_and_set_cpu(int cpu, struct cpumask *m)
{
	return (__atomic_fetch_or(&m->bits[0], 1ul << cpu, __ATOMIC_ACQ_REL) >> cpu) & 1;
}
static inline void cpumask_clear(struct cpumask *m) { __atomic_store_n(&m->bits[0], 0, __ATOMIC_RELEASE); }
static inline void cpumask_set_cpu(int cpu, struct cpumask *m) { __atomic_fetch_or(&m->bits[0], 1ul << cpu, __ATOMIC_ACQ_REL); }
static inline void percpu_counter_add_batch(struct percpu_counter *c, s64 v, s32 batch)
{
	(void)batch;
	percpu_counter_add(c, v);
}
#define percpu_counter_sub(c, v)	percpu_counter_add((c), -(s64)(v))
#define percpu_counter_inc(c)		percpu_counter_add((c), 1)
#define percpu_counter_dec(c)		percpu_counter_add((c), -1)

/* ---- memory --------------------------------------------------------- */
#define __GFP_DIRECT_RECLAIM	0x20u
#define __GFP_NORETRY		0x40u
#define is_vmalloc_addr(p)	false
#define offset_in_page(p)	((unsigned long)(uintptr_t)(p) & (PAGE_SIZE - 1))
static inline void *kvrealloc(const void *p, size_t size, gfp_t flags)
{
	return krealloc((void *)p, size, flags);
}
static inline s64 div_s64_rem(s64 dividend, s32 divisor, s32 *rem)
{
	*rem = (s32)(dividend % divisor);
	return dividend / divisor;
}

/* ---- radix tree: the per-AG incore inode tree is unused (icache.c) ---- */
struct radix_tree_root { void *xa_head; };
#define INIT_RADIX_TREE(root, gfp)	((root)->xa_head = NULL)
/* Lookups in pag_ici_root are lookups in the inode cache (icache.c). */
void *wfs_xfs_ici_lookup(struct radix_tree_root *root, unsigned long agino);
#define radix_tree_lookup(root, index)	wfs_xfs_ici_lookup((root), (index))

/* ---- iovecs ----------------------------------------------------------- */
struct kvec { void *iov_base; size_t iov_len; };

/* ---- VFS without a page cache -------------------------------------------
 * winuxfs has no page cache: file data is written by the backend's own write
 * path (M3 design 2), so mappings are always clean and empty.
 */
struct address_space;
#define I_DIRTY_SYNC		(1u << 0)
#define I_DIRTY_DATASYNC	(1u << 1)
#define I_DIRTY_PAGES		(1u << 2)
#define I_NEW			(1u << 3)
#define I_WILL_FREE		(1u << 4)
#define I_FREEING		(1u << 5)
#define I_CLEAR			(1u << 6)
#define I_DIRTY_TIME		(1u << 11)
#define I_DIRTY_INODE		(I_DIRTY_SYNC | I_DIRTY_DATASYNC)
#define I_DIRTY			(I_DIRTY_INODE | I_DIRTY_PAGES)
#define PAGECACHE_TAG_DIRTY	0
#define PAGECACHE_TAG_WRITEBACK	1
#define mapping_tagged(m, tag)			((void)(m), false)
#define filemap_write_and_wait(m)		((void)(m), 0)
#define filemap_write_and_wait_range(m, s, e)	((void)(m), 0)
#define filemap_invalidate_lock_two(a, b)	((void)0)
#define filemap_invalidate_unlock_two(a, b)	((void)0)
#define truncate_pagecache_range(i, s, e)	((void)0)
#define truncate_setsize(i, s)			i_size_write((i), (s))
#define inode_dio_wait(i)			((void)0)
#define lock_two_nondirectories(a, b)		((void)0)
#define unlock_two_nondirectories(a, b)		((void)0)
#define evict_inodes(sb)			((void)0)
#define uid_eq(a, b)				((a) == (b))
#define gid_eq(a, b)				((a) == (b))
static inline u32 sysv_encode_dev(u32 dev) { return dev; }	/* rdev is stored encoded */
#define SB_ACTIVE		(1u << 30)
#define SB_UNFROZEN		0
#define SB_FREEZE_WRITE		1
#define SB_FREEZE_COMPLETE	4
#define sb_start_intwrite(sb)	((void)(sb))
#define shrinker_free(s)	((void)(s))	/* no shrinkers are registered */
/* the host keeps the volume identity: nothing to publish */
#define super_set_uuid(sb, uuid, len)	((void)(sb), (void)(uuid), (void)(len))
#define MAX_RW_COUNT		(INT_MAX & PAGE_MASK)
/* The host process is the privileged caller of every admin operation. */
#define CAP_SYS_ADMIN		21
#define capable(cap)		((void)(cap), true)
/* Freezing the host device is the host's business (shutdown ioctl). */
#define bdev_freeze(bdev)	((void)(bdev), -EOPNOTSUPP)
static inline int bdev_thaw(struct block_device *bdev) { (void)bdev; return 0; }
#define sb_end_intwrite(sb)	((void)(sb))

/* ---- tunables (kernel xfs_globals.c / xfs_sysctl.c) ------------------- */
struct xfs_globals {
	int	log_recovery_delay;	/* seconds; 0 */
	int	mount_delay;
	bool	bug_on_assert;
	bool	always_cow;
};
extern struct xfs_globals xfs_globals;
/* Kernel default: 30 s between periodic log syncs (xfssyncd). */
extern int xfs_syncd_centisecs;
/*
 * Kernel xfs_super.c work queues used from libxfs: xfs_alloc_wq runs bmbt
 * splits on a fresh stack (xfs_btree_split); the variable is defined in
 * xfs_alloc.c. Created by the first write mount (M3a T4), NULL before.
 */
#define XFS_WQFLAGS(wqflags)	(WQ_FREEZABLE | (wqflags))

#endif /* WFS_SHIM_KCOMPAT_H */
