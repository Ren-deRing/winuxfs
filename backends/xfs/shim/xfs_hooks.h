/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef XFS_HOOKS_H_
#define XFS_HOOKS_H_
/* Live hooks are scrub/online-repair only: empty, as with !CONFIG_XFS_LIVE_HOOKS. */
struct xfs_hooks { int unused; };
# define DEFINE_STATIC_XFS_HOOK_SWITCH(name)
# define xfs_hooks_switch_on(name)		((void)0)
# define xfs_hooks_switch_off(name)		((void)0)
# define xfs_hooks_switched_on(name)		(false)
# define xfs_hooks_init(chain)			((void)0)
# define NOTIFY_DONE 0
# define xfs_hooks_call(chain, val, priv)	(NOTIFY_DONE)
#endif
