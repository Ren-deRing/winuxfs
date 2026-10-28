// SPDX-License-Identifier: GPL-2.0-only
#include "wfs_shim.h"
#include <stdio.h>

static wfs_log_fn g_hook;

void wfs_log_set_hook(wfs_log_fn fn) { __atomic_store_n(&g_hook, fn, __ATOMIC_SEQ_CST); }

static void emit(int level, const char *msg)
{
	wfs_log_fn fn = __atomic_load_n(&g_hook, __ATOMIC_SEQ_CST);

	if (fn)
		fn(level, msg);
	else
		fprintf(stderr, "XFS: %s\n", msg);
}

static void vemit(int level, const char *fmt, va_list ap)
{
	char buf[512];

	vsnprintf(buf, sizeof(buf), fmt, ap);
	emit(level, buf);
}

void xfs_printk_level(const char *kern_level, const struct xfs_mount *mp, const char *fmt, ...)
{
	va_list ap;

	(void)mp;
	va_start(ap, fmt);
	vemit(kern_level[0] - '0', fmt, ap);
	va_end(ap);
}

void _xfs_alert_tag(const struct xfs_mount *mp, uint32_t tag, const char *fmt, ...)
{
	va_list ap;

	(void)mp; (void)tag;
	va_start(ap, fmt);
	vemit(1, fmt, ap);
	va_end(ap);
}

int wfs_assert_fatal(void)
{
#ifdef WFS_ASSERT_FATAL
	return 1;
#else
	return 0;
#endif
}

void assfail(struct xfs_mount *mp, char *expr, char *f, int l)
{
	char buf[256];

	(void)mp;
	snprintf(buf, sizeof(buf), "Assertion failed: %s, file: %s, line: %d", expr, f, l);
	emit(1, buf);
#ifdef WFS_ASSERT_FATAL
	abort();
#endif
}

void asswarn(struct xfs_mount *mp, char *expr, char *f, int l)
{
	char buf[256];

	(void)mp;
	snprintf(buf, sizeof(buf), "Assertion warning: %s, file: %s, line: %d", expr, f, l);
	emit(4, buf);
}

void xfs_hex_dump(const void *p, int length)
{
	const unsigned char *b = p;
	char line[80];
	int i, n;

	for (i = 0; i < length; i += 16) {
		int j;

		n = 0;
		for (j = 0; j < 16 && i + j < length; j++)
			n += snprintf(line + n, sizeof(line) - (size_t)n, "%02x ", b[i + j]);
		emit(7, line);
	}
}

void xfs_warn_experimental(struct xfs_mount *mp, enum xfs_experimental_feat f)
{
	(void)mp; (void)f;
}
