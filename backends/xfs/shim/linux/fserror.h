/* SPDX-License-Identifier: GPL-2.0-only */
/* linux/fserror.h: filesystem error reporting to userspace (fanotify). The
 * host learns about errors from return codes and the log: reports are no-ops. */
#ifndef WFS_LINUX_FSERROR_H
#define WFS_LINUX_FSERROR_H
struct fserror_event;
struct super_block;
#define fserror_report_shutdown(sb, gfp)	((void)(sb))
#define fserror_report_metadata(sb, err, gfp)	((void)(sb))
#define fserror_report_file_metadata(inode, err, gfp) ((void)(inode))
#define fserror_report_io(...)			((void)0)
#endif
