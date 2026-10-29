/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Dirty-log detection for the read-only XFS backend (M2 design 6).
 *
 * logcheck.c is an excerpt-port of the head/tail search of the kernel's
 * fs/xfs/xfs_log_recover.c (xlog_find_head, xlog_find_tail and helpers) plus
 * the CRC verification pass. It never replays and never writes.
 */
#ifndef WFS_XFS_LOGCHECK_H
#define WFS_XFS_LOGCHECK_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct xfs_mount;

/*
 * Minimal read interface. `read` fills buf with len bytes at byte offset
 * byte_off of the device that holds the log (the data device for an internal
 * log, the log device for an external one) and returns 0, or non-zero on
 * any error or short read (mapped to -EIO).
 */
struct wfs_log_reader {
	int	(*read)(void *ctx, uint64_t byte_off, void *buf, size_t len);
	void	*ctx;
};

/* Superblock-derived log parameters. */
struct wfs_log_params {
	uint64_t	log_start_byte;	/* first byte of the log on the device */
	uint64_t	log_bytes;	/* log size in bytes */
	uint32_t	sector_size;	/* log sector size: power of 2, 512..32768 */
	bool		logv2;		/* XFS_SB_VERSION2_LOGV2 (always set on v5) */
	uint32_t	logbsize;	/* log stripe unit / buffer size in bytes, or 0 */
	uint8_t		uuid[16];	/* sb_uuid (log records must carry it) */
};

/*
 * Core check, independent of libxfs mount state.
 * Returns 0: clean, or dirty with norecovery (warning logged);
 *  -EUCLEAN: dirty or unparsable log without norecovery, or bad parameters;
 *            with norecovery an unparsable log is a warning and returns 0
 *            (*dirty_out is set: contents may be stale);
 *  -EIO: the reader failed.
 * *dirty_out (optional) is set to true when the log needs recovery and is
 * only meaningful when the return value is 0 or the log was parsed as dirty.
 */
int wfs_log_check(const struct wfs_log_params *p,
		  const struct wfs_log_reader *r, bool norecovery,
		  bool *dirty_out);

/*
 * Task 6 provides this: fill *r for the device holding mp's log. Returning
 * non-zero fails the check with that error.
 */
typedef int (*wfs_log_reader_provider)(struct xfs_mount *mp,
				       struct wfs_log_reader *r);
void wfs_log_set_reader_provider(wfs_log_reader_provider fn);

/*
 * Mount-time entry point (M2 design 6.2): derives the parameters from
 * mp->m_sb, gets the reader from the provider and runs wfs_log_check().
 */
int xfs_log_mount_ro_check(struct xfs_mount *mp, bool norecovery, bool *dirty_out);

#endif
