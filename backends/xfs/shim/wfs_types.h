/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Basic kernel types, compiler attributes and small helpers.
 *
 * LLP64 rule (see LLP64.md): never rely on sizeof(long). Everything that must
 * be 64 bit uses u64/s64/loff_t/xfs_off_t; `unsigned long` appears only where
 * the width is irrelevant (bit numbers < 32, opstate words, loop indexes).
 */
#ifndef WFS_SHIM_TYPES_H
#define WFS_SHIM_TYPES_H

#ifdef _WIN32
# ifndef __USE_MINGW_ANSI_STDIO
#  define __USE_MINGW_ANSI_STDIO 1	/* C99 printf (%zu, %llx) and its format checks */
# endif
#endif
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <limits.h>
#include <string.h>
#include <stdlib.h>
#include <stdarg.h>
#include <assert.h>
#include <strings.h>
#include <sys/types.h>

#if !defined(__x86_64__) && !defined(__aarch64__)
# error "winuxfs XFS shim supports 64-bit targets only"
#endif

/*
 * Kernel semantics: 64-bit integers are `long long` on every target. On LP64
 * hosts (glibc) <stdint.h> says `long`, which would make u64/uint64_t and
 * loff_t/xfs_off_t distinct types and break %llx formats and pointer types in
 * kernel-derived code. After <stdint.h> is in, the names are redefined once.
 */
#if defined(__LP64__) && !defined(_WIN32)
# define int64_t	long long
# define uint64_t	unsigned long long
# define intmax_t	long long
# define uintmax_t	unsigned long long
#endif

typedef uint8_t  u8;  typedef int8_t  s8;
typedef uint16_t u16; typedef int16_t s16;
typedef uint32_t u32; typedef int32_t s32;
typedef uint64_t u64; typedef int64_t s64;
typedef uint8_t  __u8;  typedef int8_t  __s8;
typedef uint16_t __u16; typedef int16_t __s16;
typedef uint32_t __u32; typedef int32_t __s32;
typedef uint64_t __u64; typedef int64_t __s64;

/* Endianness-annotated integers (no sparse here: plain integers). */
typedef uint16_t __be16; typedef uint32_t __be32; typedef uint64_t __be64;
typedef uint16_t __le16; typedef uint32_t __le32; typedef uint64_t __le64;

/* glibc's loff_t is `long`: force 64-bit long long on every host. */
#undef loff_t
#define loff_t long long
typedef unsigned int gfp_t;
typedef unsigned int uint;	/* may already exist in glibc: same type */
typedef unsigned char uchar;
#ifdef _WIN32
typedef unsigned int uid_t;
typedef unsigned int gid_t;
#endif
typedef long long time64_t;
struct timespec64 { s64 tv_sec; s64 tv_nsec; };
typedef u32 kuid_t;
typedef u32 kgid_t;
typedef u32 prid_t;
typedef u32 umode_t;
struct mnt_idmap;
#define NSEC_PER_SEC	1000000000L
#define USEC_PER_SEC	1000000L
#define MSEC_PER_SEC	1000L
typedef unsigned short ushort;

/* Linux 64-bit ABI types used by xfs_fs.h ioctl structures (not on-disk). */
typedef long long __kernel_long_t;
typedef unsigned long long __kernel_ulong_t;
typedef int64_t __kernel_loff_t;
#define _IOC(dir, type, nr, size) \
	(((unsigned)(dir) << 30) | ((unsigned)(size) << 16) | ((unsigned)(type) << 8) | (unsigned)(nr))
#define _IO(type, nr)		_IOC(0, type, nr, 0)
#define _IOR(type, nr, sz)	_IOC(2, type, nr, sizeof(sz))
#define _IOW(type, nr, sz)	_IOC(1, type, nr, sizeof(sz))
#define _IOWR(type, nr, sz)	_IOC(3, type, nr, sizeof(sz))

#define S64_MAX		INT64_MAX
#define S64_MIN		INT64_MIN
#define S32_MAX		INT32_MAX
#define S32_MIN		INT32_MIN
#define U64_MAX		UINT64_MAX
#define U32_MAX		UINT32_MAX
#define U16_MAX		UINT16_MAX
#define U8_MAX		UINT8_MAX
#define S16_MAX		INT16_MAX
#define S8_MAX		INT8_MAX

/* Compiler attributes (host headers may define some of these). */
#undef likely
#undef unlikely
#undef noinline
#undef __always_inline
#undef __packed
#undef __aligned
#undef __maybe_unused
#undef __must_check
#undef __printf
#undef __pure
#undef __cold
#undef __init
#undef __nonstring
#undef __exit
#undef __read_mostly
#undef __user
#undef __rcu
#undef __percpu
#undef __force
#undef __bitwise
#undef __iomem
#undef fallthrough
#undef barrier
#undef __stringify
#undef __stringify_1
#define likely(x)	__builtin_expect(!!(x), 1)
#define unlikely(x)	__builtin_expect(!!(x), 0)
#define noinline	__attribute__((noinline))
#define __always_inline	inline __attribute__((always_inline))
#define __packed	__attribute__((packed))
#define __aligned(x)	__attribute__((aligned(x)))
#define __maybe_unused	__attribute__((unused))
#define __must_check	__attribute__((warn_unused_result))
#ifdef _WIN32
# define __printf(a, b)	__attribute__((format(gnu_printf, a, b)))	/* our wfs_log formats the C99 way */
#else
# define __printf(a, b)	__attribute__((format(printf, a, b)))
#endif
#define __pure		__attribute__((pure))
#define __cold		__attribute__((cold))
#define __nonstring
#define __init
#define __exit
#define __read_mostly
#define __user
#define __rcu
#define __percpu
#define __force
#define __bitwise
#define __iomem
#define __counted_by(x)
#define ____cacheline_aligned
#define __cacheline_aligned
#define fallthrough	__attribute__((fallthrough))
#define barrier()	__asm__ __volatile__("" ::: "memory")
#define __stringify_1(x)	#x
#define __stringify(x)		__stringify_1(x)
#define IS_ENABLED(x)	0
#define EXPORT_SYMBOL(x)
#define EXPORT_SYMBOL_GPL(x)
#define EXPORT_SYMBOL_IF_KUNIT(x)
#define MODULE_LICENSE(x)
#define KERN_EMERG	"0"
#define KERN_ALERT	"1"
#define KERN_CRIT	"2"
#define KERN_ERR	"3"
#define KERN_WARNING	"4"
#define KERN_NOTICE	"5"
#define KERN_INFO	"6"
#define KERN_DEBUG	"7"

#define READ_ONCE(x)		(*(const volatile __typeof__(x) *)&(x))
#define WRITE_ONCE(x, v)	(*(volatile __typeof__(x) *)&(x) = (v))
#define ACCESS_ONCE(x)		READ_ONCE(x)

#define ARRAY_SIZE(a)		(sizeof(a) / sizeof((a)[0]))
#define sizeof_field(t, f)	sizeof(((t *)0)->f)
#define container_of(ptr, type, member) \
	((type *)((char *)(ptr) - offsetof(type, member)))
#define BUILD_BUG_ON(c)		_Static_assert(!(c), "BUILD_BUG_ON")
#define BUILD_BUG_ON_ZERO(e)	((int)sizeof(struct { int : (-!!(e)); }))
#define __same_type(a, b)	__builtin_types_compatible_p(__typeof__(a), __typeof__(b))

/* Small math helpers. */
#undef min
#undef max
#define min(a, b)	({ __typeof__(a) _a = (a); __typeof__(b) _b = (b); _a < _b ? _a : _b; })
#define max(a, b)	({ __typeof__(a) _a = (a); __typeof__(b) _b = (b); _a > _b ? _a : _b; })
#define min_t(t, a, b)	({ t _a = (a); t _b = (b); _a < _b ? _a : _b; })
#define max_t(t, a, b)	({ t _a = (a); t _b = (b); _a > _b ? _a : _b; })
#define clamp(v, lo, hi) min((__typeof__(v))(hi), max((__typeof__(v))(lo), (v)))
#define clamp_t(t, v, lo, hi) min_t(t, max_t(t, (v), (lo)), (hi))
#define swap(a, b)	do { __typeof__(a) _t = (a); (a) = (b); (b) = _t; } while (0)
#define abs_diff(a, b)	({ __typeof__(a) _a = (a); __typeof__(b) _b = (b); \
			   _a > _b ? _a - _b : _b - _a; })
#define DIV_ROUND_UP(n, d)	(((n) + (d) - 1) / (d))
#define DIV_ROUND_UP_ULL(n, d)	((((unsigned long long)(n)) + (d) - 1) / (d))
#define DIV_ROUND_DOWN_ULL(n, d) (((unsigned long long)(n)) / (d))
#define DIV_ROUND_CLOSEST(n, d)	(((n) + (d) / 2) / (d))
#define __ALIGN_MASK(x, mask)	(((x) + (mask)) & ~(mask))
#define ALIGN(x, a)		__ALIGN_MASK(x, (__typeof__(x))(a) - 1)
#define ALIGN_DOWN(x, a)	ALIGN((x) - ((a) - 1), (a))
#define IS_ALIGNED(x, a)	(((x) & ((__typeof__(x))(a) - 1)) == 0)
#define round_up(x, y)		((((x) - 1) | ((__typeof__(x))(y) - 1)) + 1)
#define round_down(x, y)	((x) & ~((__typeof__(x))(y) - 1))
#define roundup(x, y)		((((x) + ((y) - 1)) / (y)) * (y))
#define rounddown(x, y)		((x) - ((x) % (y)))

/* File mode helpers (Linux numbers; not the C runtime's). */
#undef S_IFMT
#undef S_IFSOCK
#undef S_IFLNK
#undef S_IFREG
#undef S_IFBLK
#undef S_IFDIR
#undef S_IFCHR
#undef S_IFIFO
#define S_IFMT		0170000
#define S_IFSOCK	0140000
#define S_IFLNK		0120000
#define S_IFREG		0100000
#define S_IFBLK		0060000
#define S_IFDIR		0040000
#define S_IFCHR		0020000
#define S_IFIFO		0010000
#define S_ISUID		0004000
#define S_ISGID		0002000
#define S_ISVTX		0001000
#undef S_ISLNK
#undef S_ISREG
#undef S_ISDIR
#undef S_ISCHR
#undef S_ISBLK
#undef S_ISFIFO
#undef S_ISSOCK
#define S_ISLNK(m)	(((m) & S_IFMT) == S_IFLNK)
#define S_ISREG(m)	(((m) & S_IFMT) == S_IFREG)
#define S_ISDIR(m)	(((m) & S_IFMT) == S_IFDIR)
#define S_ISCHR(m)	(((m) & S_IFMT) == S_IFCHR)
#define S_ISBLK(m)	(((m) & S_IFMT) == S_IFBLK)
#define S_ISFIFO(m)	(((m) & S_IFMT) == S_IFIFO)
#define S_ISSOCK(m)	(((m) & S_IFMT) == S_IFSOCK)
#define S_IRWXUGO	0000777
#define inode_get_atime(i)	inode_get_atime_ts(i)
#define inode_get_mtime(i)	inode_get_mtime_ts(i)
#define inode_get_ctime(i)	inode_get_ctime_ts(i)
#define S_IALLUGO	0007777

/* GFP flags: accepted and ignored except __GFP_ZERO. */
#define __GFP_ZERO	0x01u
#define __GFP_NOFAIL	0x02u
#define __GFP_NOLOCKDEP	0x04u
#define __GFP_RETRY_MAYFAIL 0x08u
#define __GFP_NOWARN	0x10u
#define GFP_KERNEL	0u
#define GFP_NOFS	0u
#define GFP_NOIO	0u
#define GFP_ATOMIC	0u
#define GFP_NOWAIT	0u
#define KM_NOFS		GFP_NOFS
#define KM_ZERO		__GFP_ZERO
#define KM_MAYFAIL	__GFP_RETRY_MAYFAIL
#define KM_NOLOCKDEP	__GFP_NOLOCKDEP

/* UUIDs. */
typedef struct { __u8 b[16]; } uuid_t;
#define UUID_SIZE	16
static inline bool uuid_equal(const uuid_t *a, const uuid_t *b)
{ return memcmp(a, b, sizeof(uuid_t)) == 0; }
static inline void uuid_copy(uuid_t *dst, const uuid_t *src)
{ memcpy(dst, src, sizeof(uuid_t)); }
static inline bool uuid_is_null(const uuid_t *u)
{
	static const uuid_t nil;
	return uuid_equal(u, &nil);
}
static inline void import_uuid(uuid_t *dst, const __u8 *src)
{ memcpy(dst->b, src, UUID_SIZE); }
static inline void export_uuid(__u8 *dst, const uuid_t *src)
{ memcpy(dst, src->b, UUID_SIZE); }
#define UUID_STRING_LEN	36

/* Minimal intrusive list, enough for libxfs and the buffer cache. */
struct list_head { struct list_head *next, *prev; };
#define LIST_HEAD_INIT(n)	{ &(n), &(n) }
#define LIST_HEAD(n)		struct list_head n = LIST_HEAD_INIT(n)
static inline void INIT_LIST_HEAD(struct list_head *l) { l->next = l; l->prev = l; }
static inline void __list_add(struct list_head *n, struct list_head *p, struct list_head *x)
{ x->prev = n; n->next = x; n->prev = p; p->next = n; }
static inline void list_add(struct list_head *n, struct list_head *h) { __list_add(n, h, h->next); }
static inline void list_add_tail(struct list_head *n, struct list_head *h) { __list_add(n, h->prev, h); }
static inline void list_del(struct list_head *e) { e->next->prev = e->prev; e->prev->next = e->next; e->next = e->prev = NULL; }
static inline void list_del_init(struct list_head *e) { e->next->prev = e->prev; e->prev->next = e->next; INIT_LIST_HEAD(e); }
static inline int list_empty(const struct list_head *h) { return h->next == h; }
#define list_entry(p, t, m)	container_of(p, t, m)
#define list_first_entry(p, t, m) list_entry((p)->next, t, m)
#define list_next_entry(p, m)	list_entry((p)->m.next, __typeof__(*(p)), m)
#define list_for_each(pos, head) for (pos = (head)->next; pos != (head); pos = pos->next)
#define list_for_each_entry(pos, head, m) \
	for (pos = list_first_entry(head, __typeof__(*pos), m); &pos->m != (head); pos = list_next_entry(pos, m))
#define list_for_each_entry_safe(pos, n, head, m) \
	for (pos = list_first_entry(head, __typeof__(*pos), m), n = list_next_entry(pos, m); \
	     &pos->m != (head); pos = n, n = list_next_entry(n, m))

/* sort(): qsort wrapper (swap function ignored; elements are plain memory). */
typedef int (*cmp_func_t)(const void *, const void *);
static inline void sort(void *base, size_t num, size_t size, cmp_func_t cmp, void *swap_fn)
{ (void)swap_fn; qsort(base, num, size, cmp); }
#define list_sort_cmp_t	int (*)(void *, const struct list_head *, const struct list_head *)
void list_sort(void *priv, struct list_head *head,
	       int (*cmp)(void *priv, const struct list_head *a, const struct list_head *b));

/* Types libxfs structs mention but this read-only port never operates on. */
struct rcu_head { void *next; };
struct completion { int done; };
struct work_struct { void *func; };
struct delayed_work { struct work_struct work; };
struct workqueue_struct;
struct super_block;
struct block_device;
struct dentry;
struct kobject { int unused; };
struct shrinker;
struct file;
struct dax_device;
struct cpumask { int unused; };
struct llist_head { void *first; };
typedef struct { int unused; } wait_queue_head_t;
struct xarray { void **xa_slots; unsigned char *xa_marks; unsigned long xa_n; };
struct ratelimit_state { int unused; };
struct semaphore { int count; };
struct rb_node { unsigned long __rb_parent_color; struct rb_node *rb_right, *rb_left; };
struct rb_root { struct rb_node *rb_node; };
struct iomap;
struct kvec;
struct seq_file;
struct page;
struct kiocb;
struct iov_iter;
#define init_completion(c)	((c)->done = 0)
#define complete(c)		((c)->done = 1)
#define wait_for_completion(c)	((void)0)
#define init_waitqueue_head(w)	((void)(w))

/* RCU: the read-only port takes no RCU-protected locks. */
#define rcu_read_lock()		((void)0)
#define rcu_read_unlock()	((void)0)
#define rcu_dereference(p)	READ_ONCE(p)
#define rcu_dereference_protected(p, c) (p)
#define rcu_assign_pointer(p, v) WRITE_ONCE(p, v)
#define RCU_INIT_POINTER(p, v)	((p) = (v))
#define synchronize_rcu()	((void)0)
#define cond_resched()		((void)0)
#define might_sleep()		((void)0)
#define smp_mb()		__atomic_thread_fence(__ATOMIC_SEQ_CST)
#define smp_rmb()		__atomic_thread_fence(__ATOMIC_ACQUIRE)
#define smp_wmb()		__atomic_thread_fence(__ATOMIC_RELEASE)


#define _THIS_IP_		({ __label__ __l; __l: (uintptr_t)&&__l; })
#define _RET_IP_		((uintptr_t)__builtin_return_address(0))
#define smp_load_acquire(p)	__atomic_load_n((p), __ATOMIC_ACQUIRE)
#define smp_store_release(p, v)	__atomic_store_n((p), (v), __ATOMIC_RELEASE)
#define check_add_overflow(a, b, d)	__builtin_add_overflow(a, b, d)
#define check_sub_overflow(a, b, d)	__builtin_sub_overflow(a, b, d)
#define check_mul_overflow(a, b, d)	__builtin_mul_overflow(a, b, d)
#define struct_size_t(type, member, n) \
	(sizeof(type) + sizeof(((type *)0)->member[0]) * (size_t)(n))
#define struct_size(p, member, n) struct_size_t(__typeof__(*(p)), member, n)
#define max3(a, b, c)		max(max(a, b), c)
#define min3(a, b, c)		min(min(a, b), c)
#define __counted_by_ptr(x)
#define kfree_rcu_mightsleep(p)	kfree(p)
#define cmp_int(a, b)		(((a) > (b)) - ((a) < (b)))
static inline u32 rol32(u32 w, unsigned int s) { s &= 31; return s ? (w << s) | (w >> (32 - s)) : w; }
static inline u32 ror32(u32 w, unsigned int s) { s &= 31; return s ? (w >> s) | (w << (32 - s)) : w; }
static inline void *memchr_inv(const void *p, int c, size_t n)
{
	const unsigned char *b = p; size_t i;
	for (i = 0; i < n; i++) if (b[i] != (unsigned char)c) return (void *)(b + i);
	return NULL;
}
/* Allocation-scope flags are meaningless without direct reclaim. */
#define memalloc_nofs_save()		0u
#define memalloc_nofs_restore(f)	((void)(f))
#define memalloc_noio_save()		0u
#define memalloc_noio_restore(f)	((void)(f))
#define DECLARE_BITMAP(name, bits) unsigned long name[DIV_ROUND_UP((bits), (int)(sizeof(long) * 8))]
static inline u32 get_random_u32(void) { return (u32)rand() ^ ((u32)rand() << 16); }
#define GLOBAL_ROOT_UID		0u
#define GLOBAL_ROOT_GID		0u
#define current_fsuid()		0u
#define current_fsgid()		0u
/* ffz: first zero bit (word-size independent of LLP64). */
#define ffz(x)			((unsigned long)__builtin_ctzl(~(unsigned long)(x)))
/* Take a type or an object expression; optional gfp flags. */
#define kzalloc_obj(p, ...)	kzalloc(sizeof(p), (__VA_ARGS__ + 0) | __GFP_ZERO)
#define kmalloc_objs(p, n, ...)	kmalloc_array((n), sizeof(p), (__VA_ARGS__ + 0))
#define kmalloc_obj(p, ...)	kmalloc(sizeof(p), (__VA_ARGS__ + 0))
#define kzalloc_objs(p, n, ...)	kcalloc((n), sizeof(p), (__VA_ARGS__ + 0))

#endif /* WFS_SHIM_TYPES_H */
