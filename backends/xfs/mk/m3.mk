# SPDX-License-Identifier: GPL-2.0-only
# M3 write/log path objects (M3a T3). Included from Makefile.
# libxfs files build with the normal libxfs rule (LIBXFS_NAMES in Makefile);
# top-level fs/xfs files are copied to $(OUT)/gen first so that the shim's
# xfs_platform.h, not the kernel's sitting next to them, is found (as for
# xfs_dir2_readdir.c). Patched files: see third_party/linux-xfs-patches.
$(OUT)/gen/top/%.c: $(KERNEL_XFS_SRC)/%.c
	@mkdir -p $(dir $@)
	cp $< $@ && chmod u+w $@
$(OUT)/m3/%.o: $(OUT)/gen/top/%.c $(SHIM_HDR) Makefile
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(LIBXFS_WNO) $(SANFLAGS) $(if $(SAN),-fno-sanitize=alignment) $(PIC) $(VIS) $(INC) $(KINC) -c -o $@ $<
# Patch 0004 (rmap merge length, LLP64): xfs_rmap.c from a patched copy.
RMAP_PATCH = ../../third_party/linux-xfs-patches/0004-rmap-merge-len-u64.patch
$(OUT)/gen/libxfs/xfs_rmap.c: $(KERNEL_XFS_SRC)/libxfs/xfs_rmap.c $(RMAP_PATCH)
	@mkdir -p $(dir $@)
	cp $< $@.tmp && chmod u+w $@.tmp && patch -s -p1 --no-backup-if-mismatch $@.tmp < $(RMAP_PATCH) && mv $@.tmp $@
$(OUT)/libxfs/xfs_rmap.o: $(OUT)/libxfs/%.o: $(OUT)/gen/libxfs/%.c $(SHIM_HDR) Makefile
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(LIBXFS_WNO) $(SANFLAGS) $(if $(SAN),-fno-sanitize=alignment) $(PIC) $(VIS) $(INC) $(KINC) -c -o $@ $<
m3-objs: $(M3_OBJ) $(LIBXFS_OBJ)
.PHONY: m3-objs
