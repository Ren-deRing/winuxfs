/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Sleeping and threads with kernel semantics (M3 design 2, M3a T2): tasks,
 * wait queues, completions, semaphores, kthreads, work queues, var/bit waits
 * and a monotonic jiffies clock.
 *
 * Every thread that enters the backend gets a task_struct on first use of
 * `current` (host threads too); it is freed when the thread exits. The task
 * protocol is the kernel's: set_current_state(TASK_UNINTERRUPTIBLE), check
 * the condition, schedule(); a wake_up_process() that lands in between makes
 * the task runnable again, so schedule() returns at once. Wait-queue wakers
 * call wake_up_process() through the entry's wake function.
 *
 * Work queues run their items on real worker threads (not inline: kernel code
 * queues work under spinlocks). An item never runs on two workers at once.
 *
 * Implementation in wfs_sched.c (pthread on every target, including mingw's
 * winpthreads; no threading headers leak into kernel-derived code).
 */
#ifndef WFS_SHIM_SCHED_H
#define WFS_SHIM_SCHED_H

#include "wfs_types.h"
#include "wfs_sync.h"

/* ---- clock ---------------------------------------------------------- */
#define HZ			100
#define MAX_SCHEDULE_TIMEOUT	((long)(~0UL >> 1))
u64 wfs_monotonic_ns(void);
unsigned long wfs_jiffies(void);
#define jiffies			wfs_jiffies()
#define msecs_to_jiffies(m)	((unsigned long)DIV_ROUND_UP((unsigned long)(m) * HZ, 1000))
#define usecs_to_jiffies(u)	((unsigned long)DIV_ROUND_UP((unsigned long)(u) * HZ, 1000000))
#define jiffies_to_msecs(j)	((unsigned int)((j) * (1000 / HZ)))
#define time_after(a, b)	((long)((b) - (a)) < 0)
#define time_before(a, b)	time_after(b, a)
#define time_after_eq(a, b)	((long)((a) - (b)) >= 0)
#define time_before_eq(a, b)	time_after_eq(b, a)
#define time_is_before_jiffies(a) time_after(jiffies, a)
void msleep(unsigned int msecs);
#define delay(ticks)		msleep(jiffies_to_msecs((unsigned long)(ticks)))
#define cond_resched()		((void)0)
#define cpu_relax()		((void)0)

/* ---- tasks ---------------------------------------------------------- */
#define TASK_RUNNING		0x0000
#define TASK_INTERRUPTIBLE	0x0001
#define TASK_UNINTERRUPTIBLE	0x0002
#define TASK_KILLABLE		TASK_UNINTERRUPTIBLE	/* no signals here */
#define TASK_IDLE		TASK_UNINTERRUPTIBLE
#define TASK_FREEZABLE		0

#define PF_MEMALLOC		0x00000800
#define PF_MEMALLOC_NOFS	0x00040000
#define PF_KSWAPD		0x00020000

struct task_struct {
	unsigned int	flags;		/* PF_* (bookkeeping only) */
	u32		fsuid, fsgid;	/* owner for inodes this thread creates */
	char		comm[16];
	void		*wfs_priv;	/* wfs_sched.c */
};
struct task_struct *wfs_current(void);
/* Set the owner the next inode creations on this thread get. */
void wfs_set_current_fsids(u32 uid, u32 gid);
#define current			wfs_current()

void wfs_set_current_state(long state);
#define set_current_state(s)	wfs_set_current_state(s)
#define __set_current_state(s)	wfs_set_current_state(s)
void schedule(void);
#define io_schedule()		schedule()
long schedule_timeout(long timeout);
#define schedule_timeout_uninterruptible(t) \
	(set_current_state(TASK_UNINTERRUPTIBLE), schedule_timeout(t))
#define schedule_timeout_interruptible(t) \
	(set_current_state(TASK_INTERRUPTIBLE), schedule_timeout(t))
#define schedule_timeout_idle(t)	schedule_timeout_uninterruptible(t)
int wake_up_process(struct task_struct *p);
#define signal_pending(p)	0
#define fatal_signal_pending(p)	0

/* Freezer, memalloc scopes: no system suspend or reclaim here. */
#define set_freezable()		((void)0)
#define try_to_freeze()		false
#define freezing(p)		false
static inline unsigned int memalloc_noreclaim_save(void) { return 0; }
static inline void memalloc_noreclaim_restore(unsigned int f) { (void)f; }
static inline unsigned int memalloc_nofs_save(void) { return 0; }
static inline void memalloc_nofs_restore(unsigned int f) { (void)f; }

/* ---- kthreads ------------------------------------------------------- */
struct task_struct *wfs_kthread_run(int (*fn)(void *), void *data, const char *name);
#define kthread_run(fn, data, fmt, ...)	wfs_kthread_run(fn, data, fmt)
bool kthread_should_stop(void);
int kthread_stop(struct task_struct *k);

/* ---- wait queues ---------------------------------------------------- */
struct wait_queue_entry;
typedef int (*wait_queue_func_t)(struct wait_queue_entry *, unsigned int mode, int flags, void *key);
#define WQ_FLAG_EXCLUSIVE	0x01
struct wait_queue_entry {
	unsigned int		flags;
	void			*private;	/* the task */
	wait_queue_func_t	func;
	struct list_head	entry;
};
typedef struct wait_queue_entry wait_queue_entry_t;
struct wait_queue_head {
	spinlock_t		lock;
	struct list_head	head;
};
typedef struct wait_queue_head wait_queue_head_t;

int default_wake_function(struct wait_queue_entry *, unsigned int, int, void *);
int autoremove_wake_function(struct wait_queue_entry *, unsigned int, int, void *);

static inline void init_waitqueue_head(struct wait_queue_head *wq)
{
	spin_lock_init(&wq->lock);
	INIT_LIST_HEAD(&wq->head);
}
#define DECLARE_WAITQUEUE(name, tsk) \
	struct wait_queue_entry name = { .private = (tsk), .func = default_wake_function, \
					 .entry = { NULL, NULL } }
#define DEFINE_WAIT_FUNC(name, fn) \
	struct wait_queue_entry name = { .private = current, .func = (fn), \
					 .entry = LIST_HEAD_INIT((name).entry) }
#define DEFINE_WAIT(name)	DEFINE_WAIT_FUNC(name, autoremove_wake_function)
struct wait_bit_queue_entry { struct wait_queue_entry wq_entry; };
#define DEFINE_WAIT_BIT(name, word, bit) \
	struct wait_bit_queue_entry name = { .wq_entry = { .private = current, \
		.func = autoremove_wake_function, .entry = LIST_HEAD_INIT((name).wq_entry.entry) } }

void add_wait_queue(struct wait_queue_head *wq, struct wait_queue_entry *e);
void add_wait_queue_exclusive(struct wait_queue_head *wq, struct wait_queue_entry *e);
void remove_wait_queue(struct wait_queue_head *wq, struct wait_queue_entry *e);
void prepare_to_wait(struct wait_queue_head *wq, struct wait_queue_entry *e, int state);
void prepare_to_wait_exclusive(struct wait_queue_head *wq, struct wait_queue_entry *e, int state);
void finish_wait(struct wait_queue_head *wq, struct wait_queue_entry *e);
/* Wake every non-exclusive waiter and up to nr_exclusive exclusive ones (0 = all). */
void __wake_up(struct wait_queue_head *wq, unsigned int mode, int nr_exclusive, void *key);
#define wake_up(wq)		__wake_up(wq, TASK_NORMAL, 1, NULL)
#define wake_up_nr(wq, nr)	__wake_up(wq, TASK_NORMAL, nr, NULL)
#define wake_up_all(wq)		__wake_up(wq, TASK_NORMAL, 0, NULL)
#define wake_up_interruptible(wq) wake_up(wq)
#define TASK_NORMAL		(TASK_INTERRUPTIBLE | TASK_UNINTERRUPTIBLE)
static inline int waitqueue_active(struct wait_queue_head *wq)
{
	struct list_head *n = __atomic_load_n(&wq->head.next, __ATOMIC_ACQUIRE);

	return n != NULL && n != &wq->head;	/* NULL: never used (static init) */
}
static inline bool wq_has_sleeper(struct wait_queue_head *wq)
{
	smp_mb();
	return waitqueue_active(wq);
}

#define wait_event(wq, condition)					\
do {									\
	DEFINE_WAIT(__wait);						\
	for (;;) {							\
		prepare_to_wait(&(wq), &__wait, TASK_UNINTERRUPTIBLE);	\
		if (condition)						\
			break;						\
		schedule();						\
	}								\
	finish_wait(&(wq), &__wait);					\
} while (0)
#define wait_event_killable(wq, condition)	({ wait_event(wq, condition); 0; })
#define wait_event_interruptible(wq, condition)	({ wait_event(wq, condition); 0; })

/* Hashed queues for var and bit waits (spurious wakeups are re-checked). */
struct wait_queue_head *wfs_hashed_waitqueue(const void *addr);
#define bit_waitqueue(word, bit)	wfs_hashed_waitqueue(word)
#define __var_waitqueue(var)		wfs_hashed_waitqueue(var)
#define wait_var_event(var, condition)					\
do {									\
	struct wait_queue_head *__wq = __var_waitqueue(var);		\
	DEFINE_WAIT(__wait);						\
	for (;;) {							\
		prepare_to_wait(__wq, &__wait, TASK_UNINTERRUPTIBLE);	\
		if (condition)						\
			break;						\
		schedule();						\
	}								\
	finish_wait(__wq, &__wait);					\
} while (0)
/* No barrier needed: __wake_up takes the queue lock the waiter enqueued under. */
static inline void wake_up_var(void *var) { wake_up_all(__var_waitqueue(var)); }
static inline void wake_up_bit(void *word, int bit) { (void)bit; wake_up_all(bit_waitqueue(word, bit)); }

/* ---- completions ---------------------------------------------------- */
struct completion {
	unsigned int		done;
	struct wait_queue_head	wait;
};
#define COMPLETION_DONE_ALL	(~0u)
static inline void init_completion(struct completion *x)
{
	x->done = 0;
	init_waitqueue_head(&x->wait);
}
static inline void reinit_completion(struct completion *x) { x->done = 0; }
/* A completion embeds a self-linked list head: initialize it in place, never copy. */
#define DECLARE_COMPLETION_ONSTACK(name) \
	struct completion name; init_completion(&name)
/*
 * The kernel's is a designated initializer; with an empty wait list
 * represented as {NULL, NULL} (repaired on first use by the wait helpers) a
 * plain initializer works and the struct may be built in place.
 */
#define COMPLETION_INITIALIZER_ONSTACK(work) \
	{ .done = 0, .wait = { .head = { NULL, NULL } } }
void complete(struct completion *x);
void complete_all(struct completion *x);
void wait_for_completion(struct completion *x);
#define wait_for_completion_io(x)	wait_for_completion(x)
bool try_wait_for_completion(struct completion *x);
bool completion_done(struct completion *x);

/* ---- semaphores ----------------------------------------------------- */
/*
 * count and waiters are atomics: take/release are lock-free when uncontended;
 * the wait queue (and its lock) is used only when a down() has to sleep.
 */
struct semaphore {
	unsigned int		count;
	unsigned int		waiters;	/* down()s in the slow path */
	struct wait_queue_head	wait;
};
static inline void sema_init(struct semaphore *s, int val)
{
	s->count = (unsigned int)val;
	s->waiters = 0;
	init_waitqueue_head(&s->wait);
}
void down(struct semaphore *s);
int down_trylock(struct semaphore *s);	/* 0 = acquired (kernel convention) */
void up(struct semaphore *s);

/* ---- work queues ---------------------------------------------------- */
struct work_struct;
typedef void (*work_func_t)(struct work_struct *);
struct workqueue_struct;
struct work_struct {
	struct list_head	entry;
	work_func_t		func;
	unsigned long		pending;	/* bit 0: queued */
	struct workqueue_struct	*wq;
};
struct delayed_work {
	struct work_struct	work;
	u64			due_ns;
	int			timer;		/* waiting for due_ns */
};
#define INIT_WORK(w, f) \
	do { INIT_LIST_HEAD(&(w)->entry); (w)->func = (f); (w)->pending = 0; (w)->wq = NULL; } while (0)
#define INIT_DELAYED_WORK(dw, f) \
	do { INIT_WORK(&(dw)->work, f); (dw)->due_ns = 0; (dw)->timer = 0; } while (0)
#define INIT_WORK_ONSTACK(w, f)	INIT_WORK(w, f)
#define destroy_work_on_stack(w)	((void)(w))
#define to_delayed_work(w)	container_of(w, struct delayed_work, work)
#define work_pending(w)		(__atomic_load_n(&(w)->pending, __ATOMIC_ACQUIRE) & 1)
#define delayed_work_pending(dw) work_pending(&(dw)->work)

#define WQ_UNBOUND		0x0002
#define WQ_FREEZABLE		0x0004
#define WQ_MEM_RECLAIM		0x0008
#define WQ_HIGHPRI		0x0010
#define WQ_CPU_INTENSIVE	0x0020
#define WQ_SYSFS		0x0040
#define WQ_PERCPU		0x0080
struct workqueue_struct *wfs_alloc_workqueue(const char *name, unsigned int flags, int max_active);
#define alloc_workqueue(fmt, flags, max_active, ...) \
	wfs_alloc_workqueue(fmt, flags, max_active)
#define alloc_ordered_workqueue(fmt, flags, ...) \
	wfs_alloc_workqueue(fmt, flags, 1)
void destroy_workqueue(struct workqueue_struct *wq);
bool queue_work(struct workqueue_struct *wq, struct work_struct *w);
bool queue_delayed_work(struct workqueue_struct *wq, struct delayed_work *dw, unsigned long delay);
bool mod_delayed_work(struct workqueue_struct *wq, struct delayed_work *dw, unsigned long delay);
bool cancel_work_sync(struct work_struct *w);
bool cancel_delayed_work_sync(struct delayed_work *dw);
bool cancel_delayed_work(struct delayed_work *dw);
bool flush_work(struct work_struct *w);
bool flush_delayed_work(struct delayed_work *dw);
/* Wait until everything queued before the call has run. */
void flush_workqueue(struct workqueue_struct *wq);
void drain_workqueue(struct workqueue_struct *wq);
/* The work item the calling worker is running, or NULL. */
struct work_struct *current_work(void);

#endif /* WFS_SHIM_SCHED_H */
