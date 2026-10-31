/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Locks and atomics. Windows: SRWLOCK (declared locally so <windows.h> and
 * its macro pollution never enter kernel-derived code). Elsewhere: pthread.
 * Spinlocks are mutexes (no interrupt context here). Buffer locks are no-ops
 * on read-only mounts (design 7.1); inode locks use rw_semaphore.
 */
#ifndef WFS_SHIM_SYNC_H
#define WFS_SHIM_SYNC_H

#include "wfs_types.h"

#ifdef _WIN32
typedef struct { void *p; } wfs_srw_t;
void InitializeSRWLock(wfs_srw_t *);
void AcquireSRWLockExclusive(wfs_srw_t *);
void ReleaseSRWLockExclusive(wfs_srw_t *);
void AcquireSRWLockShared(wfs_srw_t *);
void ReleaseSRWLockShared(wfs_srw_t *);
unsigned char TryAcquireSRWLockExclusive(wfs_srw_t *);
unsigned char TryAcquireSRWLockShared(wfs_srw_t *);
struct mutex { wfs_srw_t l; };
struct rw_semaphore { wfs_srw_t l; };
typedef struct spinlock { wfs_srw_t l; } spinlock_t;
static inline void mutex_init(struct mutex *m) { InitializeSRWLock(&m->l); }
static inline void mutex_lock(struct mutex *m) { AcquireSRWLockExclusive(&m->l); }
static inline void mutex_unlock(struct mutex *m) { ReleaseSRWLockExclusive(&m->l); }
static inline int mutex_trylock(struct mutex *m) { return TryAcquireSRWLockExclusive(&m->l) != 0; }
static inline void mutex_destroy(struct mutex *m) { (void)m; }
static inline void spin_lock_init(spinlock_t *s) { InitializeSRWLock(&s->l); }
static inline void spin_lock(spinlock_t *s) { AcquireSRWLockExclusive(&s->l); }
static inline void spin_unlock(spinlock_t *s) { ReleaseSRWLockExclusive(&s->l); }
static inline int spin_trylock(spinlock_t *s) { return TryAcquireSRWLockExclusive(&s->l) != 0; }
static inline void spin_lock_destroy(spinlock_t *s) { (void)s; }
static inline void init_rwsem(struct rw_semaphore *r) { InitializeSRWLock(&r->l); }
static inline void down_read(struct rw_semaphore *r) { AcquireSRWLockShared(&r->l); }
static inline void up_read(struct rw_semaphore *r) { ReleaseSRWLockShared(&r->l); }
static inline int down_read_trylock(struct rw_semaphore *r) { return TryAcquireSRWLockShared(&r->l) != 0; }
static inline void down_write(struct rw_semaphore *r) { AcquireSRWLockExclusive(&r->l); }
static inline void up_write(struct rw_semaphore *r) { ReleaseSRWLockExclusive(&r->l); }
static inline int down_write_trylock(struct rw_semaphore *r) { return TryAcquireSRWLockExclusive(&r->l) != 0; }
static inline void destroy_rwsem(struct rw_semaphore *r) { (void)r; }
#else
#include <pthread.h>
struct mutex { pthread_mutex_t l; };
struct rw_semaphore { pthread_rwlock_t l; };
typedef struct spinlock { pthread_mutex_t l; } spinlock_t;
static inline void mutex_init(struct mutex *m) { pthread_mutex_init(&m->l, NULL); }
static inline void mutex_lock(struct mutex *m) { pthread_mutex_lock(&m->l); }
static inline void mutex_unlock(struct mutex *m) { pthread_mutex_unlock(&m->l); }
static inline int mutex_trylock(struct mutex *m) { return pthread_mutex_trylock(&m->l) == 0; }
static inline void mutex_destroy(struct mutex *m) { pthread_mutex_destroy(&m->l); }
static inline void spin_lock_init(spinlock_t *s) { pthread_mutex_init(&s->l, NULL); }
static inline void spin_lock(spinlock_t *s) { pthread_mutex_lock(&s->l); }
static inline void spin_unlock(spinlock_t *s) { pthread_mutex_unlock(&s->l); }
static inline int spin_trylock(spinlock_t *s) { return pthread_mutex_trylock(&s->l) == 0; }
static inline void spin_lock_destroy(spinlock_t *s) { pthread_mutex_destroy(&s->l); }
static inline void init_rwsem(struct rw_semaphore *r) { pthread_rwlock_init(&r->l, NULL); }
static inline void down_read(struct rw_semaphore *r) { pthread_rwlock_rdlock(&r->l); }
static inline void up_read(struct rw_semaphore *r) { pthread_rwlock_unlock(&r->l); }
static inline int down_read_trylock(struct rw_semaphore *r) { return pthread_rwlock_tryrdlock(&r->l) == 0; }
static inline void down_write(struct rw_semaphore *r) { pthread_rwlock_wrlock(&r->l); }
static inline void up_write(struct rw_semaphore *r) { pthread_rwlock_unlock(&r->l); }
static inline int down_write_trylock(struct rw_semaphore *r) { return pthread_rwlock_trywrlock(&r->l) == 0; }
static inline void destroy_rwsem(struct rw_semaphore *r) { pthread_rwlock_destroy(&r->l); }
#endif

#define spin_lock_irqsave(s, f)		do { (void)(f); spin_lock(s); } while (0)
#define spin_unlock_irqrestore(s, f)	do { (void)(f); spin_unlock(s); } while (0)
#define lockdep_assert_held(l)		((void)0)
#define lockdep_assert_held_write(l)	((void)0)
#define lockdep_assert_held_read(l)	((void)0)
#define lockdep_is_held(l)		1
#define assert_spin_locked(l)		((void)0)
#define mutex_lock_nested(m, s)		mutex_lock(m)
#define down_read_nested(r, s)		down_read(r)
#define down_write_nested(r, s)		down_write(r)

/* Atomics. */
typedef struct { int counter; } atomic_t;
typedef struct { long long counter; } atomic64_t;
#define ATOMIC_INIT(i)		{ (i) }
#define ATOMIC64_INIT(i)	{ (i) }
static inline int atomic_read(const atomic_t *a) { return __atomic_load_n(&a->counter, __ATOMIC_RELAXED); }
static inline void atomic_set(atomic_t *a, int v) { __atomic_store_n(&a->counter, v, __ATOMIC_RELAXED); }
static inline void atomic_add(int i, atomic_t *a) { __atomic_fetch_add(&a->counter, i, __ATOMIC_SEQ_CST); }
static inline void atomic_sub(int i, atomic_t *a) { __atomic_fetch_sub(&a->counter, i, __ATOMIC_SEQ_CST); }
static inline void atomic_inc(atomic_t *a) { atomic_add(1, a); }
static inline void atomic_dec(atomic_t *a) { atomic_sub(1, a); }
static inline int atomic_inc_return(atomic_t *a) { return __atomic_add_fetch(&a->counter, 1, __ATOMIC_SEQ_CST); }
static inline int atomic_dec_return(atomic_t *a) { return __atomic_sub_fetch(&a->counter, 1, __ATOMIC_SEQ_CST); }
static inline int atomic_add_return(int i, atomic_t *a) { return __atomic_add_fetch(&a->counter, i, __ATOMIC_SEQ_CST); }
static inline bool atomic_dec_and_test(atomic_t *a) { return atomic_dec_return(a) == 0; }
static inline bool atomic_inc_not_zero(atomic_t *a)
{
	int v = atomic_read(a);

	while (v) {
		if (__atomic_compare_exchange_n(&a->counter, &v, v + 1, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST))
			return true;
	}
	return false;
}
static inline int atomic_cmpxchg(atomic_t *a, int old, int new_)
{ __atomic_compare_exchange_n(&a->counter, &old, new_, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST); return old; }
static inline long long atomic64_read(const atomic64_t *a) { return __atomic_load_n(&a->counter, __ATOMIC_RELAXED); }
static inline void atomic64_set(atomic64_t *a, long long v) { __atomic_store_n(&a->counter, v, __ATOMIC_RELAXED); }
static inline void atomic64_add(long long i, atomic64_t *a) { __atomic_fetch_add(&a->counter, i, __ATOMIC_SEQ_CST); }
static inline void atomic64_sub(long long i, atomic64_t *a) { __atomic_fetch_sub(&a->counter, i, __ATOMIC_SEQ_CST); }
static inline void atomic64_inc(atomic64_t *a) { atomic64_add(1, a); }
static inline void atomic64_dec(atomic64_t *a) { atomic64_sub(1, a); }
static inline long long atomic64_inc_return(atomic64_t *a) { return __atomic_add_fetch(&a->counter, 1, __ATOMIC_SEQ_CST); }
static inline long long atomic64_add_return(long long i, atomic64_t *a) { return __atomic_add_fetch(&a->counter, i, __ATOMIC_SEQ_CST); }

typedef struct { atomic_t refs; } refcount_t;
static inline void refcount_set(refcount_t *r, unsigned int n) { atomic_set(&r->refs, (int)n); }
static inline unsigned int refcount_read(const refcount_t *r) { return (unsigned)atomic_read(&r->refs); }
static inline void refcount_inc(refcount_t *r) { atomic_inc(&r->refs); }
static inline bool refcount_dec_and_test(refcount_t *r) { return atomic_dec_and_test(&r->refs); }

/* percpu counters are plain atomics: filled at mount, then read only. */
struct percpu_counter { atomic64_t count; };
static inline int percpu_counter_init(struct percpu_counter *c, s64 v, gfp_t g) { (void)g; atomic64_set(&c->count, v); return 0; }
static inline void percpu_counter_destroy(struct percpu_counter *c) { (void)c; }
static inline s64 percpu_counter_read(struct percpu_counter *c) { return atomic64_read(&c->count); }
static inline s64 percpu_counter_sum(struct percpu_counter *c) { return atomic64_read(&c->count); }
static inline s64 percpu_counter_read_positive(struct percpu_counter *c) { s64 v = atomic64_read(&c->count); return v < 0 ? 0 : v; }
static inline s64 percpu_counter_sum_positive(struct percpu_counter *c) { return percpu_counter_read_positive(c); }
static inline void percpu_counter_set(struct percpu_counter *c, s64 v) { atomic64_set(&c->count, v); }
static inline int percpu_counter_compare(struct percpu_counter *c, s64 rhs)
{ s64 v = atomic64_read(&c->count); return v < rhs ? -1 : v > rhs; }
#define __percpu_counter_compare(c, rhs, batch) percpu_counter_compare(c, rhs)
static inline void percpu_counter_add(struct percpu_counter *c, s64 v) { atomic64_add(v, &c->count); }

#endif /* WFS_SHIM_SYNC_H */
