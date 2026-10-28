// SPDX-License-Identifier: GPL-2.0-only
/*
 * xfs_check_ondisk_structs_compiles: compile-time check (C11 _Static_assert
 * via the kernel's own xfs_ondisk.h) that the on-disk structures have the
 * Linux sizes and offsets with this toolchain (native and mingw/LLP64).
 * Needs KERNEL_XFS_SRC (libxfs/ headers); see the Makefile.
 */
#include "xfs_platform.h"
#include "xfs_format.h"
#include "xfs_da_format.h"
#include "xfs_log_format.h"
#include "xfs_ondisk.h"

void probe_xfs_check_ondisk_structs(void);
void probe_xfs_check_ondisk_structs(void)
{
	xfs_check_ondisk_structs();
}
