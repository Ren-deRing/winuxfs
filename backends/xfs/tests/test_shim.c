// SPDX-License-Identifier: GPL-2.0-only
/* Standalone tests of the kernel compatibility shim base. */
#include "wfs_shim.h"
#include <stdio.h>
#include <string.h>

static int failures, checks;
#define CHECK(c) do { checks++; if (!(c)) { failures++; \
	fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #c); } } while (0)

static u32 crc_std(const void *p, size_t n)
{
	return ~crc32c_le(~0u, p, n);
}

static void crc32c_known_vectors(void)
{
	u8 b[32];
	int i;

	memset(b, 0, 32);
	CHECK(crc_std(b, 32) == 0x8A9136AAu);
	memset(b, 0xFF, 32);
	CHECK(crc_std(b, 32) == 0x62A8AB43u);
	for (i = 0; i < 32; i++) b[i] = (u8)i;
	CHECK(crc_std(b, 32) == 0x46DD794Eu);
	for (i = 0; i < 32; i++) b[i] = (u8)(31 - i);
	CHECK(crc_std(b, 32) == 0x113FDB5Cu);
	CHECK(crc_std("123456789", 9) == 0xE3069283u);
	CHECK(crc_std("", 0) == 0);
}

static void crc32c_chunking_and_alignment(void)
{
	u8 buf[300];
	size_t i, split;
	u32 whole;

	for (i = 0; i < sizeof(buf); i++) buf[i] = (u8)(i * 7 + 3);
	for (i = 0; i < 9; i++) {	/* unaligned starts */
		whole = crc32c_le(~0u, buf + i, 250);
		for (split = 0; split <= 250; split += 17)
			CHECK(crc32c_le(crc32c_le(~0u, buf + i, split),
					buf + i + split, 250 - split) == whole);
	}
}

static void errno_values_are_linux(void)
{
	CHECK(EPERM == 1 && ENOENT == 2 && EIO == 5 && ENOMEM == 12);
	CHECK(ENODEV == 19 && ENOTDIR == 20 && EINVAL == 22 && EFBIG == 27);
	CHECK(ENOSPC == 28 && EROFS == 30 && ENAMETOOLONG == 36);
	CHECK(ENOSYS == 38 && ENOTEMPTY == 39 && ELOOP == 40);
	CHECK(ENODATA == 61 && EBADMSG == 74 && EOVERFLOW == 75);
	CHECK(EOPNOTSUPP == 95 && ESTALE == 116 && EUCLEAN == 117);
	CHECK(EFSCORRUPTED == EUCLEAN && EFSBADCRC == EBADMSG);
	CHECK(ENOATTR == ENODATA);
	CHECK(EWRONGFS == EINVAL);
	CHECK(EFSCORRUPTED == WFS_EUCLEAN && EFSBADCRC == WFS_EBADMSG);
}

static void endian_roundtrip_be16_32_64(void)
{
	u8 raw[8] = {0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08};
	__be16 a; __be32 b; __be64 c;

	memcpy(&a, raw, 2); memcpy(&b, raw, 4); memcpy(&c, raw, 8);
	CHECK(be16_to_cpu(a) == 0x0102);
	CHECK(be32_to_cpu(b) == 0x01020304u);
	CHECK(be64_to_cpu(c) == 0x0102030405060708ull);
	CHECK(cpu_to_be16(0x0102) == a);
	CHECK(cpu_to_be32(0x01020304u) == b);
	CHECK(cpu_to_be64(0x0102030405060708ull) == c);
	CHECK(le16_to_cpu(cpu_to_le16(0xBEEF)) == 0xBEEF);
	CHECK(le32_to_cpu(cpu_to_le32(0xDEADBEEFu)) == 0xDEADBEEFu);
	CHECK(le64_to_cpu(cpu_to_le64(0x1122334455667788ull)) == 0x1122334455667788ull);
	{ u8 le[4]; u32 v = cpu_to_le32(0x01020304u); memcpy(le, &v, 4);
	  CHECK(le[0] == 4 && le[3] == 1); }
	{ u8 m[9] = {0, 1, 2, 3, 4, 5, 6, 7, 8};
	  CHECK(get_unaligned_be32(m + 1) == 0x01020304u);
	  CHECK(get_unaligned_be64(m + 1) == 0x0102030405060708ull);
	  put_unaligned_be32(0xAABBCCDDu, m + 3);
	  CHECK(m[3] == 0xAA && m[6] == 0xDD); }
}

static void bitops_are_64bit_safe(void)
{
	u64 x = 1ull << 40;
	unsigned long map[4] = {0, 0, 0, 0};

	CHECK(fls64(x) == 41 && ffs64(x) == 41);
	CHECK(fls(0) == 0 && fls(0x80000000u) == 32 && ffs(0) == 0 && ffs(8) == 4);
	CHECK(hweight64(0xF0F0F0F0F0F0F0F0ull) == 32 && hweight32(0xFFu) == 8);
	CHECK(ilog2(1) == 0 && ilog2(4096) == 12 && ilog2(1ull << 40) == 40);
	CHECK(is_power_of_2(64) && !is_power_of_2(0) && !is_power_of_2(96));
	set_bit(3, map);
	set_bit(BITS_PER_LONG + 1, map);
	CHECK(test_bit(3, map) && test_bit(BITS_PER_LONG + 1, map) && !test_bit(4, map));
	CHECK(test_and_clear_bit(3, map) && !test_bit(3, map));
	CHECK(!test_and_set_bit(5, map) && test_and_set_bit(5, map));
	CHECK(find_first_bit(map, 4 * BITS_PER_LONG) == 5);
	CHECK(find_next_bit(map, 4 * BITS_PER_LONG, 6) == BITS_PER_LONG + 1);
	CHECK(find_next_bit(map, 4 * BITS_PER_LONG, BITS_PER_LONG + 2) == 4 * BITS_PER_LONG);
	CHECK(find_first_zero_bit(map, 4 * BITS_PER_LONG) == 0);
	CHECK(BITS_PER_LONG == (int)(sizeof(long) * 8));
	CHECK(rounddown_64(1ull << 40 | 5, 4096) == 1ull << 40);
	CHECK(howmany_64((1ull << 40) + 1, 4096) == (1ull << 28) + 1);
	CHECK(div_u64(1ull << 40, 3) == 366503875925ull);
	{ u64 n = 1ull << 40; u32 r = do_div(n, 1000); CHECK(r == 776 && n == 1099511627ull); }
	CHECK(log2_if_power2(4096) == 12 && log2_if_power2(100) == -1);
	CHECK(mask64_if_power2(4096) == 4095 && mask64_if_power2(100) == 0);
	CHECK(S_ISDIR(0040755) && S_ISREG(0100644) && S_ISLNK(0120777) && !S_ISDIR(0100644));
	CHECK(min(3, 4) == 3 && max(3u, 4u) == 4 && clamp(9, 1, 5) == 5);
}

static void uuid_helpers(void)
{
	uuid_t a, b;
	u8 raw[16];
	int i;

	for (i = 0; i < 16; i++) raw[i] = (u8)(i + 1);
	memset(&a, 0, sizeof(a));
	CHECK(uuid_is_null(&a) && uuid_equal(&a, &a) && sizeof(uuid_t) == 16);
	import_uuid(&a, raw);
	CHECK(!uuid_is_null(&a));
	uuid_copy(&b, &a);
	CHECK(uuid_equal(&a, &b));
	b.b[15] ^= 1;
	CHECK(!uuid_equal(&a, &b));
}

static void locks_smoke(void)
{
	spinlock_t s; struct rw_semaphore rw; struct mutex m;
	atomic_t a = ATOMIC_INIT(0); atomic64_t a64 = ATOMIC64_INIT(1ll << 40);
	refcount_t rc;

	spin_lock_init(&s); spin_lock(&s); spin_unlock(&s);
	CHECK(spin_trylock(&s)); spin_unlock(&s);
	init_rwsem(&rw);
	down_read(&rw); down_read(&rw); up_read(&rw); up_read(&rw);
	down_write(&rw); CHECK(!down_read_trylock(&rw)); up_write(&rw);
	CHECK(down_read_trylock(&rw)); CHECK(!down_write_trylock(&rw)); up_read(&rw);
	mutex_init(&m); mutex_lock(&m); CHECK(!mutex_trylock(&m)); mutex_unlock(&m);
	CHECK(mutex_trylock(&m)); mutex_unlock(&m);
	mutex_destroy(&m); spin_lock_destroy(&s); destroy_rwsem(&rw);

	atomic_inc(&a); atomic_add(4, &a);
	CHECK(atomic_read(&a) == 5);
	CHECK(atomic_dec_and_test(&a) == 0 && atomic_read(&a) == 4);
	atomic_set(&a, 1); CHECK(atomic_dec_and_test(&a));
	CHECK(atomic64_read(&a64) == (1ll << 40));
	atomic64_add(5, &a64);
	CHECK(atomic64_inc_return(&a64) == (1ll << 40) + 6);
	refcount_set(&rc, 1); refcount_inc(&rc);
	CHECK(!refcount_dec_and_test(&rc) && refcount_dec_and_test(&rc));
	rcu_read_lock(); rcu_read_unlock();
}

#ifndef _WIN32
#include <pthread.h>
struct lk_arg { spinlock_t s; struct mutex m; struct rw_semaphore rw;
		long sctr, mctr, wctr; };
static void *hammer(void *p)
{
	struct lk_arg *a = p;
	int i;
	for (i = 0; i < 20000; i++) {
		spin_lock(&a->s); a->sctr++; spin_unlock(&a->s);
		mutex_lock(&a->m); a->mctr++; mutex_unlock(&a->m);
		down_write(&a->rw); a->wctr++; up_write(&a->rw);
		down_read(&a->rw); (void)a->wctr; up_read(&a->rw);
	}
	return NULL;
}
static void locks_threads(void)
{
	struct lk_arg a;
	pthread_t t[4];
	int i;

	memset(&a, 0, sizeof(a));
	spin_lock_init(&a.s); mutex_init(&a.m); init_rwsem(&a.rw);
	for (i = 0; i < 4; i++) pthread_create(&t[i], NULL, hammer, &a);
	for (i = 0; i < 4; i++) pthread_join(t[i], NULL);
	CHECK(a.sctr == 80000 && a.mctr == 80000 && a.wctr == 80000);
}
#else
static void locks_threads(void) {}
#endif

static void alloc_cap_returns_null_beyond_cap(void)
{
	void *a, *b, *c;
	size_t base = wfs_mem_used();

	wfs_mem_set_cap(base + 4096);
	a = kmalloc(1000, GFP_KERNEL);
	CHECK(a != NULL);
	b = kzalloc(2000, GFP_KERNEL);
	CHECK(b != NULL && ((char *)b)[1999] == 0);
	c = kmalloc(2000, GFP_KERNEL);
	CHECK(c == NULL);
	CHECK(kzalloc((size_t)1 << 40, GFP_KERNEL) == NULL);
	CHECK(wfs_mem_used() >= base + 3000);
	kfree(a);
	c = kmalloc(2000, GFP_KERNEL);
	CHECK(c != NULL);
	kfree(b); kfree(c);
	CHECK(wfs_mem_used() == base);
	wfs_mem_set_cap(0);
	kfree(NULL);

	{ struct kmem_cache *kc = kmem_cache_create("t", 64, 0, 0, NULL);
	  void *o; CHECK(kc != NULL);
	  wfs_mem_set_cap(wfs_mem_used());
	  CHECK(kmem_cache_alloc(kc, GFP_KERNEL) == NULL);
	  wfs_mem_set_cap(0);
	  o = kmem_cache_zalloc(kc, GFP_KERNEL);
	  CHECK(o != NULL && ((char *)o)[63] == 0);
	  kmem_cache_free(kc, o);
	  kmem_cache_destroy(kc); }
	CHECK(wfs_mem_used() == base);
	{ void *n = kmalloc_array((size_t)1 << 62, 8, GFP_KERNEL); CHECK(n == NULL); }
	{ char *r = kvmalloc(10, GFP_KERNEL); CHECK(r != NULL); kvfree(r); }
}

static int logged;
static void log_sink(int level, const char *msg) { (void)level; (void)msg; logged++; }

static void messages_and_assert(void)
{
	wfs_log_set_hook(log_sink);
	xfs_warn(NULL, "value %d", 3);
	xfs_alert(NULL, "x");
	CHECK(logged == 2);
	wfs_log_set_hook(NULL);
	ASSERT(1 == 1);
	CHECK(wfs_assert_fatal() == 0);
	{ int z = 0; trace_xfs_shim_test_event(z, "a"); (void)z; }
}


/* ---- wfs_sched.h: tasks, waits, completions, semaphores, work queues ---- */

static int wake_early_fn(void *d)
{
	struct task_struct **t = d;

	wake_up_process(*t);	/* lands between set_current_state and schedule */
	return 7;
}

static void sched_wakeup_before_schedule_returns(void)
{
	struct task_struct *me = current, *k;

	CHECK(me == current);
	set_current_state(TASK_UNINTERRUPTIBLE);
	k = kthread_run(wake_early_fn, &me, "waker");
	CHECK(!IS_ERR(k));
	msleep(20);
	schedule();	/* would hang if the early wakeup were lost */
	CHECK(kthread_stop(k) == 7);
	CHECK(schedule_timeout_uninterruptible(2) == 0);	/* times out */
}

struct wq_arg {
	wait_queue_head_t	wq;
	int			flag;
	atomic_t		woke;
};

static int waiter_fn(void *d)
{
	struct wq_arg *a = d;

	wait_event(a->wq, READ_ONCE(a->flag) != 0);
	atomic_inc(&a->woke);
	return 0;
}

static void sched_wait_event_wakes_all(void)
{
	struct wq_arg a;
	struct task_struct *k[3];
	int i;

	memset(&a, 0, sizeof(a));
	init_waitqueue_head(&a.wq);
	for (i = 0; i < 3; i++) k[i] = kthread_run(waiter_fn, &a, "w");
	msleep(30);
	CHECK(atomic_read(&a.woke) == 0);
	WRITE_ONCE(a.flag, 1);
	wake_up_all(&a.wq);
	for (i = 0; i < 3; i++) kthread_stop(k[i]);
	CHECK(atomic_read(&a.woke) == 3);
	CHECK(!waitqueue_active(&a.wq));
}

struct comp_arg { struct completion c; struct semaphore s; atomic_t in; int max_in; };

static int comp_fn(void *d)
{
	struct comp_arg *a = d;
	int n;

	down(&a->s);
	n = atomic_inc_return(&a->in);
	{ int m = __atomic_load_n(&a->max_in, __ATOMIC_RELAXED);
	  while (n > m && !__atomic_compare_exchange_n(&a->max_in, &m, n, 0, __ATOMIC_RELAXED, __ATOMIC_RELAXED)) ; }
	msleep(5);
	atomic_dec(&a->in);
	up(&a->s);
	complete(&a->c);
	return 0;
}

static void sched_completion_and_semaphore(void)
{
	struct comp_arg a;
	struct task_struct *k[6];
	int i;

	memset(&a, 0, sizeof(a));
	init_completion(&a.c);
	sema_init(&a.s, 2);
	for (i = 0; i < 6; i++) k[i] = kthread_run(comp_fn, &a, "c");
	for (i = 0; i < 6; i++) wait_for_completion(&a.c);
	CHECK(!try_wait_for_completion(&a.c));
	for (i = 0; i < 6; i++) kthread_stop(k[i]);
	CHECK(a.max_in <= 2);
	CHECK(down_trylock(&a.s) == 0 && down_trylock(&a.s) == 0 && down_trylock(&a.s) == 1);
	up(&a.s); up(&a.s);
	{
		DECLARE_COMPLETION_ONSTACK(d);
		struct { struct completion c; } st = { .c = COMPLETION_INITIALIZER_ONSTACK(st.c) };

		complete(&d);
		wait_for_completion(&d);
		complete_all(&st.c);
		wait_for_completion(&st.c);
		wait_for_completion(&st.c);	/* complete_all: stays done */
		CHECK(completion_done(&st.c));
	}
}

struct work_arg {
	struct work_struct	w;
	struct delayed_work	dw;
	atomic_t		runs, inside, overlap;
	u64			ran_at;
};

static void work_fn(struct work_struct *w)
{
	struct work_arg *a = container_of(w, struct work_arg, w);

	if (atomic_inc_return(&a->inside) > 1)
		atomic_inc(&a->overlap);
	CHECK(current_work() == w);
	msleep(1);
	atomic_inc(&a->runs);
	atomic_dec(&a->inside);
}

static void dwork_fn(struct work_struct *w)
{
	struct work_arg *a = container_of(to_delayed_work(w), struct work_arg, dw);

	__atomic_store_n(&a->ran_at, wfs_monotonic_ns(), __ATOMIC_RELEASE);
	atomic_inc(&a->runs);
}

static void sched_workqueue_runs_and_flushes(void)
{
	struct workqueue_struct *wq = alloc_workqueue("t-%s", WQ_UNBOUND, 4, "x");
	struct work_arg a;
	u64 t0;
	int i, queued = 0;

	CHECK(wq != NULL);
	memset(&a, 0, sizeof(a));
	INIT_WORK(&a.w, work_fn);
	INIT_DELAYED_WORK(&a.dw, dwork_fn);
	for (i = 0; i < 200; i++) {
		queued += queue_work(wq, &a.w);
		if (i % 10 == 0) msleep(1);
	}
	flush_workqueue(wq);
	CHECK(!work_pending(&a.w));
	CHECK(atomic_read(&a.runs) >= 1 && atomic_read(&a.runs) <= queued);
	CHECK(atomic_read(&a.overlap) == 0);	/* never on two workers */

	atomic_set(&a.runs, 0);
	t0 = wfs_monotonic_ns();
	CHECK(queue_delayed_work(wq, &a.dw, msecs_to_jiffies(50)));
	CHECK(!queue_delayed_work(wq, &a.dw, 1));	/* already pending */
	flush_workqueue(wq);			/* timers are not waited for */
	CHECK(atomic_read(&a.runs) == 0);
	msleep(120);
	CHECK(atomic_read(&a.runs) == 1);
	CHECK(__atomic_load_n(&a.ran_at, __ATOMIC_ACQUIRE) - t0 >= 40ull * 1000000);

	CHECK(queue_delayed_work(wq, &a.dw, msecs_to_jiffies(1000)));
	CHECK(cancel_delayed_work_sync(&a.dw));
	CHECK(!delayed_work_pending(&a.dw));
	CHECK(queue_delayed_work(wq, &a.dw, msecs_to_jiffies(1000)));
	CHECK(flush_delayed_work(&a.dw));	/* runs the timer now */
	CHECK(atomic_read(&a.runs) == 2);
	CHECK(!mod_delayed_work(wq, &a.dw, 0));
	flush_delayed_work(&a.dw);
	CHECK(atomic_read(&a.runs) == 3);
	destroy_workqueue(wq);
}

static int var_word;
static int var_fn(void *d)
{
	(void)d;
	msleep(10);
	WRITE_ONCE(var_word, 1);
	wake_up_var(&var_word);
	return 0;
}

static void sched_wait_var_event(void)
{
	struct task_struct *k = kthread_run(var_fn, NULL, "v");

	wait_var_event(&var_word, READ_ONCE(var_word) == 1);
	CHECK(var_word == 1);
	kthread_stop(k);
	CHECK(time_after(jiffies + 1, jiffies) && msecs_to_jiffies(10) == 1);
}

int main(void)
{
	crc32c_known_vectors();
	crc32c_chunking_and_alignment();
	errno_values_are_linux();
	endian_roundtrip_be16_32_64();
	bitops_are_64bit_safe();
	uuid_helpers();
	locks_smoke();
	locks_threads();
	alloc_cap_returns_null_beyond_cap();
	messages_and_assert();
	sched_wakeup_before_schedule_returns();
	sched_wait_event_wakes_all();
	sched_completion_and_semaphore();
	sched_workqueue_runs_and_flushes();
	sched_wait_var_event();
	printf("test_shim: %d checks, %d failures\n", checks, failures);
	return failures ? 1 : 0;
}
