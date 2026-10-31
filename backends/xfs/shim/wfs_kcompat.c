// SPDX-License-Identifier: GPL-2.0-only
/* Out-of-line parts of wfs_kcompat.h. */
#include <pthread.h>
#include "wfs_shim.h"

/* get_cpu()/preempt_disable(): may nest within a thread, as in the kernel. */
static pthread_mutex_t cpu_lock;
static pthread_once_t cpu_once = PTHREAD_ONCE_INIT;

static void cpu_lock_init(void)
{
	pthread_mutexattr_t a;

	pthread_mutexattr_init(&a);
	pthread_mutexattr_settype(&a, PTHREAD_MUTEX_RECURSIVE);
	pthread_mutex_init(&cpu_lock, &a);
	pthread_mutexattr_destroy(&a);
}

void wfs_cpu_lock(void)
{
	pthread_once(&cpu_once, cpu_lock_init);
	pthread_mutex_lock(&cpu_lock);
}

void wfs_cpu_unlock(void)
{
	pthread_mutex_unlock(&cpu_lock);
}

const struct cpumask wfs_cpu_one_mask = { { 1ul } };

struct xfs_globals xfs_globals;
int xfs_syncd_centisecs = 30 * 100;

/* inode times: wall clock, nanoseconds (XFS keeps ns; bigtime covers the range). */
#include <time.h>
struct inode;
struct timespec64 current_time(struct inode *inode)
{
	struct timespec ts;

	(void)inode;
	clock_gettime(CLOCK_REALTIME, &ts);
	return (struct timespec64){ .tv_sec = ts.tv_sec, .tv_nsec = ts.tv_nsec };
}

/* list_sort: stable merge sort, like the kernel's (equal elements keep order). */
#include <stdlib.h>
void list_sort(void *priv, struct list_head *head,
	       int (*cmp)(void *priv, const struct list_head *a, const struct list_head *b))
{
	struct list_head **v, **tmp, *p;
	size_t n = 0, i, w;

	list_for_each(p, head)
		n++;
	if (n < 2)
		return;
	v = malloc(2 * n * sizeof(*v));
	if (!v) {
		/* No memory: insertion sort in place (stable, O(n^2), never fails). */
		struct list_head *cur = head->next->next;

		while (cur != head) {
			struct list_head *next = cur->next, *pos = cur->prev;

			while (pos != head && cmp(priv, pos, cur) > 0)
				pos = pos->prev;
			if (pos != cur->prev) {
				list_del(cur);
				list_add(cur, pos);
			}
			cur = next;
		}
		return;
	}
	tmp = v + n;
	i = 0;
	list_for_each(p, head)
		v[i++] = p;
	for (w = 1; w < n; w *= 2) {
		size_t lo;

		for (lo = 0; lo < n; lo += 2 * w) {
			size_t mid = lo + w < n ? lo + w : n, hi = lo + 2 * w < n ? lo + 2 * w : n;
			size_t a = lo, b = mid, k = lo;

			while (a < mid && b < hi)
				tmp[k++] = cmp(priv, v[b], v[a]) < 0 ? v[b++] : v[a++];
			while (a < mid)
				tmp[k++] = v[a++];
			while (b < hi)
				tmp[k++] = v[b++];
		}
		{ struct list_head **t = v; v = tmp; tmp = t; }
	}
	INIT_LIST_HEAD(head);
	for (i = 0; i < n; i++)
		list_add_tail(v[i], head);
	free(v < tmp ? v : tmp);
}
