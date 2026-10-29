// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2000-2006 Silicon Graphics, Inc.
 * Copyright (c) 2000-2005 Silicon Graphics, Inc.
 * All Rights Reserved.
 *
 * winuxfs: dirty-log detection (excerpt-port, M2 design 6).
 *
 * Derived from the Linux kernel, pinned commit
 * d135ab453a7400a34930bdda1ef4bdc8acfac80e, fs/xfs/xfs_log_recover.c
 * (vendored at third_party/linux-xfs/xfs_log_recover.c) and, for xlog_cksum,
 * fs/xfs/xfs_log.c (not vendored; read from the pinned checkout).
 * Excerpted functions (kernel line ranges) and what changed:
 *   xlog_verify_bno          xfs_log_recover.c  57-69     as is
 *   xlog_alloc_buffer        xfs_log_recover.c  76-107    plain calloc, size cap
 *   xlog_align/xlog_do_io/xlog_bread/xlog_bread_noalign
 *                            xfs_log_recover.c 114-171    reads go through the
 *                            struct wfs_log_reader; write path removed
 *   xlog_header_check_mount  xfs_log_recover.c 240-257    uuid compare on params
 *   xlog_find_cycle_start    xfs_log_recover.c 269-300    as is
 *   xlog_find_verify_cycle   xfs_log_recover.c 312-366    as is
 *   xlog_logrec_hblks        xfs_log_recover.c 369-379    as is
 *   xlog_find_verify_log_record  xfs_log_recover.c 394-473  as is, ASSERT(0) removed
 *   xlog_find_head           xfs_log_recover.c 498-732    as is
 *   xlog_rseek_logrec_hdr    xfs_log_recover.c 763-823    as is
 *   xlog_seek_logrec_hdr     xfs_log_recover.c 835-896    as is
 *   xlog_tail_distance       xfs_log_recover.c 901-910    as is
 *   xlog_verify_tail         xfs_log_recover.c 928-994    retry cap added
 *   xlog_verify_head         xfs_log_recover.c 1020-1111  as is
 *   xlog_wrap_logbno         xfs_log_recover.c 1118-1127  as is
 *   xlog_check_unmount_rec   xfs_log_recover.c 1134-1180  no in-core state
 *   xlog_find_tail           xfs_log_recover.c 1236-1380  state/stale-block
 *                            clearing (writes) removed; rhead is copied out
 *                            of the shared read buffer (the kernel re-reads
 *                            into it in xlog_check_unmount_rec)
 *   xlog_find_zeroed         xfs_log_recover.c 1389-1470  as is
 *   xlog_valid_rec_header    xfs_log_recover.c 2964-3000  h_size bounded
 *   xlog_recover_process (CRC pass branch only)  2891-2929
 *   xlog_do_recovery_pass (CRC pass only)  3012-3320  rewritten: the record
 *                            is read into one contiguous buffer with a
 *                            wrap-aware helper instead of the split reads;
 *                            visits the same records, loop count bounded
 *   xlog_cksum               xfs_log.c        1509-1535   as is
 * Hardening beyond the kernel (the image is untrusted): every on-disk value
 * (h_len, h_size, hblks, block numbers, LSNs) is range checked before use,
 * ASSERTs became error returns, and the retry loop in xlog_verify_tail is
 * capped.
 */
#include "xfs_platform.h"
/* the kernel gets offsetofend() from linux/stddef.h; the shim has no copy */
#ifndef offsetofend
#define offsetofend(T, m) (offsetof(T, m) + sizeof(((T *)0)->m))
#endif
#include "xfs_fs.h"
#include "xfs_shared.h"
#include "xfs_format.h"
#include "xfs_log_format.h"
#include "xfs_cksum.h"
#include "xfs_trans_resv.h"
#include "xfs_bit.h"
#include "xfs_mount.h"
#include "logcheck.h"

#include <stdlib.h>
#include <string.h>

#define BLK_AVG(blk1, blk2)	((blk1+blk2) >> 1)

#define XLOG_REC_SHIFT_BB(logv2) \
	BTOBB(1 << ((logv2) ? XLOG_MAX_RECORD_BSHIFT : XLOG_BIG_RECORD_BSHIFT))
#define XLOG_TOTAL_REC_SHIFT_BB(logv2) \
	BTOBB(XLOG_MAX_ICLOGS << ((logv2) ? XLOG_MAX_RECORD_BSHIFT : \
				  XLOG_BIG_RECORD_BSHIFT))

#define WFS_LOG_MIN_BB		32	/* real logs are >= 10 MiB */
#define WFS_LOG_VERIFY_TAIL_RETRIES	(4 * XLOG_MAX_ICLOGS)

/* Reduced struct xlog: only what the head/tail search needs. */
struct xlog {
	const struct wfs_log_params	*l_p;
	const struct wfs_log_reader	*l_r;
	xfs_daddr_t			l_logBBsize;
	int				l_sectBBsize;
	bool				l_logv2;
	bool				l_msgs;
};

/* One log record header block, copied out of any read buffer. */
union wfs_rhead {
	struct xlog_rec_header	h;
	char			raw[BBSIZE];
};

#define log_warn(log, fmt, ...) \
	do { if ((log)->l_msgs) xfs_warn(NULL, fmt, ##__VA_ARGS__); } while (0)

/*
 * Sector aligned buffer routines for buffer create/read.
 * (xfs_log_recover.c 48-171)
 */
static inline bool
xlog_verify_bno(struct xlog *log, xfs_daddr_t blk_no, int bbcount)
{
	if (blk_no < 0 || blk_no >= log->l_logBBsize)
		return false;
	if (bbcount <= 0 || (blk_no + bbcount) > log->l_logBBsize)
		return false;
	return true;
}

static char *
xlog_alloc_buffer(struct xlog *log, int nbblks)
{
	if (!xlog_verify_bno(log, 0, nbblks)) {
		log_warn(log, "Invalid block length (0x%x) for buffer", nbblks);
		return NULL;
	}
	if (nbblks > 1 && log->l_sectBBsize > 1)
		nbblks += log->l_sectBBsize;
	nbblks = round_up(nbblks, log->l_sectBBsize);
	return calloc(1, BBTOB((size_t)nbblks));
}

static inline unsigned int
xlog_align(struct xlog *log, xfs_daddr_t blk_no)
{
	return BBTOB(blk_no & ((xfs_daddr_t)log->l_sectBBsize - 1));
}

static int
xlog_do_io(struct xlog *log, xfs_daddr_t blk_no, unsigned int nbblks,
	   char *data)
{
	uint64_t	off;

	if (!xlog_verify_bno(log, blk_no, nbblks)) {
		log_warn(log, "Invalid log block/length (0x%llx, 0x%x) for buffer",
			 (unsigned long long)blk_no, nbblks);
		return -EFSCORRUPTED;
	}

	blk_no = round_down(blk_no, log->l_sectBBsize);
	nbblks = round_up(nbblks, log->l_sectBBsize);
	/* the log size is a multiple of the sector size (checked up front) */
	if (blk_no + nbblks > log->l_logBBsize)
		return -EFSCORRUPTED;

	off = log->l_p->log_start_byte + BBTOB((uint64_t)blk_no);
	if (log->l_r->read(log->l_r->ctx, off, data, BBTOB((size_t)nbblks))) {
		log_warn(log, "log read I/O error at daddr 0x%llx len %d",
			 (unsigned long long)blk_no, nbblks);
		return -EIO;
	}
	return 0;
}

static int
xlog_bread(struct xlog *log, xfs_daddr_t blk_no, int nbblks, char *data,
	   char **offset)
{
	int	error;

	error = xlog_do_io(log, blk_no, nbblks, data);
	if (!error)
		*offset = data + xlog_align(log, blk_no);
	return error;
}

/*
 * Read nbblks from blk_no (which may be >= the log size, and the range may
 * cross the physical end) into a contiguous buffer. winuxfs replacement for
 * the split-read code in xlog_do_recovery_pass.
 */
static int
xlog_bread_wrapped(struct xlog *log, xfs_daddr_t blk_no, int nbblks,
		   char *dest)
{
	char		*tmp, *offset;
	int		first, error;

	if (nbblks <= 0 || nbblks > log->l_logBBsize)
		return -EFSCORRUPTED;
	blk_no %= log->l_logBBsize;
	first = (int)min_t(xfs_daddr_t, nbblks, log->l_logBBsize - blk_no);

	tmp = xlog_alloc_buffer(log, nbblks);
	if (!tmp)
		return -ENOMEM;
	error = xlog_bread(log, blk_no, first, tmp, &offset);
	if (error)
		goto out;
	memcpy(dest, offset, BBTOB((size_t)first));
	if (first < nbblks) {
		error = xlog_bread(log, 0, nbblks - first, tmp, &offset);
		if (error)
			goto out;
		memcpy(dest + BBTOB((size_t)first), offset,
		       BBTOB((size_t)(nbblks - first)));
	}
out:
	free(tmp);
	return error;
}

/*
 * read the head block of the log and check the header (240-257)
 */
static int
xlog_header_check_mount(struct xlog *log, struct xlog_rec_header *head)
{
	static const uint8_t	null_uuid[16];

	if (!memcmp(&head->h_fs_uuid, null_uuid, 16)) {
		/* IRIX style log: continue */
		log_warn(log, "null uuid in log - IRIX style log");
	} else if (memcmp(&head->h_fs_uuid, log->l_p->uuid, 16)) {
		log_warn(log, "log has mismatched uuid - can't recover");
		return -EFSCORRUPTED;
	}
	return 0;
}

/* (269-300) */
static int
xlog_find_cycle_start(struct xlog *log, char *buffer, xfs_daddr_t first_blk,
		      xfs_daddr_t *last_blk, uint cycle)
{
	char		*offset;
	xfs_daddr_t	mid_blk;
	xfs_daddr_t	end_blk;
	uint		mid_cycle;
	int		error;

	end_blk = *last_blk;
	mid_blk = BLK_AVG(first_blk, end_blk);
	while (mid_blk != first_blk && mid_blk != end_blk) {
		error = xlog_bread(log, mid_blk, 1, buffer, &offset);
		if (error)
			return error;
		mid_cycle = xlog_get_cycle(offset);
		if (mid_cycle == cycle)
			end_blk = mid_blk;   /* last_half_cycle == mid_cycle */
		else
			first_blk = mid_blk; /* first_half_cycle == mid_cycle */
		mid_blk = BLK_AVG(first_blk, end_blk);
	}
	*last_blk = end_blk;
	return 0;
}

/* (312-366) */
static int
xlog_find_verify_cycle(struct xlog *log, xfs_daddr_t start_blk, int nbblks,
		       uint stop_on_cycle_no, xfs_daddr_t *new_blk)
{
	xfs_daddr_t	i, j;
	uint		cycle;
	char		*buffer;
	xfs_daddr_t	bufblks;
	char		*buf = NULL;
	int		error = 0;

	bufblks = roundup_pow_of_two(nbblks);
	while (bufblks > log->l_logBBsize)
		bufblks >>= 1;
	while (!(buffer = xlog_alloc_buffer(log, bufblks))) {
		bufblks >>= 1;
		if (bufblks < log->l_sectBBsize)
			return -ENOMEM;
	}

	for (i = start_blk; i < start_blk + nbblks; i += bufblks) {
		int	bcount;

		bcount = min(bufblks, (start_blk + nbblks - i));

		error = xlog_bread(log, i, bcount, buffer, &buf);
		if (error)
			goto out;

		for (j = 0; j < bcount; j++) {
			cycle = xlog_get_cycle(buf);
			if (cycle == stop_on_cycle_no) {
				*new_blk = i+j;
				goto out;
			}

			buf += BBSIZE;
		}
	}

	*new_blk = -1;

out:
	free(buffer);
	return error;
}

/* (369-379) */
static inline int
xlog_logrec_hblks(struct xlog *log, struct xlog_rec_header *rh)
{
	if (log->l_logv2) {
		int	h_size = be32_to_cpu(rh->h_size);

		if ((be32_to_cpu(rh->h_version) & XLOG_VERSION_2) &&
		    h_size > XLOG_HEADER_CYCLE_SIZE)
			return DIV_ROUND_UP(h_size, XLOG_HEADER_CYCLE_SIZE);
	}
	return 1;
}

/* (394-473) Potentially backup over partial log record write. */
static int
xlog_find_verify_log_record(struct xlog *log, xfs_daddr_t start_blk,
			    xfs_daddr_t *last_blk, int extra_bblks)
{
	xfs_daddr_t		i;
	char			*buffer;
	char			*offset = NULL;
	struct xlog_rec_header	*head = NULL;
	int			error = 0;
	int			smallmem = 0;
	int			num_blks = *last_blk - start_blk;
	int			xhdrs;

	/* kernel: ASSERT(start_blk != 0 || *last_blk != start_blk); without
	 * debug it falls through to "no header found" (1). Keep that. */
	if (start_blk == 0 && *last_blk == start_blk)
		return 1;
	if (num_blks <= 0)
		return -EFSCORRUPTED;

	buffer = xlog_alloc_buffer(log, num_blks);
	if (!buffer) {
		buffer = xlog_alloc_buffer(log, 1);
		if (!buffer)
			return -ENOMEM;
		smallmem = 1;
	} else {
		error = xlog_bread(log, start_blk, num_blks, buffer, &offset);
		if (error)
			goto out;
		offset += ((num_blks - 1) << BBSHIFT);
	}

	for (i = (*last_blk) - 1; i >= 0; i--) {
		if (i < start_blk) {
			/* valid log record not found */
			log_warn(log,
		"Log inconsistent (didn't find previous header)");
			error = -EFSCORRUPTED;
			goto out;
		}

		if (smallmem) {
			error = xlog_bread(log, i, 1, buffer, &offset);
			if (error)
				goto out;
		}

		head = (struct xlog_rec_header *)offset;

		if (head->h_magicno == cpu_to_be32(XLOG_HEADER_MAGIC_NUM))
			break;

		if (!smallmem)
			offset -= BBSIZE;
	}

	/*
	 * We hit the beginning of the physical log & still no header.  Return
	 * to caller.  If caller can handle a return of -1, then this routine
	 * will be called again for the end of the physical log.
	 */
	if (i == -1) {
		error = 1;
		goto out;
	}

	if ((error = xlog_header_check_mount(log, head)))
		goto out;

	xhdrs = xlog_logrec_hblks(log, head);

	if (*last_blk - i + extra_bblks !=
	    (xfs_daddr_t)BTOBB(be32_to_cpu(head->h_len)) + xhdrs)
		*last_blk = i;

out:
	free(buffer);
	return error;
}

/*
 * Head is defined to be the point of the log where the next log write
 * could go. (498-732; see the kernel comments there for the case analysis)
 */
static int
xlog_find_head(struct xlog *log, xfs_daddr_t *return_head_blk);

/* (1389-1470) Is the log zeroed at all? 0 = fully written, 1 = use *blk_no */
static int
xlog_find_zeroed(struct xlog *log, xfs_daddr_t *blk_no)
{
	char		*buffer;
	char		*offset;
	uint		first_cycle, last_cycle;
	xfs_daddr_t	new_blk, last_blk, start_blk;
	xfs_daddr_t	num_scan_bblks;
	int		error, log_bbnum = log->l_logBBsize;
	int		ret = 1;

	*blk_no = 0;

	/* check totally zeroed log */
	buffer = xlog_alloc_buffer(log, 1);
	if (!buffer)
		return -ENOMEM;
	error = xlog_bread(log, 0, 1, buffer, &offset);
	if (error)
		goto out_free_buffer;

	first_cycle = xlog_get_cycle(offset);
	if (first_cycle == 0) {		/* completely zeroed log */
		*blk_no = 0;
		goto out_free_buffer;
	}

	/* check partially zeroed log */
	error = xlog_bread(log, log_bbnum-1, 1, buffer, &offset);
	if (error)
		goto out_free_buffer;

	last_cycle = xlog_get_cycle(offset);
	if (last_cycle != 0) {		/* log completely written to */
		ret = 0;
		goto out_free_buffer;
	}

	/* we have a partially zeroed log */
	last_blk = log_bbnum-1;
	error = xlog_find_cycle_start(log, buffer, 0, &last_blk, 0);
	if (error)
		goto out_free_buffer;

	num_scan_bblks = XLOG_TOTAL_REC_SHIFT_BB(log->l_logv2);
	if (num_scan_bblks > log_bbnum)
		num_scan_bblks = log_bbnum;

	if (last_blk < num_scan_bblks)
		num_scan_bblks = last_blk;
	start_blk = last_blk - num_scan_bblks;

	if (num_scan_bblks > 0) {
		if ((error = xlog_find_verify_cycle(log, start_blk,
					 (int)num_scan_bblks, 0, &new_blk)))
			goto out_free_buffer;
		if (new_blk != -1)
			last_blk = new_blk;
	}

	error = xlog_find_verify_log_record(log, start_blk, &last_blk, 0);
	if (error == 1)
		error = -EFSCORRUPTED;	/* no previous record header: corrupt, not I/O */
	if (error)
		goto out_free_buffer;

	*blk_no = last_blk;
out_free_buffer:
	free(buffer);
	if (error)
		return error;
	return ret;
}

static int
xlog_find_head(struct xlog *log, xfs_daddr_t *return_head_blk)
{
	char		*buffer;
	char		*offset;
	xfs_daddr_t	new_blk, first_blk, start_blk, last_blk, head_blk;
	int		num_scan_bblks;
	uint		first_half_cycle, last_half_cycle;
	uint		stop_on_cycle;
	int		error, log_bbnum = log->l_logBBsize;

	/* Is the end of the log device zeroed? */
	error = xlog_find_zeroed(log, &first_blk);
	if (error < 0) {
		log_warn(log, "empty log check failed");
		return error;
	}
	if (error == 1) {
		*return_head_blk = first_blk;
		if (!first_blk)
			log_warn(log, "totally zeroed log");
		return 0;
	}

	first_blk = 0;			/* get cycle # of 1st block */
	buffer = xlog_alloc_buffer(log, 1);
	if (!buffer)
		return -ENOMEM;

	error = xlog_bread(log, 0, 1, buffer, &offset);
	if (error)
		goto out_free_buffer;

	first_half_cycle = xlog_get_cycle(offset);

	last_blk = head_blk = log_bbnum - 1;	/* get cycle # of last block */
	error = xlog_bread(log, last_blk, 1, buffer, &offset);
	if (error)
		goto out_free_buffer;

	last_half_cycle = xlog_get_cycle(offset);
	if (last_half_cycle == 0) {
		/* xlog_find_zeroed said "fully written": cannot happen */
		error = -EFSCORRUPTED;
		goto out_free_buffer;
	}

	if (first_half_cycle == last_half_cycle) {
		/* entire log stamped with one cycle (see kernel comment) */
		head_blk = log_bbnum;
		stop_on_cycle = last_half_cycle - 1;
	} else {
		stop_on_cycle = last_half_cycle;
		error = xlog_find_cycle_start(log, buffer, first_blk, &head_blk,
				last_half_cycle);
		if (error)
			goto out_free_buffer;
	}

	num_scan_bblks = min_t(int, log_bbnum,
			       XLOG_TOTAL_REC_SHIFT_BB(log->l_logv2));
	if (head_blk >= num_scan_bblks) {
		start_blk = head_blk - num_scan_bblks;
		if ((error = xlog_find_verify_cycle(log,
						start_blk, num_scan_bblks,
						stop_on_cycle, &new_blk)))
			goto out_free_buffer;
		if (new_blk != -1)
			head_blk = new_blk;
	} else {		/* need to read 2 parts of log */
		start_blk = log_bbnum - (num_scan_bblks - head_blk);
		if ((error = xlog_find_verify_cycle(log, start_blk,
					num_scan_bblks - (int)head_blk,
					(stop_on_cycle - 1), &new_blk)))
			goto out_free_buffer;
		if (new_blk != -1) {
			head_blk = new_blk;
			goto validate_head;
		}

		start_blk = 0;
		if ((error = xlog_find_verify_cycle(log,
					start_blk, (int)head_blk,
					stop_on_cycle, &new_blk)))
			goto out_free_buffer;
		if (new_blk != -1)
			head_blk = new_blk;
	}

validate_head:
	/* make sure head_blk is not pointing into the middle of a record */
	num_scan_bblks = XLOG_REC_SHIFT_BB(log->l_logv2);
	if (num_scan_bblks > log_bbnum)
		num_scan_bblks = log_bbnum;
	if (head_blk >= num_scan_bblks) {
		start_blk = head_blk - num_scan_bblks; /* don't read head_blk */

		error = xlog_find_verify_log_record(log, start_blk, &head_blk, 0);
		if (error == 1)
			error = -EFSCORRUPTED;
		if (error)
			goto out_free_buffer;
	} else {
		start_blk = 0;
		error = xlog_find_verify_log_record(log, start_blk, &head_blk, 0);
		if (error < 0)
			goto out_free_buffer;
		if (error == 1) {
			/* We hit the beginning of the log during our search */
			start_blk = log_bbnum - (num_scan_bblks - head_blk);
			new_blk = log_bbnum;
			if (start_blk < 0 || start_blk >= log_bbnum) {
				error = -EFSCORRUPTED;
				goto out_free_buffer;
			}
			error = xlog_find_verify_log_record(log, start_blk,
							&new_blk, (int)head_blk);
			if (error == 1)
				error = -EFSCORRUPTED;
			if (error)
				goto out_free_buffer;
			if (new_blk != log_bbnum)
				head_blk = new_blk;
		} else if (error)
			goto out_free_buffer;
	}

	free(buffer);
	if (head_blk == log_bbnum)
		*return_head_blk = 0;
	else
		*return_head_blk = head_blk;
	return 0;

out_free_buffer:
	free(buffer);
	if (error)
		log_warn(log, "failed to find log head");
	return error;
}

/*
 * winuxfs: the kernel reads one basic block per call in the two seek helpers
 * below. On an untrusted image the walk may cover the whole log (up to 2 GiB,
 * ~4M reads), so the scan reads WFS_SCAN_CHUNK_BB blocks per reader call:
 * the worst case is log_bbnum / WFS_SCAN_CHUNK_BB (16K) reader calls.
 */
#define WFS_SCAN_CHUNK_BB	256

/*
 * Scan blocks lo..hi (inclusive) for record headers, backward or forward,
 * stopping when *found reaches count. Records the block and a copy of the
 * header block of the last header seen.
 */
static int
xlog_scan_hdrs(struct xlog *log, xfs_daddr_t lo, xfs_daddr_t hi, bool backward,
	       int count, int *found, xfs_daddr_t *rblk, union wfs_rhead *rhead,
	       bool is_wrapped, bool *wrapped)
{
	char		*buf, *offset;
	xfs_daddr_t	pos;
	int		error;

	if (lo > hi)
		return 0;
	buf = xlog_alloc_buffer(log, WFS_SCAN_CHUNK_BB);
	if (!buf)
		return -ENOMEM;
	pos = backward ? hi : lo;
	while (backward ? pos >= lo : pos <= hi) {
		xfs_daddr_t	c0, c1;
		int		n, k;

		if (backward) {
			c1 = pos;
			c0 = max(lo, c1 - WFS_SCAN_CHUNK_BB + 1);
		} else {
			c0 = pos;
			c1 = min(hi, c0 + WFS_SCAN_CHUNK_BB - 1);
		}
		n = (int)(c1 - c0 + 1);
		error = xlog_bread(log, c0, n, buf, &offset);
		if (error) {
			free(buf);
			return error;
		}
		for (k = 0; k < n; k++) {
			int	idx = backward ? n - 1 - k : k;
			char	*bp = offset + (size_t)idx * BBSIZE;

			if (*(__be32 *)bp != cpu_to_be32(XLOG_HEADER_MAGIC_NUM))
				continue;
			*rblk = c0 + idx;
			if (rhead)
				memcpy(rhead->raw, bp, BBSIZE);
			if (is_wrapped)
				*wrapped = true;
			if (++(*found) == count) {
				free(buf);
				return 0;
			}
		}
		pos = backward ? c0 - 1 : c1 + 1;
	}
	free(buf);
	return 0;
}

/*
 * Seek backwards in the log for log record headers. (763-823)
 * Returns the number found or a negative error; *rhead is a copy.
 */
static int
xlog_rseek_logrec_hdr(struct xlog *log, xfs_daddr_t head_blk,
		      xfs_daddr_t tail_blk, int count, char *buffer,
		      xfs_daddr_t *rblk, union wfs_rhead *rhead, bool *wrapped)
{
	int			found = 0;
	int			error;
	xfs_daddr_t		end_blk;

	(void)buffer;
	*wrapped = false;

	end_blk = head_blk > tail_blk ? tail_blk : 0;
	error = xlog_scan_hdrs(log, end_blk, head_blk - 1, true, count, &found,
			       rblk, rhead, false, wrapped);
	if (error)
		return error;

	if (tail_blk >= head_blk && found != count) {
		error = xlog_scan_hdrs(log, tail_blk, log->l_logBBsize - 1,
				       true, count, &found, rblk, rhead, true,
				       wrapped);
		if (error)
			return error;
	}
	return found;
}

/* Seek forward in the log for log record headers. (835-896) */
static int
xlog_seek_logrec_hdr(struct xlog *log, xfs_daddr_t head_blk,
		     xfs_daddr_t tail_blk, int count, char *buffer,
		     xfs_daddr_t *rblk, bool *wrapped)
{
	int			found = 0;
	int			error;
	xfs_daddr_t		end_blk;

	(void)buffer;
	*wrapped = false;

	end_blk = head_blk > tail_blk ? head_blk : log->l_logBBsize - 1;
	error = xlog_scan_hdrs(log, tail_blk, end_blk, false, count, &found,
			       rblk, NULL, false, wrapped);
	if (error)
		return error;

	if (tail_blk > head_blk && found != count) {
		error = xlog_scan_hdrs(log, 0, head_blk - 1, false, count,
				       &found, rblk, NULL, true, wrapped);
		if (error)
			return error;
	}
	return found;
}

/* (901-910) Calculate distance from head to tail (unused space in the log). */
static inline int
xlog_tail_distance(struct xlog *log, xfs_daddr_t head_blk,
		   xfs_daddr_t tail_blk)
{
	if (head_blk < tail_blk)
		return tail_blk - head_blk;

	return tail_blk + (log->l_logBBsize - head_blk);
}

/* (xfs_log.c 1509-1535) */
static uint32_t
xlog_cksum(struct xlog *log, struct xlog_rec_header *rhead, char *dp,
	   unsigned int hdrsize, unsigned int size)
{
	uint32_t		crc;

	/* first generate the crc for the record header ... */
	crc = xfs_start_cksum_update((char *)rhead, hdrsize,
			      offsetof(struct xlog_rec_header, h_crc));

	/* ... then for additional cycle data for v2 logs ... */
	if (log->l_logv2) {
		int		xheads, i;

		xheads = DIV_ROUND_UP(size, XLOG_HEADER_CYCLE_SIZE) - 1;
		for (i = 0; i < xheads; i++)
			crc = crc32c(crc, &rhead->h_ext[i], XLOG_REC_EXT_SIZE);
	}

	/* ... and finally for the payload */
	crc = crc32c(crc, dp, size);

	return (__force uint32_t)xfs_end_cksum(crc);
}

/* (2964-3000) with an upper bound on h_len/h_size */
static int
xlog_valid_rec_header(struct xlog *log, struct xlog_rec_header *rhead,
		      xfs_daddr_t blkno, int bufsize)
{
	u32			h_version = be32_to_cpu(rhead->h_version);
	int			hlen;

	if (rhead->h_magicno != cpu_to_be32(XLOG_HEADER_MAGIC_NUM))
		return -EFSCORRUPTED;

	/* The log version must match the superblock */
	if (log->l_logv2) {
		if (h_version != XLOG_VERSION_2)
			return -EFSCORRUPTED;
	} else {
		if (h_version != XLOG_VERSION_1)
			return -EFSCORRUPTED;
	}

	hlen = be32_to_cpu(rhead->h_len);
	if (hlen <= 0 || hlen > bufsize)
		return -EFSCORRUPTED;

	if (blkno > log->l_logBBsize || blkno > INT_MAX)
		return -EFSCORRUPTED;

	return 0;
}

/*
 * CRC verification pass over the records from tail to head
 * (xlog_do_recovery_pass with pass == XLOG_RECOVER_CRCPASS; 3012-3320, and
 * the CRC half of xlog_recover_process 2891-2929). Returns 0, -EFSBADCRC,
 * -EFSCORRUPTED or an I/O error. *first_bad is the start block of the
 * record that failed.
 */
static int
xlog_crc_pass(struct xlog *log, xfs_daddr_t head_blk, xfs_daddr_t tail_blk,
	      xfs_daddr_t *first_bad)
{
	xfs_daddr_t	total, consumed = 0;
	char		*hbp = NULL, *dbp = NULL;
	int		hblks = 1, h_size, error = 0;
	union wfs_rhead	first;

	if (head_blk == tail_blk)
		return 0;
	*first_bad = tail_blk;	/* kernel: rhead_blk starts at tail_blk */
	if (head_blk < 0 || head_blk >= log->l_logBBsize ||
	    tail_blk < 0 || tail_blk >= log->l_logBBsize)
		return -EFSCORRUPTED;

	total = head_blk > tail_blk ? head_blk - tail_blk :
		log->l_logBBsize - tail_blk + head_blk;

	if (log->l_logv2) {
		char *offset;
		int h_len;

		hbp = xlog_alloc_buffer(log, 1);
		if (!hbp)
			return -ENOMEM;
		error = xlog_bread(log, tail_blk, 1, hbp, &offset);
		if (error)
			goto out;
		memcpy(first.raw, offset, BBSIZE);
		h_size = be32_to_cpu(first.h.h_size);
		h_len = be32_to_cpu(first.h.h_len);
		/* mkfs wrote lsunit-sized records with h_size hardcoded to 32k */
		if (h_len > h_size && log->l_p->logbsize &&
		    h_len <= (int)log->l_p->logbsize &&
		    first.h.h_num_logops == cpu_to_be32(1))
			h_size = log->l_p->logbsize;
		if (h_size <= 0 || h_size > XLOG_MAX_RECORD_BSIZE) {
			error = -EFSCORRUPTED;
			goto out;
		}
		error = xlog_valid_rec_header(log, &first.h, tail_blk, h_size);
		if (error)
			goto out;
		if (h_size > XLOG_HEADER_CYCLE_SIZE &&
		    (first.h.h_version & cpu_to_be32(XLOG_VERSION_2)))
			hblks = DIV_ROUND_UP(h_size, XLOG_HEADER_CYCLE_SIZE);
		free(hbp);
		hbp = NULL;
	} else {
		h_size = XLOG_BIG_RECORD_BSIZE;
	}
	if (hblks >= log->l_logBBsize || BTOBB(h_size) >= (uint64_t)log->l_logBBsize) {
		error = -EFSCORRUPTED;
		goto out;
	}

	hbp = calloc(1, BBTOB((size_t)hblks));
	dbp = calloc(1, BBTOB((size_t)BTOBB(h_size)));
	if (!hbp || !dbp) {
		error = -ENOMEM;
		goto out;
	}

	/* every iteration advances consumed by >= hblks + 1 */
	while (consumed < total) {
		xfs_daddr_t	blk = (tail_blk + consumed) % log->l_logBBsize;
		struct xlog_rec_header *rhead = (struct xlog_rec_header *)hbp;
		__le32		expected_crc, crc, other_crc;
		int		bblks;

		*first_bad = blk;
		error = xlog_bread_wrapped(log, blk, hblks, hbp);
		if (error)
			goto out;
		error = xlog_valid_rec_header(log, rhead, blk, h_size);
		if (error)
			goto out;
		bblks = (int)BTOBB(be32_to_cpu(rhead->h_len));
		if (hblks + bblks > log->l_logBBsize) {
			error = -EFSCORRUPTED;
			goto out;
		}
		error = xlog_bread_wrapped(log, blk + hblks, bblks, dbp);
		if (error)
			goto out;

		expected_crc = rhead->h_crc;
		crc = cpu_to_le32(xlog_cksum(log, rhead, dbp, XLOG_REC_SIZE,
					     be32_to_cpu(rhead->h_len)));
		/* see the end of struct xlog_rec_header in xfs_log_format.h */
		if (expected_crc && crc != expected_crc) {
			other_crc = cpu_to_le32(xlog_cksum(log, rhead, dbp,
					XLOG_REC_SIZE_OTHER,
					be32_to_cpu(rhead->h_len)));
			if (other_crc == expected_crc)
				crc = other_crc;
		}
		/* mkfs always writes a zero crc: valid even on v5 */
		if (expected_crc && crc != expected_crc) {
			error = -EFSBADCRC;
			goto out;
		}
		consumed += hblks + bblks;
	}
	error = 0;
out:
	free(hbp);
	free(dbp);
	return error;
}

/* (928-994) Verify the log tail. */
static int
xlog_verify_tail(struct xlog *log, xfs_daddr_t head_blk,
		 xfs_daddr_t *tail_blk, int hsize)
{
	char			*buffer;
	xfs_daddr_t		first_bad;
	int			error = 0;
	bool			wrapped;
	xfs_daddr_t		tmp_tail = *tail_blk;
	xfs_daddr_t		orig_tail = *tail_blk;
	int			retries = 0;

	buffer = xlog_alloc_buffer(log, 1);
	if (!buffer)
		return -ENOMEM;

	/* Make sure the tail points to a record. */
	error = xlog_seek_logrec_hdr(log, head_blk, *tail_blk, 1, buffer,
			&tmp_tail, &wrapped);
	if (error < 0)
		goto out;
	if (*tail_blk != tmp_tail)
		*tail_blk = tmp_tail;

	first_bad = 0;
	error = xlog_crc_pass(log, head_blk, *tail_blk, &first_bad);
	while ((error == -EFSBADCRC || error == -EFSCORRUPTED) && first_bad) {
		int	tail_distance;

		tail_distance = xlog_tail_distance(log, head_blk, first_bad);
		if (tail_distance > BTOBB(XLOG_MAX_ICLOGS * hsize))
			break;
		/* winuxfs: the kernel loop is not capped; corrupt data must not spin */
		if (++retries > WFS_LOG_VERIFY_TAIL_RETRIES)
			break;

		tmp_tail = first_bad;
		error = xlog_seek_logrec_hdr(log, head_blk, first_bad, 2,
				buffer, &tmp_tail, &wrapped);
		if (error < 0)
			goto out;
		if (error < 2) {
			/* no next record: cannot move the tail forward */
			error = -EFSCORRUPTED;
			break;
		}

		*tail_blk = tmp_tail;
		first_bad = 0;
		error = xlog_crc_pass(log, head_blk, *tail_blk, &first_bad);
	}

	if (!error && *tail_blk != orig_tail)
		log_warn(log,
		"Tail block (0x%llx) overwrite detected. Updated to 0x%llx",
			 (unsigned long long)orig_tail,
			 (unsigned long long)*tail_blk);
out:
	free(buffer);
	return error;
}

/* (1020-1111) Detect and trim torn writes from the head of the log. */
static int
xlog_verify_head(struct xlog *log, xfs_daddr_t *head_blk,
		 xfs_daddr_t *tail_blk, char *buffer, xfs_daddr_t *rhead_blk,
		 union wfs_rhead *rhead, bool *wrapped)
{
	union wfs_rhead		tmp_rhead;
	char			*tmp_buffer;
	xfs_daddr_t		first_bad = -1;
	xfs_daddr_t		tmp_rhead_blk;
	int			found;
	int			error;
	bool			tmp_wrapped;

	tmp_buffer = xlog_alloc_buffer(log, 1);
	if (!tmp_buffer)
		return -ENOMEM;
	error = xlog_rseek_logrec_hdr(log, *head_blk, *tail_blk,
				      XLOG_MAX_ICLOGS, tmp_buffer,
				      &tmp_rhead_blk, &tmp_rhead, &tmp_wrapped);
	free(tmp_buffer);
	if (error < 0)
		return error;
	if (error == 0)
		return -EFSCORRUPTED;	/* winuxfs: tmp_rhead_blk is unset */

	error = xlog_crc_pass(log, *head_blk, tmp_rhead_blk, &first_bad);
	if ((error == -EFSBADCRC || error == -EFSCORRUPTED) &&
	    first_bad != -1) {
		/* potential torn write: reset the error and warn about it */
		error = 0;
		log_warn(log,
"Torn write (CRC failure) detected at log block 0x%llx. Truncating head block from 0x%llx.",
			 (unsigned long long)first_bad,
			 (unsigned long long)*head_blk);

		found = xlog_rseek_logrec_hdr(log, first_bad, *tail_blk, 1,
				buffer, rhead_blk, rhead, wrapped);
		if (found < 0)
			return found;
		if (found == 0)
			return -EFSCORRUPTED;

		*head_blk = first_bad;
		*tail_blk = BLOCK_LSN(be64_to_cpu(rhead->h.h_tail_lsn));
		if (*head_blk == *tail_blk)
			return 0;
	}
	if (error)
		return error;

	if (*tail_blk < 0 || *tail_blk >= log->l_logBBsize)
		return -EFSCORRUPTED;
	{
		int hsize = be32_to_cpu(rhead->h.h_size);

		if (hsize < 0 || hsize > XLOG_MAX_RECORD_BSIZE)
			return -EFSCORRUPTED;
		return xlog_verify_tail(log, *head_blk, tail_blk, hsize);
	}
}

/* (1118-1127) */
static inline xfs_daddr_t
xlog_wrap_logbno(struct xlog *log, xfs_daddr_t bno)
{
	return bno % log->l_logBBsize;
}

/* (1134-1180) Does the head of the log point to an unmount record? */
static int
xlog_check_unmount_rec(struct xlog *log, xfs_daddr_t *head_blk,
		       xfs_daddr_t *tail_blk, union wfs_rhead *rhead,
		       xfs_daddr_t rhead_blk, char *buffer, bool *clean)
{
	struct xlog_op_header	*op_head;
	xfs_daddr_t		umount_data_blk;
	xfs_daddr_t		after_umount_blk;
	int			hblks;
	int			error;
	char			*offset;

	*clean = false;

	hblks = xlog_logrec_hblks(log, &rhead->h);
	after_umount_blk = xlog_wrap_logbno(log,
			rhead_blk + hblks +
			(xfs_daddr_t)BTOBB(be32_to_cpu(rhead->h.h_len)));

	if (*head_blk == after_umount_blk &&
	    be32_to_cpu(rhead->h.h_num_logops) == 1) {
		umount_data_blk = xlog_wrap_logbno(log, rhead_blk + hblks);
		error = xlog_bread(log, umount_data_blk, 1, buffer, &offset);
		if (error)
			return error;

		op_head = (struct xlog_op_header *)offset;
		if (op_head->oh_flags & XLOG_UNMOUNT_TRANS) {
			*tail_blk = after_umount_blk;
			*clean = true;
		}
	}

	return 0;
}

/*
 * Find the head and tail of the log (1236-1380, no in-core state, no stale
 * block clearing). The log needs recovery iff *head_blk != *tail_blk, as in
 * the kernel's xlog_recover().
 */
static int
xlog_find_tail(struct xlog *log, xfs_daddr_t *head_blk, xfs_daddr_t *tail_blk)
{
	union wfs_rhead		rhead;
	char			*offset = NULL;
	char			*buffer;
	int			error;
	xfs_daddr_t		rhead_blk;
	bool			wrapped = false;
	bool			clean = false;

	if ((error = xlog_find_head(log, head_blk)))
		return error;
	if (*head_blk < 0 || *head_blk >= log->l_logBBsize)
		return -EFSCORRUPTED;

	buffer = xlog_alloc_buffer(log, 1);
	if (!buffer)
		return -ENOMEM;
	if (*head_blk == 0) {				/* special case */
		error = xlog_bread(log, 0, 1, buffer, &offset);
		if (error)
			goto done;

		if (xlog_get_cycle(offset) == 0) {
			*tail_blk = 0;
			goto done;
		}
	}

	/* Search backwards (wrapping to the head) for a record header. */
	error = xlog_rseek_logrec_hdr(log, *head_blk, *head_blk, 1, buffer,
				      &rhead_blk, &rhead, &wrapped);
	if (error < 0)
		goto done;
	if (!error) {
		log_warn(log, "%s: couldn't find sync record", __func__);
		error = -EFSCORRUPTED;
		goto done;
	}
	error = 0;
	*tail_blk = BLOCK_LSN(be64_to_cpu(rhead.h.h_tail_lsn));
	if (*tail_blk < 0 || *tail_blk >= log->l_logBBsize) {
		error = -EFSCORRUPTED;
		goto done;
	}

	error = xlog_check_unmount_rec(log, head_blk, tail_blk, &rhead,
				       rhead_blk, buffer, &clean);
	if (error)
		goto done;

	/* Verify the log head if the log is not clean (torn write check). */
	if (!clean) {
		xfs_daddr_t	orig_head = *head_blk;

		error = xlog_verify_head(log, head_blk, tail_blk, buffer,
					 &rhead_blk, &rhead, &wrapped);
		if (error)
			goto done;

		if (*head_blk != orig_head) {
			error = xlog_check_unmount_rec(log, head_blk, tail_blk,
						       &rhead, rhead_blk,
						       buffer, &clean);
			if (error)
				goto done;
		}
	}
	if (*tail_blk < 0 || *tail_blk >= log->l_logBBsize)
		error = -EFSCORRUPTED;
done:
	free(buffer);
	if (error)
		log_warn(log, "failed to locate log tail");
	return error;
}

/* ------------------------------------------------------------------ */

int
wfs_log_check(const struct wfs_log_params *p, const struct wfs_log_reader *r,
	      bool norecovery, bool *dirty_out)
{
	struct xlog	log;
	xfs_daddr_t	head_blk = 0, tail_blk = 0;
	int		error;

	if (dirty_out)
		*dirty_out = false;
	if (!p || !r || !r->read)
		return -EINVAL;

	/* the sector size and log size come from the (untrusted) superblock */
	if (p->sector_size < BBSIZE || p->sector_size > 32768 ||
	    (p->sector_size & (p->sector_size - 1)) ||
	    p->log_bytes < (uint64_t)WFS_LOG_MIN_BB * BBSIZE ||
	    p->log_bytes > ((uint64_t)INT_MAX << BBSHIFT) ||
	    (p->log_bytes % p->sector_size) ||
	    (p->log_start_byte % p->sector_size) ||
	    p->log_start_byte + p->log_bytes < p->log_start_byte) {
		xfs_warn(NULL, "log parameters out of range (size %llu, sector %u)",
			 (unsigned long long)p->log_bytes, p->sector_size);
		return -EFSCORRUPTED;
	}

	memset(&log, 0, sizeof(log));
	log.l_p = p;
	log.l_r = r;
	log.l_logBBsize = BTOBB(p->log_bytes);
	log.l_sectBBsize = p->sector_size >> BBSHIFT;
	log.l_logv2 = p->logv2;
	log.l_msgs = true;

	error = xlog_find_tail(&log, &head_blk, &tail_blk);
	if (error == -EFSBADCRC)	/* backend boundary: BADCRC -> EUCLEAN */
		error = -EFSCORRUPTED;
	if (error == -EIO) {
		/* the reader failed: the device is unusable, even for norecovery */
		xfs_err(NULL, "log: I/O error while reading the log");
		return error;
	}
	if (error) {
		/*
		 * The log cannot be interpreted (corrupt). The kernel never
		 * parses the log under norecovery, so there it only warns
		 * (policy in design 6.2); otherwise the mount is refused.
		 */
		if (norecovery) {
			if (dirty_out)
				*dirty_out = true;	/* unknown: treat as stale */
			xfs_warn(NULL,
	"log cannot be interpreted (error %d): mounting with norecovery, "
	"data may be stale or inconsistent", error);
			return 0;
		}
		xfs_err(NULL, "log: cannot be interpreted (corrupt?), error %d; "
			"mount on Linux to replay or use the option norecovery",
			error);
		return error;
	}

	if (head_blk == tail_blk)
		return 0;		/* clean */

	if (dirty_out)
		*dirty_out = true;
	if (norecovery) {
		xfs_warn(NULL,
	"log is dirty (head 0x%llx tail 0x%llx): mounting with norecovery, "
	"metadata may be stale or inconsistent",
			 (unsigned long long)head_blk, (unsigned long long)tail_blk);
		return 0;
	}
	xfs_err(NULL,
	"log is dirty (head 0x%llx tail 0x%llx): mount the filesystem on Linux "
	"once to replay the log, or use the mount option norecovery to read "
	"it without replay (data may be stale)",
		(unsigned long long)head_blk, (unsigned long long)tail_blk);
	return -EFSCORRUPTED;
}

static wfs_log_reader_provider g_provider;

void
wfs_log_set_reader_provider(wfs_log_reader_provider fn)
{
	g_provider = fn;
}

int
xfs_log_mount_ro_check(struct xfs_mount *mp, bool norecovery, bool *dirty_out)
{
	struct wfs_log_params	p;
	struct wfs_log_reader	r;
	uint32_t		sect;
	int			error;

	if (!g_provider)
		return -EINVAL;
	memset(&p, 0, sizeof(p));
	memset(&r, 0, sizeof(r));

	/* kernel: the log sector size applies only with the SECTORBIT feature */
	sect = (mp->m_features & XFS_FEAT_SECTOR) && mp->m_sb.sb_logsectsize ?
	       mp->m_sb.sb_logsectsize : BBSIZE;
	if (mp->m_sb.sb_logstart)
		p.log_start_byte = BBTOB((uint64_t)XFS_FSB_TO_DADDR(mp, mp->m_sb.sb_logstart));
	p.log_bytes = (uint64_t)mp->m_sb.sb_logblocks << mp->m_sb.sb_blocklog;
	p.sector_size = sect;
	p.logv2 = xfs_has_logv2(mp);
	p.logbsize = mp->m_sb.sb_logsunit > 1 ? mp->m_sb.sb_logsunit : 0;
	memcpy(p.uuid, &mp->m_sb.sb_uuid, sizeof(p.uuid));

	error = g_provider(mp, &r);
	if (error)
		return error;
	return wfs_log_check(&p, &r, norecovery, dirty_out);
}
