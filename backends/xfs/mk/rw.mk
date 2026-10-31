# SPDX-License-Identifier: GPL-2.0-only
# Read-write mount (M3a T4). Included from Makefile.
#   make test-rw          native
#   make test-rw-san      ASan+UBSan (OUT is separate)
#   make test-rw-tsan     ThreadSanitizer
# Needs mkfs.xfs, xfs_repair, xfs_db and xfs_logprint; SKIPPED visibly otherwise.
RW_TEST = $(OUT)/test_rw$(EXE)
RW_TMP ?= $(abspath $(shell git rev-parse --git-common-dir 2>/dev/null)/../target/tmp)
$(RW_TEST): tests/test_rw.c $(CORE_OBJ) $(SHIM_OBJ) $(BACKEND_OBJ) $(PENDING_LIB) mount.h wfs_xfs.h Makefile mk/rw.mk | $(OUT)
	$(CC) $(CFLAGS) $(LIBXFS_WNO) $(SANFLAGS) $(INC) $(KINC) -I. -o $@ tests/test_rw.c \
		$(CORE_OBJ) $(SHIM_OBJ) $(BACKEND_OBJ) $(PENDING_LIB) $(LIBS)

test-rw:
ifneq ($(CROSS),)
	@echo "SKIPPED: test-rw (cross build)"
else ifeq ($(HAVE_KERNEL),)
	@echo "SKIPPED: test-rw (set KERNEL_XFS_SRC to a tree with libxfs/)"
else ifeq ($(shell command -v mkfs.xfs && command -v xfs_repair && command -v xfs_db && command -v xfs_logprint),)
	@echo "SKIPPED: test-rw (needs mkfs.xfs, xfs_repair, xfs_db, xfs_logprint: install xfsprogs)"
else
	$(MAKE) --no-print-directory $(RW_TEST)
	@mkdir -p $(RW_TMP)
	$(RW_TEST) $(RW_TMP)
endif

test-rw-san:
	$(MAKE) --no-print-directory test-rw SAN=1 OUT=$(OUT)-san

# xfsaild, the CIL push work and log I/O completion run on their own threads
test-rw-tsan:
	$(MAKE) --no-print-directory test-rw SAN=1 SANFLAGS='-fsanitize=thread -fno-omit-frame-pointer' \
		OUT=$(OUT)-tsan

.PHONY: test-rw test-rw-san test-rw-tsan
