/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * The VFS objects kernel XFS code touches (linux/fs.h subset): struct inode
 * (embedded in struct xfs_inode as i_vnode), super_block (one per mount,
 * xfs_mount.m_super), dir_context. The XFS structs themselves come from the
 * kernel headers (fs/xfs/xfs_inode.h, xfs_mount.h) since M3a T3.
 */
#ifndef WFS_SHIM_VFS_H
#define WFS_SHIM_VFS_H

#include "wfs_shim.h"

struct address_space;
struct user_namespace;
struct mnt_idmap;

/* One per mount. s_dev is what trace points print; nothing else reads it. */
struct sb_writers { int frozen; };
struct super_block {
	u32			s_dev;
	unsigned long		s_flags;	/* SB_ACTIVE, ... */
	char			s_id[32];
	loff_t			s_maxbytes;
	struct user_namespace	*s_user_ns;
	struct sb_writers	s_writers;
	void			*s_fs_info;	/* the xfs_mount */
};
#define SB_RDONLY		(1u << 0)
#define SB_BORN			(1u << 29)

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
	struct super_block	*i_sb;		/* the mount's (xfs_mount.m_super) */
	struct rw_semaphore	i_rwsem;	/* XFS_IOLOCK */
	u64			i_blocks;
	unsigned short		i_bytes;
	struct rcu_head		i_rcu;
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


/* No page cache, DAX or stable-writes devices (M3 design 2). */
#define unlock_new_inode(i)			((void)(i))
#define mapping_set_stable_writes(m)		((void)(m))
#define mapping_clear_stable_writes(m)		((void)(m))
#define bdev_stable_writes(b)			((void)(b), false)
struct llist_node { struct llist_node *next; };

/*
 * Directory iteration (xfs_dir2_readdir.c, vendored top-level file).
 * dir_emit() hands each entry to ctx->actor; "." and ".." are dropped here
 * (the ABI never returns them; design 8.4) but ctx->pos still advances, so the
 * cookie arithmetic of the kernel code is untouched.
 */
#ifdef __linux__
#include <dirent.h>	/* glibc's DT_* enum (same values): host tests include it too */
#endif
#ifndef DT_UNKNOWN
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

#endif /* WFS_SHIM_VFS_H */
