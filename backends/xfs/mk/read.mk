# SPDX-License-Identifier: GPL-2.0-only
# File read + readlink (Task 10): make test-read [SAN=1]. Included from Makefile
# (read.c is in BACKEND_SRC). Shares the fixture cache with mount.mk (FIXDIR).
$(OUT)/be/read.o: LIBXFS_BE_WNO = $(LIBXFS_WNO)
READ_TEST = $(OUT)/test_read$(EXE)
$(READ_TEST): tests/test_read.c $(CORE_OBJ) $(SHIM_OBJ) $(BACKEND_OBJ) $(PENDING_LIB) ops.h icache.h wfs_xfs.h Makefile mk/read.mk | $(OUT)
	$(CC) $(CFLAGS) $(LIBXFS_WNO) -Wno-format-truncation $(SANFLAGS) $(INC) $(KINC) -I. -o $@ tests/test_read.c \
		$(CORE_OBJ) $(SHIM_OBJ) $(BACKEND_OBJ) $(PENDING_LIB) $(LIBS)

test-read:
ifneq ($(CROSS),)
	@echo "SKIPPED: test-read (cross build)"
else ifeq ($(HAVE_KERNEL),)
	@echo "SKIPPED: test-read (set KERNEL_XFS_SRC to a tree with libxfs/)"
else ifeq ($(shell command -v mkfs.xfs),)
	@echo "SKIPPED: test-read (mkfs.xfs not installed: install xfsprogs)"
else ifeq ($(shell command -v xfs_db),)
	@echo "SKIPPED: test-read (xfs_db not installed: install xfsprogs)"
else
	$(MAKE) --no-print-directory $(READ_TEST) $(FIXDIR)/.done
	$(READ_TEST) $(FIXDIR) $(ROOTFIX)
endif

test-read-san:
	$(MAKE) --no-print-directory test-read SAN=1 OUT=$(OUT)-san

.PHONY: test-read test-read-san
