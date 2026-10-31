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
