# SPDX-License-Identifier: GPL-2.0-only
# statfs + summary counters (Task 11): make test-statfs [SAN=1]. Included from
# Makefile (statfs.c is in BACKEND_SRC). Fixture images: FIXDIR from mount.mk.
$(OUT)/be/statfs.o: LIBXFS_BE_WNO = $(LIBXFS_WNO)
STATFS_TEST = $(OUT)/test_statfs$(EXE)
$(STATFS_TEST): tests/test_statfs.c $(LIBXFS_OBJ) $(SHIM_OBJ) $(BACKEND_OBJ) $(PENDING_LIB) ops.h wfs_xfs.h Makefile mk/statfs.mk | $(OUT)
	$(CC) $(CFLAGS) $(LIBXFS_WNO) $(SANFLAGS) $(INC) $(KINC) -I. -o $@ tests/test_statfs.c \
		$(LIBXFS_OBJ) $(SHIM_OBJ) $(BACKEND_OBJ) $(PENDING_LIB) $(LIBS)

test-statfs:
ifneq ($(CROSS),)
	@echo "SKIPPED: test-statfs (cross build)"
else ifeq ($(HAVE_KERNEL),)
	@echo "SKIPPED: test-statfs (set KERNEL_XFS_SRC to a tree with libxfs/)"
else ifeq ($(shell command -v mkfs.xfs),)
	@echo "SKIPPED: test-statfs (mkfs.xfs not installed: install xfsprogs)"
else ifeq ($(shell command -v xfs_db),)
	@echo "SKIPPED: test-statfs (xfs_db not installed: install xfsprogs)"
else
	$(MAKE) --no-print-directory $(STATFS_TEST) $(FIXDIR)/.done
	$(STATFS_TEST) $(FIXDIR) $(ROOTFIX)
endif

test-statfs-san:
	$(MAKE) --no-print-directory test-statfs SAN=1 OUT=$(OUT)-san

.PHONY: test-statfs test-statfs-san
