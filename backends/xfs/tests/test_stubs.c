// SPDX-License-Identifier: GPL-2.0-only
/*
 * write_path_symbols_do_not_abort: calls stubs_ro.c / stubs_pending.c entry
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

int xfs_trans_alloc(struct xfs_mount *, struct xfs_trans_res *, unsigned, unsigned, unsigned, struct xfs_trans **);
int xfs_trans_commit(struct xfs_trans *);
int xfs_defer_finish(struct xfs_trans **);
int xfs_trans_roll(struct xfs_trans **);
int xfs_buf_delwri_submit(struct list_head *);
int xfs_dec_freecounter(struct xfs_mount *, int, uint64_t, bool);
void xfs_trans_log_inode(struct xfs_trans *, struct xfs_inode *, unsigned);
void xfs_fs_mark_sick(struct xfs_mount *, unsigned);
bool xfs_log_check_lsn(struct xfs_mount *, int64_t);
bool xfs_attr_check_namespace(unsigned);

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
	struct xfs_trans *tp = NULL;

	wfs_log_set_hook(hook);

	CHECK(xfs_trans_alloc(NULL, NULL, 0, 0, 0, &tp) == -EROFS);
	CHECK(strstr(last, "xfs_trans_alloc") != NULL);
	CHECK(xfs_trans_commit(NULL) == -EROFS);
	CHECK(xfs_defer_finish(&tp) == -EROFS);
	CHECK(xfs_trans_roll(&tp) == -EROFS);
	CHECK(xfs_buf_delwri_submit(NULL) == -EROFS);
	CHECK(xfs_dec_freecounter(NULL, 0, 1, false) == -EROFS);
	xfs_trans_log_inode(NULL, NULL, 0);
	xfs_fs_mark_sick(NULL, 0);
	CHECK(strstr(last, "xfs_fs_mark_sick") != NULL);

	/* PENDING stubs: error, logged once. */
	nlogs = 0;
	CHECK(xfs_log_check_lsn(NULL, 1));
	CHECK(xfs_log_check_lsn(NULL, 1));
	CHECK(nlogs == 1);
	CHECK(strstr(last, "xfs_log_check_lsn") != NULL);

	/* repeated ro calls are bounded (10 log lines per function) */
	nlogs = 0;
	for (int i = 0; i < 100; i++)
		xfs_trans_commit(NULL);
	CHECK(nlogs == 9);	/* 1 of the 10 logged calls was used above */

	/* real two-line copies */
	CHECK(xfs_attr_check_namespace(0));
	CHECK(!xfs_attr_check_namespace(0xff));

	if (failures) {
		fprintf(stderr, "%d failures\n", failures);
		return 1;
	}
	puts("stubs: OK (no abort, errors returned, names logged)");
	return 0;
}
