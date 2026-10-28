/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * xfs_warn/xfs_err/... , ASSERT and the log sink.
 *
 * Messages go through a replaceable sink (the backend sets one that forwards
 * to the host log). ASSERT is a no-op in release builds (corruption must
 * never kill the process, design 9); -DWFS_ASSERT_FATAL (tests, fuzzing)
 * makes a failing ASSERT abort. XFS_IS_CORRUPT is in xfs_platform.h.
 */
#ifndef WFS_SHIM_LOG_H
#define WFS_SHIM_LOG_H

#include "wfs_types.h"

struct xfs_mount;
struct xfs_buf;

/* level is the KERN_* digit as an int 0..7. */
typedef void (*wfs_log_fn)(int level, const char *msg);
void wfs_log_set_hook(wfs_log_fn fn);	/* NULL restores stderr */

__printf(3, 4)
void xfs_printk_level(const char *kern_level, const struct xfs_mount *mp,
		      const char *fmt, ...);

#define xfs_emerg(mp, fmt, ...)	xfs_printk_level(KERN_EMERG, mp, fmt, ##__VA_ARGS__)
#define xfs_alert(mp, fmt, ...)	xfs_printk_level(KERN_ALERT, mp, fmt, ##__VA_ARGS__)
#define xfs_crit(mp, fmt, ...)	xfs_printk_level(KERN_CRIT, mp, fmt, ##__VA_ARGS__)
#define xfs_err(mp, fmt, ...)	xfs_printk_level(KERN_ERR, mp, fmt, ##__VA_ARGS__)
#define xfs_warn(mp, fmt, ...)	xfs_printk_level(KERN_WARNING, mp, fmt, ##__VA_ARGS__)
#define xfs_notice(mp, fmt, ...) xfs_printk_level(KERN_NOTICE, mp, fmt, ##__VA_ARGS__)
#define xfs_info(mp, fmt, ...)	xfs_printk_level(KERN_INFO, mp, fmt, ##__VA_ARGS__)
#ifdef DEBUG
#define xfs_debug(mp, fmt, ...)	xfs_printk_level(KERN_DEBUG, mp, fmt, ##__VA_ARGS__)
#else
#define xfs_debug(mp, fmt, ...)	do { if (0) xfs_printk_level(KERN_DEBUG, mp, fmt, ##__VA_ARGS__); } while (0)
#endif

__printf(3, 4)
void _xfs_alert_tag(const struct xfs_mount *mp, uint32_t tag, const char *fmt, ...);
#define xfs_alert_tag(mp, tag, fmt, ...) _xfs_alert_tag(mp, tag, fmt, ##__VA_ARGS__)

/* Rate limiting: each call site passes its first 10 messages. */
#define xfs_printk_ratelimited(func, dev, fmt, ...)		\
do {								\
	static int _wfs_rl_count;				\
	if (__atomic_fetch_add(&_wfs_rl_count, 1, __ATOMIC_RELAXED) < 10) \
		func(dev, fmt, ##__VA_ARGS__);			\
} while (0)
#define xfs_printk_once(func, dev, fmt, ...)			\
do {								\
	static int _wfs_once;					\
	if (!__atomic_exchange_n(&_wfs_once, 1, __ATOMIC_RELAXED)) \
		func(dev, fmt, ##__VA_ARGS__);			\
} while (0)
#define xfs_emerg_ratelimited(dev, fmt, ...)  xfs_printk_ratelimited(xfs_emerg, dev, fmt, ##__VA_ARGS__)
#define xfs_alert_ratelimited(dev, fmt, ...)  xfs_printk_ratelimited(xfs_alert, dev, fmt, ##__VA_ARGS__)
#define xfs_crit_ratelimited(dev, fmt, ...)   xfs_printk_ratelimited(xfs_crit, dev, fmt, ##__VA_ARGS__)
#define xfs_err_ratelimited(dev, fmt, ...)    xfs_printk_ratelimited(xfs_err, dev, fmt, ##__VA_ARGS__)
#define xfs_warn_ratelimited(dev, fmt, ...)   xfs_printk_ratelimited(xfs_warn, dev, fmt, ##__VA_ARGS__)
#define xfs_notice_ratelimited(dev, fmt, ...) xfs_printk_ratelimited(xfs_notice, dev, fmt, ##__VA_ARGS__)
#define xfs_info_ratelimited(dev, fmt, ...)   xfs_printk_ratelimited(xfs_info, dev, fmt, ##__VA_ARGS__)
#define xfs_debug_ratelimited(dev, fmt, ...)  xfs_printk_ratelimited(xfs_debug, dev, fmt, ##__VA_ARGS__)
#define xfs_warn_once(dev, fmt, ...)    xfs_printk_once(xfs_warn, dev, fmt, ##__VA_ARGS__)
#define xfs_notice_once(dev, fmt, ...)  xfs_printk_once(xfs_notice, dev, fmt, ##__VA_ARGS__)
#define xfs_info_once(dev, fmt, ...)    xfs_printk_once(xfs_info, dev, fmt, ##__VA_ARGS__)

void assfail(struct xfs_mount *mp, char *expr, char *f, int l);
void asswarn(struct xfs_mount *mp, char *expr, char *f, int l);
int wfs_assert_fatal(void);	/* 1 when built with -DWFS_ASSERT_FATAL */
void xfs_hex_dump(const void *p, int length);

enum xfs_experimental_feat {
	XFS_EXPERIMENTAL_SHRINK,
	XFS_EXPERIMENTAL_LARP,
	XFS_EXPERIMENTAL_MAX,
};
void xfs_warn_experimental(struct xfs_mount *mp, enum xfs_experimental_feat f);

#define WARN_ON(c)	({ bool _w = !!(c); if (unlikely(_w)) asswarn(NULL, #c, __FILE__, __LINE__); _w; })
#define WARN_ON_ONCE(c)	WARN_ON(c)
#define WARN_ONCE(c, ...) WARN_ON(c)
#define WARN(c, ...)	WARN_ON(c)
#define BUG_ON(c)	((void)(unlikely(c) ? assfail(NULL, #c, __FILE__, __LINE__) : (void)0))

#define ASSERT_ALWAYS(expr) \
	(likely(expr) ? (void)0 : assfail(NULL, #expr, (char *)__FILE__, __LINE__))
#ifdef WFS_ASSERT_FATAL
# define ASSERT(expr)	ASSERT_ALWAYS(expr)
#else
/* Never evaluated, but the expression still type-checks and marks its
 * variables used. */
# define ASSERT(expr)	((void)(0 && (expr)))
#endif

#endif /* WFS_SHIM_LOG_H */
