# SPDX-License-Identifier: GPL-2.0-only
# Mount (Task 6). Included from Makefile (mount.c, options.c are in BACKEND_SRC).
#   make test-mount              native; builds the fixture images once
#   make test-mount SAN=1        ASan+UBSan (OUT is separate)
#   make test-mount-san          same
# Fixtures: tools/gen-fixtures.sh into FIXDIR (cached; delete to regenerate);
# SKIPPED visibly when gcc, kernel tree, mkfs.xfs, xfs_db or zstd is missing.
$(OUT)/be/mount.o $(OUT)/be/options.o $(OUT)/be/logcheck.o: LIBXFS_BE_WNO = $(LIBXFS_WNO)
MOUNT_TEST = $(OUT)/test_mount$(EXE)
FIXDIR ?= ../../target/xfs-fixtures
ROOTFIX ?= ../../tests/xfs/root-fixtures
BE_NOENTRY = $(filter-out $(OUT)/be/entry_stub.o,$(BACKEND_OBJ))
$(MOUNT_TEST): tests/test_mount.c $(LIBXFS_OBJ) $(SHIM_OBJ) $(BACKEND_OBJ) $(PENDING_LIB) mount.h wfs_xfs.h Makefile mk/mount.mk | $(OUT)
	$(CC) $(CFLAGS) $(LIBXFS_WNO) $(SANFLAGS) $(INC) $(KINC) -I. -o $@ tests/test_mount.c \
		$(LIBXFS_OBJ) $(SHIM_OBJ) $(BACKEND_OBJ) $(PENDING_LIB) $(LIBS)

$(FIXDIR)/.done:
	../../tools/gen-fixtures.sh --out $(FIXDIR) && touch $@

test-mount:
ifneq ($(CROSS),)
	@echo "SKIPPED: test-mount (cross build)"
else ifeq ($(HAVE_KERNEL),)
	@echo "SKIPPED: test-mount (set KERNEL_XFS_SRC to a tree with libxfs/)"
else ifeq ($(shell command -v mkfs.xfs),)
	@echo "SKIPPED: test-mount (mkfs.xfs not installed: install xfsprogs)"
else ifeq ($(shell command -v xfs_db),)
	@echo "SKIPPED: test-mount (xfs_db not installed: install xfsprogs)"
else
	$(MAKE) --no-print-directory $(MOUNT_TEST) $(FIXDIR)/.done
	$(MOUNT_TEST) $(FIXDIR) $(ROOTFIX)
endif

test-mount-san:
	$(MAKE) --no-print-directory test-mount SAN=1 OUT=$(OUT)-san

.PHONY: test-mount test-mount-san
