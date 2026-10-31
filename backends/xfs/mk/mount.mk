# SPDX-License-Identifier: GPL-2.0-only
# Mount (Task 6). Included from Makefile (mount.c, options.c are in BACKEND_SRC).
#   make test-mount              native; builds the fixture images once
#   make test-mount SAN=1        ASan+UBSan (OUT is separate)
#   make test-mount-san          same
# Fixtures: tools/gen-fixtures.sh into FIXDIR (cached; delete to regenerate);
# SKIPPED visibly when gcc, kernel tree, mkfs.xfs, xfs_db or zstd is missing.
$(OUT)/be/mount.o $(OUT)/be/options.o $(OUT)/be/logcheck.o: LIBXFS_BE_WNO = $(LIBXFS_WNO)
MOUNT_TEST = $(OUT)/test_mount$(EXE)
# One absolute cache shared by all worktrees (gen-fixtures.sh cd's to the repo
# root, so a relative path resolved differently there and for `touch` below).
FIXDIR ?= $(abspath $(shell git rev-parse --git-common-dir 2>/dev/null)/../target/xfs-fixtures)
ROOTFIX ?= ../../tests/xfs/root-fixtures
BE_NOENTRY = $(filter-out $(OUT)/be/entry_stub.o,$(BACKEND_OBJ))
$(MOUNT_TEST): tests/test_mount.c $(CORE_OBJ) $(SHIM_OBJ) $(BACKEND_OBJ) $(PENDING_LIB) mount.h wfs_xfs.h Makefile mk/mount.mk | $(OUT)
	$(CC) $(CFLAGS) $(LIBXFS_WNO) $(SANFLAGS) $(INC) $(KINC) -I. -o $@ tests/test_mount.c \
		$(CORE_OBJ) $(SHIM_OBJ) $(BACKEND_OBJ) $(PENDING_LIB) $(LIBS)

# Regenerate when the generator or the manifest tool changes.
$(FIXDIR)/.done: ../../tools/gen-fixtures.sh ../../tools/xfs_manifest.py
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
else ifeq ($(shell command -v xfs_repair && command -v xfs_logprint && command -v python3),)
	@echo "SKIPPED: test-mount (fixture generation needs xfs_repair, xfs_logprint and python3)"
else
	$(MAKE) --no-print-directory $(MOUNT_TEST) $(FIXDIR)/.done
	$(MOUNT_TEST) $(FIXDIR) $(ROOTFIX)
endif

test-mount-san:
	$(MAKE) --no-print-directory test-mount SAN=1 OUT=$(OUT)-san

.PHONY: test-mount test-mount-san
