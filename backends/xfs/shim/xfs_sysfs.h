/* SPDX-License-Identifier: GPL-2.0-only */
/* fs/xfs/xfs_sysfs.h replacement: no sysfs here, kobjects are inert. */
#ifndef __XFS_SYSFS_H__
#define __XFS_SYSFS_H__

struct kobj_type;
struct xfs_kobj {
	struct kobject		kobject;
	struct completion	complete;
};
extern const struct kobj_type xfs_mp_ktype;
extern const struct kobj_type xfs_dbg_ktype;
extern const struct kobj_type xfs_log_ktype;
extern const struct kobj_type xfs_stats_ktype;

static inline int xfs_sysfs_init(struct xfs_kobj *kobj, const struct kobj_type *ktype,
				 struct xfs_kobj *parent_kobj, const char *name)
{
	(void)kobj; (void)ktype; (void)parent_kobj; (void)name;
	return 0;
}
static inline void xfs_sysfs_del(struct xfs_kobj *kobj) { (void)kobj; }
static inline int xfs_mount_sysfs_init(struct xfs_mount *mp) { (void)mp; return 0; }
static inline void xfs_mount_sysfs_del(struct xfs_mount *mp) { (void)mp; }
static inline int xfs_zoned_sysfs_init(struct xfs_mount *mp) { (void)mp; return 0; }
static inline void xfs_zoned_sysfs_del(struct xfs_mount *mp) { (void)mp; }
#endif
