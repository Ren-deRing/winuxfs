<!-- SPDX-License-Identifier: GPL-2.0-only -->
# LLP64 hazards in the XFS shim

On Windows x64 (mingw) `long` and `unsigned long` are 32 bit; on Linux x86-64
(the kernel's assumption) they are 64 bit. Rules the shim follows:

1. **64-bit integers are `long long` on every target.** `wfs_types.h` forces
   `int64_t`/`uint64_t`/`intmax_t`/`loff_t` to `long long` even on glibc (where
   stdint says `long`), so `u64`, `xfs_off_t` and `%llx` formats are identical
   on host and Windows builds. The native test build therefore exercises the
   same type identities as the mingw build.
2. **`unsigned long` is only a bitmap word.** Grepping `third_party/linux-xfs/libxfs`
   for `unsigned long` / `BITS_PER_LONG` shows only: `pag_opstate`
   (bit numbers 0..4, `test_bit`/`set_bit`), `li_flags`, `t_pflags`,
   `pag_ici_reclaim_cursor`, the xarray `index` in `xfs_group.c` (group
   numbers, `xfs_group_max < 2^32`), `xfs_cksum.h` offsets (small), and
   `(long)` casts of block deltas in write-path code (`xfs_trans_mod_sb`,
   `xfs_bmap.c`, `xfs_alloc.c`), never executed on a read-only mount.
   Decision: `BITS_PER_LONG = sizeof(long) * 8` (32 on mingw), the bitops work
   for any word size, and all bit numbers used are below 32. No code relies on
   a 64-bit `long`.
3. **No `1UL << n` with n >= 32** in shim code; use `1ULL`. Shim bit masks use
   `1UL << (nr % BITS_PER_LONG)`, which is correct for either width.
4. **printf**: `__USE_MINGW_ANSI_STDIO=1` is defined before any libc include
   and `__printf` uses `gnu_printf` on Windows, so `%zu`, `%llx`, `%Lx` are
   valid and checked. Kernel code passes `(unsigned long long)` for 64-bit
   values; never `%lx` for a u64.
5. **errno**: mingw's numbers differ from Linux (ENOSYS 40, ENOTEMPTY 41,
   ENAMETOOLONG 38, ELOOP 114). `wfs_errno.h` includes the C `<errno.h>` first
   and then `#undef`/redefines every `E*` to the Linux value from
   `include/winuxfs/errno.h`. The list is derived from that header: if errno.h
   gains an entry, add the matching `#undef`/`#define` pair.
6. **Atomics / `atomic64_t`** are `long long` based, never `long`.
7. **`time64_t`, `__kernel_long_t`** are `long long`, so inode timestamps are
   64 bit on both targets.
8. **Windows headers never enter kernel-derived code.** SRW lock functions are
   declared locally in `wfs_sync.h`; `<windows.h>` macros (`min`, `max`, `ERROR`,
   `small`) would collide with kernel identifiers.
