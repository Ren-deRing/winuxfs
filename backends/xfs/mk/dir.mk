# SPDX-License-Identifier: GPL-2.0-only
# Directory lookup/readdir (Task 9): make test-dir [SAN=1]. Included from Makefile.
# xfs_dir2_readdir.c is the vendored kernel file plus patch 0001 (cookie mask
# removed). It is patched into a generated copy (not edited in place, and not
# the whole tree: a sibling xfs_platform.h would shadow the shim's) and built
# as a normal backend object; "xfs_dir2_readdir.c" is in BACKEND_SRC.
DIR_PATCH = ../../third_party/linux-xfs-patches/0001-readdir-no-cookie-mask.patch
$(OUT)/gen/xfs_dir2_readdir.c: $(KERNEL_XFS_SRC)/xfs_dir2_readdir.c $(DIR_PATCH)
	@mkdir -p $(dir $@)
	cp $< $@.tmp && chmod u+w $@.tmp && patch -s -p1 --no-backup-if-mismatch $@.tmp < $(DIR_PATCH) && mv $@.tmp $@
$(OUT)/be/xfs_dir2_readdir.o: $(OUT)/gen/xfs_dir2_readdir.c $(SHIM_HDR) Makefile
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(LIBXFS_WNO) $(SANFLAGS) $(PIC) $(VIS) $(INC) $(KINC) -c -o $@ $<

# Patch 0002 (bounded leaf address, fuzz finding): xfs_dir2_leaf.c and
# xfs_dir2_node.c are built from patched copies; the other libxfs files from
# the vendored tree as before.
DIR_PATCH2 = ../../third_party/linux-xfs-patches/0002-dir-lookup-bound-leaf-address.patch
$(OUT)/gen/libxfs/.patched: $(KERNEL_XFS_SRC)/libxfs/xfs_dir2_leaf.c $(KERNEL_XFS_SRC)/libxfs/xfs_dir2_node.c $(DIR_PATCH2)
	@mkdir -p $(dir $@)
	cp $(KERNEL_XFS_SRC)/libxfs/xfs_dir2_leaf.c $(KERNEL_XFS_SRC)/libxfs/xfs_dir2_node.c $(dir $@)
	chmod u+w $(dir $@)xfs_dir2_leaf.c $(dir $@)xfs_dir2_node.c
	patch -s -d $(dir $@) -p1 --no-backup-if-mismatch < $(abspath $(DIR_PATCH2))
	touch $@
$(OUT)/libxfs/xfs_dir2_leaf.o: $(OUT)/libxfs/%.o: $(OUT)/gen/libxfs/.patched $(SHIM_HDR) Makefile
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(LIBXFS_WNO) $(SANFLAGS) $(if $(SAN),-fno-sanitize=alignment) $(PIC) $(VIS) $(INC) $(KINC) -c -o $@ $(OUT)/gen/libxfs/$*.c
$(OUT)/libxfs/xfs_dir2_node.o: $(OUT)/libxfs/%.o: $(OUT)/gen/libxfs/.patched $(SHIM_HDR) Makefile
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(LIBXFS_WNO) $(SANFLAGS) $(if $(SAN),-fno-sanitize=alignment) $(PIC) $(VIS) $(INC) $(KINC) -c -o $@ $(OUT)/gen/libxfs/$*.c
$(OUT)/be/dir.o: LIBXFS_BE_WNO = $(LIBXFS_WNO)

# Vendored libxfs casts 4-byte aligned short btree pointers (inobt/bnobt/cnt
# nodes) to the 8-byte aligned union xfs_btree_ptr (xfs_btree.c:341). Harmless
# on x86/arm64 and the kernel idiom, but UBSan's alignment check flags it as soon
# as a multi-level inobt is read (large directories, 5000+ inodes).
$(OUT)/libxfs/%.o: SANFLAGS += $(if $(SAN),-fno-sanitize=alignment)

DIR_TEST = $(OUT)/test_dir$(EXE)
$(DIR_TEST): tests/test_dir.c $(LIBXFS_OBJ) $(SHIM_OBJ) $(BACKEND_OBJ) $(PENDING_LIB) icache.h wfs_xfs.h Makefile mk/dir.mk | $(OUT)
	$(CC) $(CFLAGS) $(LIBXFS_WNO) -Wno-format-truncation $(SANFLAGS) $(INC) $(KINC) -I. -o $@ tests/test_dir.c \
		$(LIBXFS_OBJ) $(SHIM_OBJ) $(BACKEND_OBJ) $(PENDING_LIB) $(LIBS)

test-dir:
ifneq ($(CROSS),)
	@echo "SKIPPED: test-dir (cross build)"
else ifeq ($(HAVE_KERNEL),)
	@echo "SKIPPED: test-dir (set KERNEL_XFS_SRC to a tree with libxfs/)"
else ifeq ($(shell command -v mkfs.xfs),)
	@echo "SKIPPED: test-dir (mkfs.xfs not installed: install xfsprogs)"
else ifeq ($(shell command -v xfs_db),)
	@echo "SKIPPED: test-dir (xfs_db not installed: install xfsprogs)"
else
	$(MAKE) --no-print-directory $(DIR_TEST)
	$(DIR_TEST)
endif

test-dir-san:
	$(MAKE) --no-print-directory test-dir SAN=1 OUT=$(OUT)-san

.PHONY: test-dir test-dir-san
