/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * In-core inode for the read-only port (derived from fs/xfs/xfs_inode.h;
 * Task 8 implements the cache and the lock functions declared here).
 * `struct inode` is the minimal VFS inode libxfs touches (design 3.3);
 * kernel-only members (dquots, log item, GC lists, ioend) are dropped.
 */
#ifndef	__XFS_INODE_H__
#define	__XFS_INODE_H__

#include "xfs_inode_buf.h"
#include "xfs_inode_fork.h"
#include "xfs_inode_util.h"

struct xfs_dinode;
struct xfs_inode;
struct xfs_buf;
struct xfs_bmbt_irec;
struct xfs_mount;
struct xfs_trans;
struct xfs_dquot;
struct xfs_inode_log_item;

struct inode {
	u64			i_ino;
	u32			i_mode;		/* umode_t */
	u32			i_nlink;
	kuid_t			i_uid;
	kgid_t			i_gid;
	u32			i_generation;
	u32			i_rdev;
	loff_t			i_size;
	struct timespec64	i_atime_, i_mtime_, i_ctime_;
	u64			i_version;
	unsigned long		i_state;
	unsigned long		i_flags;
	spinlock_t		i_lock;		/* i_state */
	atomic_t		i_dio_count;	/* always 0: no direct I/O path */
	struct address_space	*i_mapping;	/* always NULL: no page cache */
};
static inline unsigned long inode_state_read_once(struct inode *i)
{
	return __atomic_load_n(&i->i_state, __ATOMIC_RELAXED);
}
static inline void inode_state_clear(struct inode *i, unsigned long f)
{
	__atomic_and_fetch(&i->i_state, ~f, __ATOMIC_RELAXED);
}
static inline void inode_state_set(struct inode *i, unsigned long f)
{
	__atomic_or_fetch(&i->i_state, f, __ATOMIC_RELAXED);
}
/* VFS reference: a transient icache reference (dropped by xfs_irele). */
void wfs_xfs_ihold(struct inode *inode);
#define ihold(i)		wfs_xfs_ihold(i)
struct timespec64 current_time(struct inode *inode);
static inline struct timespec64 inode_set_ctime_current(struct inode *i)
{
	struct timespec64 now = current_time(i);

	i->i_ctime_ = now;
	return now;
}
/* NFS change attribute: bump on every change (cheap here, no I_VERSION_QUERIED). */
static inline bool inode_maybe_inc_iversion(struct inode *i, bool force)
{
	(void)force;
	__atomic_add_fetch(&i->i_version, 1, __ATOMIC_RELAXED);
	return true;
}
#define inode_inc_iversion(i)	((void)inode_maybe_inc_iversion((i), true))
#define i_size_read(i)		((i)->i_size)
#define i_size_write(i, s)	((i)->i_size = (s))
#define set_nlink(i, n)		((i)->i_nlink = (n))
#define i_uid_write(i, v)	((i)->i_uid = (v))
#define i_gid_write(i, v)	((i)->i_gid = (v))
#define i_uid_read(i)		((i)->i_uid)
#define i_gid_read(i)		((i)->i_gid)
#define inode_set_iversion_queried(i, v) ((i)->i_version = (v))
#define inode_peek_iversion(i)	((i)->i_version)
#define inode_set_iversion(i, v) ((i)->i_version = (v))
#define inc_nlink(i)		((i)->i_nlink++)
#define drop_nlink(i)		((i)->i_nlink--)
#define inode_fsuid_set(i, idmap)	((i)->i_uid = 0)
#define inode_fsgid_set(i, idmap)	((i)->i_gid = 0)
static inline void inode_init_owner(const struct mnt_idmap *idmap, struct inode *inode,
				    const struct inode *dir, u32 mode)
{
	(void)idmap; (void)dir;
	inode->i_mode = mode;
	inode->i_uid = 0;
	inode->i_gid = 0;
}
#define IS_DAX(i)		0
#define IS_I_VERSION(i)		1
#define inode_peek_iversion_raw(i) ((i)->i_version)
#define IS_NOATIME(i)		0

#define WFS_TS_ACCESSORS(n, f)							\
static inline struct timespec64 inode_get_##n##_ts(const struct inode *i)	\
{ struct timespec64 t = { i->f.tv_sec, i->f.tv_nsec }; return t; }		\
static inline void inode_set_##n##_to_ts(struct inode *i, struct timespec64 t)	\
{ i->f.tv_sec = t.tv_sec; i->f.tv_nsec = t.tv_nsec; }				\
static inline s64 inode_get_##n##_sec(const struct inode *i) { return i->f.tv_sec; }	\
static inline long inode_get_##n##_nsec(const struct inode *i) { return (long)i->f.tv_nsec; }
WFS_TS_ACCESSORS(atime, i_atime_)
WFS_TS_ACCESSORS(mtime, i_mtime_)
WFS_TS_ACCESSORS(ctime, i_ctime_)

typedef struct xfs_inode {
	struct xfs_mount	*i_mount;	/* fs mount struct ptr */
	struct xfs_dquot	*i_udquot;	/* always NULL */
	struct xfs_dquot	*i_gdquot;
	struct xfs_dquot	*i_pdquot;

	struct xfs_imap		i_imap;		/* location for xfs_imap() */

	struct xfs_ifork	*i_cowfp;	/* copy on write extents */
	struct xfs_ifork	i_df;		/* data fork */
	struct xfs_ifork	i_af;		/* attribute fork */

	struct xfs_inode_log_item *i_itemp;	/* always NULL */
	struct rw_semaphore	i_lock;		/* inode lock */
	atomic_t		i_pincount;

	uint16_t		i_checked;
	uint16_t		i_sick;

	spinlock_t		i_flags_lock;
	unsigned long		i_flags;	/* XFS_I* flags below */
	uint64_t		i_delayed_blks;
	xfs_fsize_t		i_disk_size;
	xfs_rfsblock_t		i_nblocks;
	prid_t			i_projid;
	xfs_extlen_t		i_extsize;
	union {
		uint32_t	i_used_blocks;
		xfs_extlen_t	i_cowextsize;
		uint16_t	i_flushiter;
	};
	uint8_t			i_forkoff;	/* attr fork offset >> 3 */
	enum xfs_metafile_type	i_metatype;
	uint16_t		i_diflags;
	uint64_t		i_diflags2;
	struct timespec64	i_crtime;	/* time created */

	xfs_agino_t		i_next_unlinked;
	xfs_agino_t		i_prev_unlinked;

	struct inode		i_vnode;	/* embedded VFS inode */

	/* winuxfs (Task 8): cache bookkeeping lives behind this pointer. */
	void			*i_priv;
} xfs_inode_t;

static inline bool xfs_inode_on_unlinked_list(const struct xfs_inode *ip)
{
	return ip->i_prev_unlinked != 0;
}

static inline bool xfs_inode_has_attr_fork(const struct xfs_inode *ip)
{
	return ip->i_forkoff > 0;
}

static inline struct xfs_ifork *
xfs_ifork_ptr(struct xfs_inode *ip, int whichfork)
{
	switch (whichfork) {
	case XFS_DATA_FORK:
		return &ip->i_df;
	case XFS_ATTR_FORK:
		if (!xfs_inode_has_attr_fork(ip))
			return NULL;
		return &ip->i_af;
	case XFS_COW_FORK:
		return ip->i_cowfp;
	default:
		ASSERT(0);
		return NULL;
	}
}

static inline unsigned int xfs_inode_fork_boff(struct xfs_inode *ip)
{
	return XFS_FORKOFF_TO_B(ip->i_forkoff);
}

static inline unsigned int xfs_inode_data_fork_size(struct xfs_inode *ip)
{
	if (xfs_inode_has_attr_fork(ip))
		return xfs_inode_fork_boff(ip);
	return XFS_LITINO(ip->i_mount);
}

static inline unsigned int xfs_inode_attr_fork_size(struct xfs_inode *ip)
{
	if (xfs_inode_has_attr_fork(ip))
		return XFS_LITINO(ip->i_mount) - xfs_inode_fork_boff(ip);
	return 0;
}

static inline unsigned int
xfs_inode_fork_size(struct xfs_inode *ip, int whichfork)
{
	switch (whichfork) {
	case XFS_DATA_FORK:
		return xfs_inode_data_fork_size(ip);
	case XFS_ATTR_FORK:
		return xfs_inode_attr_fork_size(ip);
	default:
		return 0;
	}
}

static inline struct xfs_inode *XFS_I(struct inode *inode)
{
	return container_of(inode, struct xfs_inode, i_vnode);
}

static inline struct inode *VFS_I(struct xfs_inode *ip) { return &ip->i_vnode; }
static inline const struct inode *VFS_IC(const struct xfs_inode *ip) { return &ip->i_vnode; }
static inline xfs_ino_t I_INO(const struct xfs_inode *ip) { return VFS_IC(ip)->i_ino; }

static inline xfs_fsize_t XFS_ISIZE(struct xfs_inode *ip)
{
	if (S_ISREG(VFS_I(ip)->i_mode))
		return i_size_read(VFS_I(ip));
	return ip->i_disk_size;
}

static inline void __xfs_iflags_set(xfs_inode_t *ip, unsigned long flags) { ip->i_flags |= flags; }
static inline void xfs_iflags_set(xfs_inode_t *ip, unsigned long flags)
{ spin_lock(&ip->i_flags_lock); __xfs_iflags_set(ip, flags); spin_unlock(&ip->i_flags_lock); }
static inline void xfs_iflags_clear(xfs_inode_t *ip, unsigned long flags)
{ spin_lock(&ip->i_flags_lock); ip->i_flags &= ~flags; spin_unlock(&ip->i_flags_lock); }
static inline int __xfs_iflags_test(const struct xfs_inode *ip, unsigned long flags)
{ return (ip->i_flags & flags) != 0; }
static inline int xfs_iflags_test(xfs_inode_t *ip, unsigned long flags)
{
	int ret;
	spin_lock(&ip->i_flags_lock);
	ret = __xfs_iflags_test(ip, flags);
	spin_unlock(&ip->i_flags_lock);
	return ret;
}

static inline bool xfs_is_reflink_inode(const struct xfs_inode *ip) { return ip->i_diflags2 & XFS_DIFLAG2_REFLINK; }
static inline bool xfs_is_metadir_inode(const struct xfs_inode *ip) { return ip->i_diflags2 & XFS_DIFLAG2_METADATA; }

static inline bool xfs_is_internal_inode(const struct xfs_inode *ip)
{
	struct xfs_mount *mp = ip->i_mount;

	if (xfs_has_metadir(mp))
		return xfs_is_metadir_inode(ip);
	return I_INO(ip) == mp->m_sb.sb_rbmino ||
	       I_INO(ip) == mp->m_sb.sb_rsumino ||
	       xfs_is_quota_inode(&mp->m_sb, I_INO(ip));
}

static inline bool xfs_is_zoned_inode(const struct xfs_inode *ip)
{ return xfs_has_zoned(ip->i_mount) && XFS_IS_REALTIME_INODE(ip); }

bool xfs_is_always_cow_inode(const struct xfs_inode *ip);
static inline bool xfs_is_cow_inode(const struct xfs_inode *ip)
{ return xfs_is_reflink_inode(ip) || xfs_is_always_cow_inode(ip); }
static inline bool xfs_inode_has_filedata(const struct xfs_inode *ip)
{ return ip->i_df.if_nextents > 0 || ip->i_delayed_blks > 0; }
static inline bool xfs_inode_has_cow_data(const struct xfs_inode *ip)
{ return ip->i_cowfp && ip->i_cowfp->if_bytes; }
static inline bool xfs_inode_has_bigtime(const struct xfs_inode *ip)
{ return ip->i_diflags2 & XFS_DIFLAG2_BIGTIME; }
static inline bool xfs_inode_has_large_extent_counts(const struct xfs_inode *ip)
{ return ip->i_diflags2 & XFS_DIFLAG2_NREXT64; }
static inline bool xfs_inode_has_bigrtalloc(const struct xfs_inode *ip)
{ return XFS_IS_REALTIME_INODE(ip) && ip->i_mount->m_sb.sb_rextsize > 1; }
static inline enum xfs_group_type xfs_inode_group_type(const struct xfs_inode *ip)
{ return XFS_IS_REALTIME_INODE(ip) ? XG_TYPE_RTG : XG_TYPE_AG; }

#define xfs_inode_buftarg(ip) \
	(XFS_IS_REALTIME_INODE(ip) ? \
		(ip)->i_mount->m_rtdev_targp : (ip)->i_mount->m_ddev_targp)

#define XFS_IRECLAIM		(1 << 0)
#define XFS_ISTALE		(1 << 1)
#define XFS_IRECLAIMABLE	(1 << 2)
#define XFS_INEW		(1 << 3)
#define XFS_IPRESERVE_DM_FIELDS	(1 << 4)
#define XFS_ITRUNCATED		(1 << 5)
#define XFS_EOFBLOCKS_RELEASED	(1 << 6)
#define XFS_IFLUSHING		(1 << 7)
#define __XFS_IPINNED_BIT	8
#define XFS_IPINNED		(1 << __XFS_IPINNED_BIT)
#define XFS_IEOFBLOCKS		(1 << 9)
#define XFS_NEED_INACTIVE	(1 << 10)
#define XFS_IRECOVERY		(1 << 11)
#define XFS_ICOWBLOCKS		(1 << 12)
#define XFS_INACTIVATING	(1 << 13)
#define XFS_IQUOTAUNCHECKED	(1 << 14)
#define XFS_IREMAPPING		(1U << 15)

#define	XFS_IOLOCK_EXCL		(1u << 0)
#define	XFS_IOLOCK_SHARED	(1u << 1)
#define	XFS_ILOCK_EXCL		(1u << 2)
#define	XFS_ILOCK_SHARED	(1u << 3)
#define	XFS_MMAPLOCK_EXCL	(1u << 4)
#define	XFS_MMAPLOCK_SHARED	(1u << 5)
#define XFS_LOCK_MASK		(XFS_IOLOCK_EXCL | XFS_IOLOCK_SHARED \
				| XFS_ILOCK_EXCL | XFS_ILOCK_SHARED \
				| XFS_MMAPLOCK_EXCL | XFS_MMAPLOCK_SHARED)
#define XFS_ILOCK_SHIFT			24
#define XFS_ILOCK_PARENT_VAL		5u
#define XFS_ILOCK_MAX_SUBCLASS		(XFS_ILOCK_PARENT_VAL - 1)
#define XFS_ILOCK_DEP_MASK		0xff000000u
#define	XFS_ILOCK_PARENT		(XFS_ILOCK_PARENT_VAL << XFS_ILOCK_SHIFT)
#define XFS_LOCK_SUBCLASS_MASK		0xfffff0000u

#define XFS_INHERIT_GID(pip)	\
	(xfs_has_grpid((pip)->i_mount) || (VFS_I(pip)->i_mode & S_ISGID))

/* Implemented by Task 8 (icache.c); ILOCK_* map to the i_lock rwlock. */
void		xfs_ilock(xfs_inode_t *, uint);
int		xfs_ilock_nowait(xfs_inode_t *, uint);
void		xfs_iunlock(xfs_inode_t *, uint);
void		xfs_ilock_demote(xfs_inode_t *, uint);
void		xfs_assert_ilocked(struct xfs_inode *, uint);
uint		xfs_ilock_data_map_shared(struct xfs_inode *);
uint		xfs_ilock_attr_map_shared(struct xfs_inode *);
void		xfs_irele(struct xfs_inode *ip);
#define xfs_ipincount(ip)	((unsigned int) atomic_read(&(ip)->i_pincount))

#define XFS_DEFAULT_COWEXTSZ_HINT 32

int		xfs_ifree(struct xfs_trans *, struct xfs_inode *);
int		xfs_itruncate_extents_flags(struct xfs_trans **, struct xfs_inode *,
				int, xfs_fsize_t, int);
static inline int
xfs_itruncate_extents(struct xfs_trans **tpp, struct xfs_inode *ip,
		      int whichfork, xfs_fsize_t new_size)
{ return xfs_itruncate_extents_flags(tpp, ip, whichfork, new_size, 0); }

struct xfs_inode *xfs_iunlink_lookup(struct xfs_perag *pag, xfs_agino_t agino);
int xfs_iunlink_reload_next(struct xfs_trans *tp, struct xfs_buf *agibp,
		xfs_agino_t prev_agino, xfs_agino_t next_agino);
bool xfs_ifork_zapped(const struct xfs_inode *ip, int whichfork);
void xfs_inode_count_blocks(struct xfs_trans *tp, struct xfs_inode *ip,
		xfs_filblks_t *dblocks, xfs_filblks_t *rblocks);
unsigned int xfs_inode_alloc_unitsize(struct xfs_inode *ip);

static inline bool xfs_inode_unlinked_incomplete(const struct xfs_inode *ip)
{ return VFS_IC(ip)->i_nlink == 0 && !xfs_inode_on_unlinked_list(ip); }


/*
 * Directory iteration (xfs_dir2_readdir.c, vendored top-level file).
 * dir_emit() hands each entry to ctx->actor; "." and ".." are dropped here
 * (the ABI never returns them; design 8.4) but ctx->pos still advances, so the
 * cookie arithmetic of the kernel code is untouched.
 */
#ifndef DT_UNKNOWN	/* glibc <dirent.h> (host tests) has the same values */
#define DT_UNKNOWN	0
#define DT_FIFO		1
#define DT_CHR		2
#define DT_DIR		4
#define DT_BLK		6
#define DT_REG		8
#define DT_LNK		10
#define DT_SOCK		12
#define DT_WHT		14
#endif

struct dir_context;
typedef bool (*wfs_dir_actor_t)(struct dir_context *, const char *name, int namelen,
				loff_t pos, u64 ino, unsigned int type);
struct dir_context {
	wfs_dir_actor_t	actor;
	loff_t		pos;
};

static inline bool dir_emit(struct dir_context *ctx, const char *name, int namelen,
			    u64 ino, unsigned int type)
{
	if ((namelen == 1 && name[0] == '.') ||
	    (namelen == 2 && name[0] == '.' && name[1] == '.'))
		return true;
	return ctx->actor(ctx, name, namelen, ctx->pos, ino, type);
}

/* Readahead is a no-op without a plug; the buffer cache has no queue. */
struct blk_plug { int unused; };
#define blk_start_plug(p)	((void)(p))
#define blk_finish_plug(p)	((void)(p))

int xfs_readdir(struct xfs_trans *tp, struct xfs_inode *dp, struct dir_context *ctx,
		size_t bufsize);

#endif	/* __XFS_INODE_H__ */
