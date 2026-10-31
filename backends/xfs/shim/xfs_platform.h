/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Replacement for fs/xfs/xfs_platform.h: libxfs sources do
 * `#include "xfs_platform.h"` first and get the whole kernel environment from
 * it. This is the winuxfs version (Linux-free, native + mingw LLP64).
 *
 * Include order for the compile: -I backends/xfs/shim, then libxfs/, then the
 * vendored fs/xfs root.
 */
#ifndef _XFS_PLATFORM_H
#define _XFS_PLATFORM_H

#include "wfs_shim.h"
#include <linux/rbtree.h>	/* third_party/linux-lib, patched for LLP64 */
#include "wfs_bio.h"

/* Config: read-only port, RT devices readable, no quota/scrub/debug. */
#define CONFIG_XFS_RT 1
#define CONFIG_XFS_SUPPORT_V4 0
#define CONFIG_XFS_SUPPORT_ASCII_CI 0
#define XFS_WARN 1

#define PAGE_SHIFT		12
#define PAGE_SIZE		(1UL << PAGE_SHIFT)
#define PAGE_MASK		(~(PAGE_SIZE - 1))

/* Kernel specific type declarations for XFS. */
typedef __s64			xfs_off_t;	/* <file offset> type */
typedef unsigned long long	xfs_ino_t;	/* <inode> type */
typedef __s64			xfs_daddr_t;	/* <disk address> type */
typedef __u32			xfs_dev_t;
typedef __u32			xfs_nlink_t;
typedef unsigned long long	sector_t;
typedef __u32			dev_t_;

#include "xfs_stats.h"
#include "xfs_drain.h"
#include "xfs_hooks.h"
#include "xfs_types.h"
#include "xfs_fs.h"
#include "wfs_cksum.h"
#include "xfs_buf.h"
#include "xfs_message.h"

#define xfs_panic_mask		0
#define xfs_error_level		0
/* sysctl tunables are constants on a read-only mount */
#define xfs_inherit_sync	1
#define xfs_inherit_nodump	1
#define xfs_inherit_noatime	1
#define xfs_inherit_nosymlinks	1
#define xfs_inherit_nodefrag	1
#define xfs_rotorstep		1

#define NBBY_			8
#define BLKDEV_IOSHIFT		PAGE_SHIFT
#define BLKDEV_IOSIZE		(1 << BLKDEV_IOSHIFT)
#define BLKDEV_BB		BTOBB(BLKDEV_IOSIZE)

#define __return_address	__builtin_return_address(0)

/*
 * Return the address of a label; barrier() keeps the optimizer from merging
 * the error jump-outs (GNU labels-as-values, gcc/clang on both targets).
 */
#define __this_address	({ __label__ __here; __here: barrier(); &&__here; })

#define xfs_sort(a, n, s, fn)	sort(a, n, s, fn, NULL)
#define xfs_stack_trace()	((void)0)

#define STATIC static noinline

/* dev_t helpers for special-file inodes (xfs_dev_t is the on-disk form). */
#define MINORBITS		20
#define MKDEV_(ma, mi)		(((u32)(ma) << MINORBITS) | (u32)(mi))
static inline u32 sysv_major_(u32 dev) { return (dev >> 18) & 0x1ff; }
static inline u32 sysv_minor_(u32 dev) { return dev & 0x3ffff; }
static inline u32 xfs_to_linux_dev_t(xfs_dev_t dev) { return MKDEV_(sysv_major_(dev), sysv_minor_(dev)); }
static inline xfs_dev_t linux_to_xfs_dev_t(u32 dev) { return ((dev >> MINORBITS) << 18) | (dev & 0xfffff & 0x3ffff); }

/*
 * Metadata corruption not caught by block or structure verifiers.
 */
#define XFS_IS_CORRUPT(mp, expr)	\
	(unlikely(expr) ? xfs_corruption_error(#expr, XFS_ERRLEVEL_LOW, (mp), \
					       NULL, 0, __FILE__, __LINE__, \
					       __this_address), \
			  true : false)

#define XFS_IS_REALTIME_INODE(ip)			\
	(((ip)->i_diflags & XFS_DIFLAG_REALTIME) &&	\
	 (ip)->i_mount->m_rtdev_targp)
#define XFS_IS_REALTIME_MOUNT(mp) ((mp)->m_rtdev_targp ? 1 : 0)

/* linux/fs.h FS_XFLAG_* (fsxattr flags, Linux uapi values). */
#define FS_XFLAG_REALTIME	0x00000001
#define FS_XFLAG_PREALLOC	0x00000002
#define FS_XFLAG_IMMUTABLE	0x00000008
#define FS_XFLAG_APPEND		0x00000010
#define FS_XFLAG_SYNC		0x00000020
#define FS_XFLAG_NOATIME	0x00000040
#define FS_XFLAG_NODUMP		0x00000080
#define FS_XFLAG_RTINHERIT	0x00000100
#define FS_XFLAG_PROJINHERIT	0x00000200
#define FS_XFLAG_NOSYMLINKS	0x00000400
#define FS_XFLAG_EXTSIZE	0x00000800
#define FS_XFLAG_EXTSZINHERIT	0x00001000
#define FS_XFLAG_NODEFRAG	0x00002000
#define FS_XFLAG_FILESTREAM	0x00004000
#define FS_XFLAG_DAX		0x00008000
#define FS_XFLAG_COWEXTSIZE	0x00010000
#define FS_XFLAG_CASEFOLD	0x20000000	/* local value; read path only reports it */
#define FS_XFLAG_HASATTR	0x80000000

#define PTR_FMT "%p"

#endif /* _XFS_PLATFORM_H */
