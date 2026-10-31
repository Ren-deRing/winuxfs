<!-- Sections above "M3 shim needs (generated)" are hand-written; regenerate the rest with
     tools/xfs-shim-probe.sh --pre and keep this header. -->
# M3 shim gap analysis (M3a T1)

Baseline on 2026-10-31: 33 kernel files, 1277 first-order errors with the probe preinclude
(2 files already clean: `xfs_trans_space.c`, `xfs_attr_inactive.c`). Groups, with the decision for T2/T3:

| Group | Size | Plan |
|---|---|---|
| Trace points | 231 `trace_*` | Extend `tools/gen-trace-stubs.sh` (no-op inline stubs), as in M2 |
| Sleep/wake primitives | wait queues, `wait_var_event`, `schedule`, completions, `sema_init/down/up`, `wake_up_bit` | Real implementations in the shim on pthread/SRW condition variables (`wfs_sync.h`); `schedule()` only inside the shim's wait helpers |
| Work queues and threads | `alloc_workqueue`, `INIT_WORK/DELAYED_WORK`, `queue_*work`, `cancel_delayed_work_sync`, `kthread_run/stop/should_stop` (xfsaild) | Design R2: start with `WFS_XFS_SYNC_LOG` (work runs inline on the caller, xfsaild pushed synchronously at commit/sync), then real threads in M3c |
| Log bio I/O | `struct bio`, `bio_*`, `submit_bio`, `blk_status_t`, `bdev_write_cache` in `xfs_log.c` (`xlog_write_iclog`) | Shim `struct bio` that collects the iclog's segments and issues `wfs_xfs_blk_write` + `blk_flush` (REQ_PREFLUSH/FUA) synchronously, completion called inline. `xfs_rw_bdev` (recovery reads) likewise |
| Lists/hlist/rbtree | list splice/move variants, hlist, `rb_*` (extent busy tree) | Lists/hlist in the shim; rbtree: vendor `lib/rbtree.c` (GPL-2.0-or-later) or a small shim tree — prefer vendoring (exact semantics) |
| Per-CPU / CIL | `this_cpu_ptr`, `get_cpu/put_cpu`, `for_each_cpu`, cpumask, `num_online_cpus`, percpu counter batch | Model one CPU: per-cpu data is a single instance under the CIL lock; correct, no scalability |
| VFS / page cache | `i_mapping`, `mapping_tagged`, `filemap_*`, `truncate_pagecache_range`, `inode_dio_wait`, `lock_two_nondirectories`, `evict_inodes`, `sb_start_intwrite`, iversion | No page cache in winuxfs: no-ops or "clean" answers; data I/O is the backend's own write path (design 2). Most callers are in `xfs_bmap_util.c` (fallocate/swapext) — compile only the functions M3 needs, the rest behind `#if 0` via patch or not linked (gc-sections) |
| Static keys, kobject/sysfs, freezer, memalloc flags | `DEFINE_STATIC_KEY_FALSE`, `kobject_*`, `try_to_freeze`, `memalloc_noreclaim_*` | Trivial shim definitions |
| Struct fields | `xfs_mount.m_ail/m_super/m_sync_workqueue`, `xfs_buf.b_pin_count/b_waiters`, `xfs_buftarg.bt_bdev`, `xfs_group.xg_busy_extents/xg_intents_drain`, `inode.i_mapping/i_lock/i_dio_count` | Add to the shim structs (M2 design 13 reserved the buffer fields) |
| Missing XFS glue | `xfs_buf_delwri_*`, `xfs_buftarg_wait/drain`, `xfs_lock_two_inodes`, `xfs_iflush_cluster`, `xfs_group_intent_get/put`, `xfs_readonly_buftarg`, `xfs_fs_writable`, `xfs_flush_inodes`, `xfs_zero_range` ... (18) | New backend code: buffer cache (T2), inode cache flush (T3/T6), mount state (T4) |
| Item-op tables, recover handlers, `struct kvec` | `iop_*` members, `xlog_recover_*_commit_pass2`, defer op fields | Cascades from the shim's minimal `xfs_log_item`/defer declarations: replace those shim stubs with the kernel headers (`libxfs/xfs_trans.h`, `xfs_defer.h`) once T2 lands |

## Status after M3a T2 (2026-10-31)

86 first-order errors left (from 1277), 13 of 33 files clean. Done in T2: trace stubs; tasks,
wait queues, completions, semaphores (lock-free fast path), kthreads, work queues
(`wfs_sched`); atomics, static keys, one-CPU per-cpu model, VFS answers, lists/hlist/ERR_PTR
(`wfs_kcompat`, `wfs_types`); rbtree (`third_party/linux-lib`, LLP64 patch); bio on the host
block layer (`bio.c`); buffer cache with the kernel buffer contract (locks, pins, delwri,
writes, shutdown on write error); LLP64 audit (patch 0004).

Left for T3 (they need the transaction/log objects to exist or a design decision there):

- `__KERNEL__`: kernel-only members (`xg_busy_extents`, perag work) — enable together with
  compiling `xfs_extent_busy.c` (xfs_group_insert allocates the busy tree), a no-op
  `xfs_blockgc_worker`, and creating `xfs_alloc_wq` (bmbt splits) at the first write mount.
- `xfs_mount` fields (`m_ail`, `m_super`, `m_sync_workqueue`) and the inode-level glue
  (`xfs_lock_two_inodes`, `xfs_lock_inodes`, `xfs_sort_inodes`, `xfs_iflush_cluster`): decide
  whether the shim keeps shadowing `xfs_mount.h`/`xfs_inode.h` or compiles the kernel's.
- Mount-state glue: `xfs_fs_writable`, `xfs_readonly_buftarg`, `xfs_dev_is_read_only`,
  `xfs_clear_incompat_log_features`, `xfs_flush_inodes`; map the kernel `xfs_force_shutdown`
  to `wfs_xfs_force_shutdown`.
- `xfs_sysfs.h` shadow (no-op `xfs_kobj`), `xfs_pnfs.h` (exportfs) not needed.
- `xfs_bmap_util.c`: `xfs_zero_range` (iomap) and `blkdev_issue_zeroout` belong to the data
  write path (T7); compile only what M3 needs.

Not in M3a: RT intents (`xfs_rt*`), exchmaps, dquot items stay stubs (rejected at mount: RT write, quota).

# M3 shim needs (generated)

Generated by `tools/xfs-shim-probe.sh --pre`: each M3 kernel file compiled with
`-fsyntax-only` against the shim. Counts are first-order; fixing headers exposes more.

## Probe-only additions that the shim must make real

- `__releases(x)`, `__acquires(x)`, `__must_hold(x)`, `____cacheline_aligned_in_smp` (empty annotations)
- `<linux/iversion.h>` (`inode_inc_iversion` etc.), `xfs_qm.h` (quota off: shadow header)

## Trace points: 0 missing `trace_*` (generate with tools/gen-trace-stubs.sh)



## Linux kernel APIs: 4

- `blkdev_issue_zeroout` — xfs_bmap_util
- `kobject_del` — xfs_log
- `kobject_init_and_add` — xfs_log
- `kobject_put` — xfs_log

## XFS/xlog functions not declared through the shim: 10

- `xfs_clear_incompat_log_features` — xfs_log
- `xfs_dev_is_read_only` — xfs_log_recover
- `xfs_flush_inodes` — xfs_trans
- `xfs_fs_writable` — xfs_log
- `xfs_iflush_cluster` — xfs_inode_item
- `xfs_lock_inodes` — xfs_defer
- `xfs_lock_two_inodes` — xfs_bmap_util, xfs_defer, xfs_trans
- `xfs_readonly_buftarg` — xfs_log, xfs_log_recover
- `xfs_sort_inodes` — xfs_defer
- `xfs_zero_range` — xfs_bmap_util

## Undeclared identifiers: 0


## Struct members missing from shim structs

- `struct xfs_group`: `xg_busy_extents`
- `struct xfs_mount`: `m_ail`, `m_super`, `m_sync_workqueue`

## Types incomplete/unknown

- `struct address_space` — xfs_bmap_util
- `struct xfs_kobj` — xfs_log

## Other errors (deduplicated, top 40)

- 17× field X has incomplete type
- 8× expected X, X, X, X or X before X token
- 4× X {aka X} has no member named X
- 1× linux/exportfs_block.h: 그런 파일이나 디렉터리가 없습니다
- 1× expected declaration specifiers before X
- 1× expected X at end of input
