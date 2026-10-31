// SPDX-License-Identifier: GPL-2.0-only
/*
 * Tasks, wait queues, completions, semaphores, kthreads and work queues on
 * pthreads (see wfs_sched.h for the contract).
 */
#include <errno.h>
/* The host's values: wfs_errno.h redefines the E* names to Linux ones. */
enum { SYS_EINTR = EINTR, SYS_ETIMEDOUT = ETIMEDOUT };
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include "wfs_shim.h"

/* ---- clock ---------------------------------------------------------- */

u64 wfs_monotonic_ns(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (u64)ts.tv_sec * 1000000000ull + (u64)ts.tv_nsec;
}

unsigned long wfs_jiffies(void)
{
	return (unsigned long)(wfs_monotonic_ns() / (1000000000ull / HZ));
}

/* Absolute deadline for pthread_cond_timedwait, which uses CLOCK_REALTIME. */
static struct timespec deadline_after_ns(u64 ns)
{
	struct timespec ts;
	u64 t;

	clock_gettime(CLOCK_REALTIME, &ts);
	t = (u64)ts.tv_nsec + ns % 1000000000ull;
	ts.tv_sec += (time_t)(ns / 1000000000ull + t / 1000000000ull);
	ts.tv_nsec = (long)(t % 1000000000ull);
	return ts;
}

void msleep(unsigned int msecs)
{
	struct timespec ts = { msecs / 1000, (long)(msecs % 1000) * 1000000L };

	while (nanosleep(&ts, &ts) != 0 && errno == SYS_EINTR)
		;
}

/* ---- tasks ---------------------------------------------------------- */

struct wfs_task {
	struct task_struct	t;
	struct wfs_task		*reg_prev, *reg_next;	/* host-thread registry */
	pthread_mutex_t		mu;
	pthread_cond_t		cv;
	long			state;		/* TASK_*; RUNNING after a wakeup */
	/* kthread part */
	int			is_kthread;
	int			should_stop;
	int			(*fn)(void *);
	void			*data;
	int			result;
	pthread_t		thread;
	struct work_struct	*cur_work;	/* worker threads */
};

static pthread_key_t task_key;
static pthread_once_t task_once = PTHREAD_ONCE_INIT;
static int task_key_live;

/*
 * Host threads get their task lazily and lose it at thread exit through the
 * TSD destructor. The destructor lives in this plugin, so before the plugin
 * is unloaded the key must go (or glibc/winpthreads would call into unmapped
 * code when a host thread exits later): wfs_sched_unload() deletes it and
 * frees the tasks of host threads still alive, found through this registry.
 */
static pthread_mutex_t reg_lock = PTHREAD_MUTEX_INITIALIZER;
static struct wfs_task *reg_head;

static void task_free(struct wfs_task *k)
{
	pthread_cond_destroy(&k->cv);
	pthread_mutex_destroy(&k->mu);
	free(k);
}

static void reg_add(struct wfs_task *k)
{
	pthread_mutex_lock(&reg_lock);
	k->reg_prev = NULL;
	k->reg_next = reg_head;
	if (reg_head)
		reg_head->reg_prev = k;
	reg_head = k;
	pthread_mutex_unlock(&reg_lock);
}

static void reg_del(struct wfs_task *k)
{
	pthread_mutex_lock(&reg_lock);
	if (k->reg_prev)
		k->reg_prev->reg_next = k->reg_next;
	else
		reg_head = k->reg_next;
	if (k->reg_next)
		k->reg_next->reg_prev = k->reg_prev;
	pthread_mutex_unlock(&reg_lock);
}

/* Thread exit of a host thread: its implicit task goes with it. */
static void task_key_dtor(void *p)
{
	struct wfs_task *k = p;

	if (k && !k->is_kthread) {
		reg_del(k);
		task_free(k);
	}
}

static void task_key_init(void)
{
	if (pthread_key_create(&task_key, task_key_dtor) != 0)
		abort();
	task_key_live = 1;
}

__attribute__((destructor)) static void wfs_sched_unload(void)
{
	struct wfs_task *k, *n;

	if (!task_key_live)
		return;
	pthread_key_delete(task_key);	/* no destructor runs after this */
	task_key_live = 0;
	pthread_mutex_lock(&reg_lock);
	for (k = reg_head; k; k = n) {
		n = k->reg_next;
		task_free(k);
	}
	reg_head = NULL;
	pthread_mutex_unlock(&reg_lock);
}

static struct wfs_task *task_new(void)
{
	struct wfs_task *k = calloc(1, sizeof(*k));

	if (!k)
		abort();	/* nothing sensible to do without a task */
	pthread_mutex_init(&k->mu, NULL);
	pthread_cond_init(&k->cv, NULL);
	k->t.wfs_priv = k;
	k->state = TASK_RUNNING;
	return k;
}

static struct wfs_task *self(void)
{
	struct wfs_task *k;

	pthread_once(&task_once, task_key_init);
	k = pthread_getspecific(task_key);
	if (!k) {
		k = task_new();
		reg_add(k);
		pthread_setspecific(task_key, k);
	}
	return k;
}

static inline struct wfs_task *task_of(struct task_struct *t)
{
	return t->wfs_priv;
}

struct task_struct *wfs_current(void)
{
	return &self()->t;
}

void wfs_set_current_state(long state)
{
	struct wfs_task *k = self();

	pthread_mutex_lock(&k->mu);
	k->state = state;
	pthread_mutex_unlock(&k->mu);
}

void schedule(void)
{
	struct wfs_task *k = self();

	pthread_mutex_lock(&k->mu);
	while (k->state != TASK_RUNNING)
		pthread_cond_wait(&k->cv, &k->mu);
	pthread_mutex_unlock(&k->mu);
}

long schedule_timeout(long timeout)
{
	struct wfs_task *k = self();
	u64 start, end, now;
	struct timespec dl;

	if (timeout == MAX_SCHEDULE_TIMEOUT) {
		schedule();
		return timeout;
	}
	if (timeout < 0)
		timeout = 0;
	start = wfs_monotonic_ns();
	end = start + (u64)timeout * (1000000000ull / HZ);
	dl = deadline_after_ns(end - start);
	pthread_mutex_lock(&k->mu);
	while (k->state != TASK_RUNNING) {
		if (pthread_cond_timedwait(&k->cv, &k->mu, &dl) == SYS_ETIMEDOUT) {
			k->state = TASK_RUNNING;
			break;
		}
	}
	pthread_mutex_unlock(&k->mu);
	now = wfs_monotonic_ns();
	if (now >= end)
		return 0;
	return (long)DIV_ROUND_UP(end - now, 1000000000ull / HZ);
}

int wake_up_process(struct task_struct *p)
{
	struct wfs_task *k = task_of(p);
	int woke;

	pthread_mutex_lock(&k->mu);
	woke = k->state != TASK_RUNNING;
	k->state = TASK_RUNNING;
	pthread_cond_signal(&k->cv);
	pthread_mutex_unlock(&k->mu);
	return woke;
}

/* ---- kthreads ------------------------------------------------------- */

static void *kthread_main(void *arg)
{
	struct wfs_task *k = arg;

	pthread_once(&task_once, task_key_init);
	pthread_setspecific(task_key, k);
	k->result = k->fn(k->data);
	return NULL;
}

struct task_struct *wfs_kthread_run(int (*fn)(void *), void *data, const char *name)
{
	struct wfs_task *k = task_new();

	k->is_kthread = 1;
	k->fn = fn;
	k->data = data;
	snprintf(k->t.comm, sizeof(k->t.comm), "%s", name ? name : "kthread");
	if (pthread_create(&k->thread, NULL, kthread_main, k) != 0) {
		task_free(k);
		return ERR_PTR(-ENOMEM);
	}
	return &k->t;
}

bool kthread_should_stop(void)
{
	return __atomic_load_n(&self()->should_stop, __ATOMIC_ACQUIRE) != 0;
}

int kthread_stop(struct task_struct *t)
{
	struct wfs_task *k = task_of(t);
	int r;

	__atomic_store_n(&k->should_stop, 1, __ATOMIC_RELEASE);
	wake_up_process(t);
	pthread_join(k->thread, NULL);
	r = k->result;
	task_free(k);
	return r;
}

/* ---- wait queues ---------------------------------------------------- */

/* A statically initialized head has a {NULL, NULL} list: link it on first use. */
static inline void wq_fix(struct wait_queue_head *wq)
{
	if (!wq->head.next)
		INIT_LIST_HEAD(&wq->head);
}

int default_wake_function(struct wait_queue_entry *e, unsigned int mode, int flags, void *key)
{
	(void)mode; (void)flags; (void)key;
	return wake_up_process(e->private);
}

int autoremove_wake_function(struct wait_queue_entry *e, unsigned int mode, int flags, void *key)
{
	int r = default_wake_function(e, mode, flags, key);

	list_del_init(&e->entry);	/* under wq->lock (__wake_up) */
	return r;
}

void add_wait_queue(struct wait_queue_head *wq, struct wait_queue_entry *e)
{
	e->flags &= ~WQ_FLAG_EXCLUSIVE;
	spin_lock(&wq->lock);
	wq_fix(wq);
	list_add(&e->entry, &wq->head);
	spin_unlock(&wq->lock);
}

void add_wait_queue_exclusive(struct wait_queue_head *wq, struct wait_queue_entry *e)
{
	e->flags |= WQ_FLAG_EXCLUSIVE;
	spin_lock(&wq->lock);
	wq_fix(wq);
	list_add_tail(&e->entry, &wq->head);
	spin_unlock(&wq->lock);
}

void remove_wait_queue(struct wait_queue_head *wq, struct wait_queue_entry *e)
{
	spin_lock(&wq->lock);
	list_del(&e->entry);
	spin_unlock(&wq->lock);
}

static void prepare(struct wait_queue_head *wq, struct wait_queue_entry *e, int state, int excl)
{
	if (excl)
		e->flags |= WQ_FLAG_EXCLUSIVE;
	else
		e->flags &= ~WQ_FLAG_EXCLUSIVE;
	spin_lock(&wq->lock);
	wq_fix(wq);
	if (list_empty(&e->entry)) {
		if (excl)
			list_add_tail(&e->entry, &wq->head);
		else
			list_add(&e->entry, &wq->head);
	}
	/* State before releasing the lock: a waker that finds us sets RUNNING. */
	wfs_set_current_state(state);
	spin_unlock(&wq->lock);
}

void prepare_to_wait(struct wait_queue_head *wq, struct wait_queue_entry *e, int state)
{
	prepare(wq, e, state, 0);
}

void prepare_to_wait_exclusive(struct wait_queue_head *wq, struct wait_queue_entry *e, int state)
{
	prepare(wq, e, state, 1);
}

void finish_wait(struct wait_queue_head *wq, struct wait_queue_entry *e)
{
	wfs_set_current_state(TASK_RUNNING);
	/*
	 * The kernel peeks at the entry without the lock (list_empty_careful);
	 * neighbours being unlinked write our pointers under the lock, so take
	 * it: correct without relying on benign races.
	 */
	spin_lock(&wq->lock);
	if (!list_empty(&e->entry))
		list_del_init(&e->entry);
	spin_unlock(&wq->lock);
}

void __wake_up(struct wait_queue_head *wq, unsigned int mode, int nr_exclusive, void *key)
{
	struct wait_queue_entry *e, *n;

	spin_lock(&wq->lock);
	wq_fix(wq);
	list_for_each_entry_safe(e, n, &wq->head, entry) {
		unsigned int flags = e->flags;

		if (e->func(e, mode, 0, key) && (flags & WQ_FLAG_EXCLUSIVE) &&
		    nr_exclusive > 0 && !--nr_exclusive)
			break;
	}
	spin_unlock(&wq->lock);
}

#define HASHED_WQS 256
static struct wait_queue_head hashed_wqs[HASHED_WQS];	/* zero = valid empty heads */

struct wait_queue_head *wfs_hashed_waitqueue(const void *addr)
{
	uintptr_t a = (uintptr_t)addr;

	a ^= a >> 17;
	a *= 0x9E3779B97F4A7C15ull;
	return &hashed_wqs[(a >> 32) % HASHED_WQS];
}

/* ---- completions ---------------------------------------------------- */

void complete(struct completion *x)
{
	spin_lock(&x->wait.lock);
	if (x->done != COMPLETION_DONE_ALL)
		x->done++;
	spin_unlock(&x->wait.lock);
	__wake_up(&x->wait, TASK_NORMAL, 1, NULL);
}

void complete_all(struct completion *x)
{
	spin_lock(&x->wait.lock);
	x->done = COMPLETION_DONE_ALL;
	spin_unlock(&x->wait.lock);
	wake_up_all(&x->wait);
}

static bool completion_take(struct completion *x)
{
	bool got = false;

	spin_lock(&x->wait.lock);
	if (x->done) {
		if (x->done != COMPLETION_DONE_ALL)
			x->done--;
		got = true;
	}
	spin_unlock(&x->wait.lock);
	return got;
}

void wait_for_completion(struct completion *x)
{
	DEFINE_WAIT(w);

	for (;;) {
		prepare_to_wait_exclusive(&x->wait, &w, TASK_UNINTERRUPTIBLE);
		if (completion_take(x))
			break;
		schedule();
	}
	finish_wait(&x->wait, &w);
}

bool try_wait_for_completion(struct completion *x)
{
	return completion_take(x);
}

bool completion_done(struct completion *x)
{
	bool d;

	spin_lock(&x->wait.lock);
	d = x->done != 0;
	spin_unlock(&x->wait.lock);
	return d;
}

/* ---- semaphores ----------------------------------------------------- */

static bool sem_take(struct semaphore *s)
{
	unsigned int c = __atomic_load_n(&s->count, __ATOMIC_RELAXED);

	while (c) {
		if (__atomic_compare_exchange_n(&s->count, &c, c - 1, 1,
						__ATOMIC_SEQ_CST, __ATOMIC_RELAXED))
			return true;
	}
	return false;
}

static inline void wfs_pause(void)
{
#if defined(__x86_64__) || defined(__i386__)
	__builtin_ia32_pause();
#elif defined(__aarch64__)
	__asm__ __volatile__("yield");
#endif
}

/* Spin budget before sleeping: buffer locks are held for microseconds, and a
 * sleep/wake pair costs two context switches (like the kernel's mutex spin). */
#define SEM_SPINS 200

/*
 * No lost wakeup: a sleeper increments `waiters` and enqueues itself before
 * its final sem_take(); up() increments `count` before reading `waiters`.
 * Both are seq_cst, so either the sleeper sees the count or up() sees the
 * sleeper (which is already on the queue) and wakes it.
 */
void down(struct semaphore *s)
{
	DEFINE_WAIT(w);
	int i;

	for (i = 0; i < SEM_SPINS; i++) {
		if (sem_take(s))
			return;
		wfs_pause();
	}
	__atomic_add_fetch(&s->waiters, 1, __ATOMIC_SEQ_CST);
	for (;;) {
		prepare_to_wait_exclusive(&s->wait, &w, TASK_UNINTERRUPTIBLE);
		if (sem_take(s))
			break;
		schedule();
	}
	finish_wait(&s->wait, &w);
	__atomic_sub_fetch(&s->waiters, 1, __ATOMIC_SEQ_CST);
}

int down_trylock(struct semaphore *s)
{
	return sem_take(s) ? 0 : 1;
}

void up(struct semaphore *s)
{
	__atomic_add_fetch(&s->count, 1, __ATOMIC_SEQ_CST);
	if (__atomic_load_n(&s->waiters, __ATOMIC_SEQ_CST))
		__wake_up(&s->wait, TASK_NORMAL, 1, NULL);
}

/* ---- work queues ---------------------------------------------------- */

#define WQ_MAX_WORKERS 4

struct workqueue_struct {
	char			name[32];
	pthread_mutex_t		mu;
	pthread_cond_t		cv;		/* workers: new work or stop */
	pthread_cond_t		idle_cv;	/* flushers: progress */
	struct list_head	ready;		/* queued work_structs */
	struct list_head	timers;		/* delayed works not yet due */
	struct work_struct	*running[WQ_MAX_WORKERS];
	int			nworkers;
	int			stopping;
	pthread_t		threads[WQ_MAX_WORKERS];
	struct wfs_task		*tasks[WQ_MAX_WORKERS];
};

static bool running_elsewhere(struct workqueue_struct *wq, struct work_struct *w)
{
	int i;

	for (i = 0; i < wq->nworkers; i++)
		if (wq->running[i] == w)
			return true;
	return false;
}

static u64 promote_due_timers(struct workqueue_struct *wq)
{
	struct work_struct *w, *n;
	u64 now = wfs_monotonic_ns(), next = 0;

	list_for_each_entry_safe(w, n, &wq->timers, entry) {
		struct delayed_work *dw = to_delayed_work(w);

		if (dw->due_ns <= now) {
			dw->timer = 0;
			list_move_tail(&w->entry, &wq->ready);
		} else if (!next || dw->due_ns < next) {
			next = dw->due_ns;
		}
	}
	return next;
}

struct worker_arg { struct workqueue_struct *wq; int idx; };

static void *worker_main(void *arg)
{
	struct worker_arg a = *(struct worker_arg *)arg;
	struct workqueue_struct *wq = a.wq;
	struct wfs_task *me;

	free(arg);
	me = self();
	reg_del(me);
	me->is_kthread = 1;	/* freed by destroy_workqueue, not the key dtor */
	pthread_mutex_lock(&wq->mu);
	wq->tasks[a.idx] = me;
	for (;;) {
		struct work_struct *w = NULL, *c;
		u64 next = promote_due_timers(wq);

		list_for_each_entry(c, &wq->ready, entry) {
			if (!running_elsewhere(wq, c)) {
				w = c;
				break;
			}
		}
		if (w) {
			list_del_init(&w->entry);
			__atomic_and_fetch(&w->pending, ~1ul, __ATOMIC_ACQ_REL);
			wq->running[a.idx] = w;
			me->cur_work = w;
			pthread_mutex_unlock(&wq->mu);
			w->func(w);
			pthread_mutex_lock(&wq->mu);
			me->cur_work = NULL;
			wq->running[a.idx] = NULL;
			pthread_cond_broadcast(&wq->idle_cv);
			/* A requeue that waited for us may now run elsewhere. */
			if (!list_empty(&wq->ready))
				pthread_cond_broadcast(&wq->cv);
			continue;
		}
		if (wq->stopping && list_empty(&wq->timers))
			break;
		if (next) {
			struct timespec dl = deadline_after_ns(next - min(next, wfs_monotonic_ns()));

			pthread_cond_timedwait(&wq->cv, &wq->mu, &dl);
		} else {
			pthread_cond_wait(&wq->cv, &wq->mu);
		}
	}
	pthread_mutex_unlock(&wq->mu);
	return NULL;
}

struct workqueue_struct *wfs_alloc_workqueue(const char *name, unsigned int flags, int max_active)
{
	struct workqueue_struct *wq = calloc(1, sizeof(*wq));
	int i, n;

	(void)flags;
	if (!wq)
		return NULL;
	snprintf(wq->name, sizeof(wq->name), "%s", name ? name : "wq");
	pthread_mutex_init(&wq->mu, NULL);
	pthread_cond_init(&wq->cv, NULL);
	pthread_cond_init(&wq->idle_cv, NULL);
	INIT_LIST_HEAD(&wq->ready);
	INIT_LIST_HEAD(&wq->timers);
	n = max_active <= 0 || max_active > WQ_MAX_WORKERS ? WQ_MAX_WORKERS : max_active;
	for (i = 0; i < n; i++) {
		struct worker_arg *a = malloc(sizeof(*a));

		if (!a)
			break;
		a->wq = wq;
		a->idx = i;
		if (pthread_create(&wq->threads[i], NULL, worker_main, a) != 0) {
			free(a);
			break;
		}
		wq->nworkers++;
	}
	if (!wq->nworkers) {
		destroy_workqueue(wq);
		return NULL;
	}
	return wq;
}

void destroy_workqueue(struct workqueue_struct *wq)
{
	int i;

	if (!wq)
		return;
	drain_workqueue(wq);
	pthread_mutex_lock(&wq->mu);
	wq->stopping = 1;
	pthread_cond_broadcast(&wq->cv);
	pthread_mutex_unlock(&wq->mu);
	for (i = 0; i < wq->nworkers; i++)
		pthread_join(wq->threads[i], NULL);
	for (i = 0; i < wq->nworkers; i++)
		if (wq->tasks[i])
			task_free(wq->tasks[i]);
	pthread_cond_destroy(&wq->idle_cv);
	pthread_cond_destroy(&wq->cv);
	pthread_mutex_destroy(&wq->mu);
	free(wq);
}

bool queue_work(struct workqueue_struct *wq, struct work_struct *w)
{
	if (__atomic_fetch_or(&w->pending, 1ul, __ATOMIC_ACQ_REL) & 1)
		return false;
	pthread_mutex_lock(&wq->mu);
	w->wq = wq;
	list_add_tail(&w->entry, &wq->ready);
	pthread_cond_signal(&wq->cv);
	pthread_mutex_unlock(&wq->mu);
	return true;
}

static void arm_locked(struct workqueue_struct *wq, struct delayed_work *dw, unsigned long delay)
{
	struct work_struct *w = &dw->work;

	w->wq = wq;
	if (!delay) {
		list_add_tail(&w->entry, &wq->ready);
	} else {
		dw->due_ns = wfs_monotonic_ns() + (u64)delay * (1000000000ull / HZ);
		dw->timer = 1;
		list_add_tail(&w->entry, &wq->timers);
	}
	pthread_cond_signal(&wq->cv);
}

bool queue_delayed_work(struct workqueue_struct *wq, struct delayed_work *dw, unsigned long delay)
{
	if (__atomic_fetch_or(&dw->work.pending, 1ul, __ATOMIC_ACQ_REL) & 1)
		return false;
	pthread_mutex_lock(&wq->mu);
	arm_locked(wq, dw, delay);
	pthread_mutex_unlock(&wq->mu);
	return true;
}

/* Take a queued (ready or timer) item off its queue. Caller holds wq->mu. */
static bool unqueue_locked(struct workqueue_struct *wq, struct work_struct *w)
{
	if (!(__atomic_load_n(&w->pending, __ATOMIC_ACQUIRE) & 1) || list_empty(&w->entry))
		return false;
	list_del_init(&w->entry);
	__atomic_and_fetch(&w->pending, ~1ul, __ATOMIC_ACQ_REL);
	pthread_cond_broadcast(&wq->idle_cv);
	return true;
}

bool mod_delayed_work(struct workqueue_struct *wq, struct delayed_work *dw, unsigned long delay)
{
	bool was;

	pthread_mutex_lock(&wq->mu);
	was = unqueue_locked(wq, &dw->work);
	__atomic_fetch_or(&dw->work.pending, 1ul, __ATOMIC_ACQ_REL);
	arm_locked(wq, dw, delay);
	pthread_mutex_unlock(&wq->mu);
	return was;
}

static void wait_not_running_locked(struct workqueue_struct *wq, struct work_struct *w)
{
	while (running_elsewhere(wq, w))
		pthread_cond_wait(&wq->idle_cv, &wq->mu);
}

bool cancel_work_sync(struct work_struct *w)
{
	struct workqueue_struct *wq = w->wq;
	bool was;

	if (!wq)
		return false;
	pthread_mutex_lock(&wq->mu);
	was = unqueue_locked(wq, w);
	wait_not_running_locked(wq, w);
	pthread_mutex_unlock(&wq->mu);
	return was;
}

bool cancel_delayed_work_sync(struct delayed_work *dw)
{
	return cancel_work_sync(&dw->work);
}

bool cancel_delayed_work(struct delayed_work *dw)
{
	struct workqueue_struct *wq = dw->work.wq;
	bool was;

	if (!wq)
		return false;
	pthread_mutex_lock(&wq->mu);
	was = unqueue_locked(wq, &dw->work);
	pthread_mutex_unlock(&wq->mu);
	return was;
}

static bool flush_work_locked(struct workqueue_struct *wq, struct work_struct *w)
{
	bool waited = false;

	while ((__atomic_load_n(&w->pending, __ATOMIC_ACQUIRE) & 1) || running_elsewhere(wq, w)) {
		waited = true;
		pthread_cond_wait(&wq->idle_cv, &wq->mu);
	}
	return waited;
}

bool flush_work(struct work_struct *w)
{
	struct workqueue_struct *wq = w->wq;
	bool waited;

	if (!wq)
		return false;
	pthread_mutex_lock(&wq->mu);
	waited = flush_work_locked(wq, w);
	pthread_mutex_unlock(&wq->mu);
	return waited;
}

bool flush_delayed_work(struct delayed_work *dw)
{
	struct workqueue_struct *wq = dw->work.wq;
	bool waited = false;

	if (!wq)
		return false;
	pthread_mutex_lock(&wq->mu);
	if (dw->timer) {	/* the kernel runs a pending timer right away */
		dw->timer = 0;
		list_move_tail(&dw->work.entry, &wq->ready);
		pthread_cond_signal(&wq->cv);
		waited = true;
	}
	waited |= flush_work_locked(wq, &dw->work);
	pthread_mutex_unlock(&wq->mu);
	return waited;
}

static bool queued_or_running_locked(struct workqueue_struct *wq, struct work_struct *w)
{
	struct work_struct *c;

	if (running_elsewhere(wq, w))
		return true;
	list_for_each_entry(c, &wq->ready, entry)
		if (c == w)
			return true;
	return false;
}

/*
 * Wait for every item that was ready or running at the call (the kernel's
 * flush). Delayed works whose timer has not fired are not waited for.
 */
void flush_workqueue(struct workqueue_struct *wq)
{
	struct work_struct **snap, *c;
	size_t n = 0, cap = WQ_MAX_WORKERS, i;
	bool busy;
	int k;

	pthread_mutex_lock(&wq->mu);
	list_for_each_entry(c, &wq->ready, entry)
		cap++;
	snap = malloc(cap * sizeof(*snap));
	if (!snap) {
		pthread_mutex_unlock(&wq->mu);
		drain_workqueue(wq);	/* stronger, needs no memory */
		return;
	}
	list_for_each_entry(c, &wq->ready, entry)
		snap[n++] = c;
	for (k = 0; k < wq->nworkers; k++)
		if (wq->running[k])
			snap[n++] = wq->running[k];
	do {
		busy = false;
		for (i = 0; i < n && !busy; i++)
			busy = queued_or_running_locked(wq, snap[i]);
		if (busy)
			pthread_cond_wait(&wq->idle_cv, &wq->mu);
	} while (busy);
	pthread_mutex_unlock(&wq->mu);
	free(snap);
}

void drain_workqueue(struct workqueue_struct *wq)
{
	pthread_mutex_lock(&wq->mu);
	for (;;) {
		bool busy = !list_empty(&wq->ready);
		int i;

		for (i = 0; i < wq->nworkers; i++)
			busy |= wq->running[i] != NULL;
		if (!busy)
			break;
		pthread_cond_wait(&wq->idle_cv, &wq->mu);
	}
	pthread_mutex_unlock(&wq->mu);
}

struct work_struct *current_work(void)
{
	return self()->cur_work;
}
