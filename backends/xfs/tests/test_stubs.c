// SPDX-License-Identifier: GPL-2.0-only
/*
 * write_path_symbols_do_not_abort: calls the remaining stubs_ro.c entry
 * points. Built with -DWFS_ASSERT_FATAL, so any ASSERT reached would abort;
 * stubs must return their error code and log their own name instead.
 */
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include "wfs_shim.h"

struct xfs_trans;
struct xfs_buf;
struct xfs_mount;
struct xfs_perag;
struct xfs_trans_res;
struct xfs_buftarg;
struct xfs_buf_map;
struct xfs_buf_ops;
struct xfs_inode;

struct xfs_bmalloca;
void xfs_fs_mark_sick(struct xfs_mount *, unsigned);
int xfs_bmap_rtalloc(struct xfs_bmalloca *);

static int nlogs, failures;
static char last[512];

static void hook(int level, const char *msg)
{
	(void)level;
	nlogs++;
	snprintf(last, sizeof(last), "%s", msg);
}

#define CHECK(c) do { if (!(c)) { failures++; fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

int main(void)
{
	wfs_log_set_hook(hook);

	/* M3: the transaction/log symbols are the kernel's own now (xfs_trans.c
	 * and friends); what stays stubbed is outside the M3a write path. */
	CHECK(xfs_bmap_rtalloc(NULL) < 0);
	CHECK(strstr(last, "xfs_bmap_rtalloc") != NULL);
	xfs_fs_mark_sick(NULL, 0);
	CHECK(strstr(last, "xfs_fs_mark_sick") != NULL);

	/* repeated ro calls are bounded (10 log lines per function) */
	nlogs = 0;
	for (int i = 0; i < 100; i++)
		(void)xfs_bmap_rtalloc(NULL);
	CHECK(nlogs == 9);	/* 1 of the 10 logged calls was used above */

	if (failures) {
		fprintf(stderr, "%d failures\n", failures);
		return 1;
	}
	puts("stubs: OK (no abort, errors returned, names logged)");
	return 0;
}
