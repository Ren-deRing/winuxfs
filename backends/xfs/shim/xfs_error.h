/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Error reporting. Verifier errors are reported to the host log by the
 * backend (Task 4/5 implement these functions); error injection is compiled
 * out exactly like a non-DEBUG kernel (design 3.3).
 */
#ifndef __XFS_ERROR_H__
#define	__XFS_ERROR_H__

struct xfs_mount;
struct xfs_buf;
struct xfs_inode;

void xfs_error_report(const char *tag, int level, int error,
		struct xfs_mount *mp, const char *filename,
		int linenum, xfs_failaddr_t failaddr);
void xfs_corruption_error(const char *tag, int level, struct xfs_mount *mp,
		const void *buf, size_t bufsize, const char *filename,
		int linenum, xfs_failaddr_t failaddr);
void xfs_buf_corruption_error(struct xfs_buf *bp, xfs_failaddr_t fa);
void xfs_buf_verifier_error(struct xfs_buf *bp, int error, const char *name,
		const void *buf, size_t bufsz, xfs_failaddr_t failaddr);
void xfs_verifier_error(struct xfs_buf *bp, int error, xfs_failaddr_t failaddr);
void xfs_inode_verifier_error(struct xfs_inode *ip, int error, const char *name,
		const void *buf, size_t bufsz, xfs_failaddr_t failaddr);

#define	XFS_ERROR_REPORT(e, lvl, error, mp)	\
	xfs_error_report(e, lvl, error, mp, __FILE__, __LINE__, __return_address)
#define	XFS_CORRUPTION_ERROR(e, lvl, mp, buf, bufsize)	\
	xfs_corruption_error(e, lvl, mp, buf, bufsize, \
			     __FILE__, __LINE__, __return_address)

#define XFS_ERRLEVEL_OFF	0
#define XFS_ERRLEVEL_LOW	1
#define XFS_ERRLEVEL_HIGH	5

#define XFS_CORRUPTION_DUMP_LEN		(128)

#define xfs_errortag_init(mp)			(0)
#define xfs_errortag_del(mp)
#define XFS_TEST_ERROR(mp, tag)			(false)
#define XFS_ERRORTAG_DELAY(mp, tag)		((void)0)
#define xfs_errortag_add(mp, tag)		(-ENOSYS)
#define xfs_errortag_copy(dst_mp, src_mp)	((void)0)
#define xfs_errortag_add_name(mp, tag_name)	(-ENOSYS)
#define xfs_errortag_clearall(mp)		(-ENOSYS)

#define		XFS_NO_PTAG			0u
#define		XFS_PTAG_IFLUSH			(1u << 0)
#define		XFS_PTAG_LOGRES			(1u << 1)
#define		XFS_PTAG_AILDELETE		(1u << 2)
#define		XFS_PTAG_ERROR_REPORT		(1u << 3)
#define		XFS_PTAG_SHUTDOWN_CORRUPT	(1u << 4)
#define		XFS_PTAG_SHUTDOWN_IOERROR	(1u << 5)
#define		XFS_PTAG_SHUTDOWN_LOGERROR	(1u << 6)
#define		XFS_PTAG_FSBLOCK_ZERO		(1u << 7)
#define		XFS_PTAG_VERIFIER_ERROR		(1u << 8)

#endif	/* __XFS_ERROR_H__ */
