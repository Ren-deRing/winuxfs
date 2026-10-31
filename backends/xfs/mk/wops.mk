# SPDX-License-Identifier: GPL-2.0-only
# Read-write mount (M3a T4). Included from Makefile.
#   make test-wops          native
#   make test-wops-san      ASan+UBSan (OUT is separate)
#   make test-wops-tsan     ThreadSanitizer
# Needs mkfs.xfs, xfs_repair, xfs_db and xfs_logprint; SKIPPED visibly otherwise.
WOPS_TEST = $(OUT)/test_wops$(EXE)
WOPS_TMP ?= $(abspath $(shell git rev-parse --git-common-dir 2>/dev/null)/../target/tmp)
$(WOPS_TEST): tests/test_wops.c $(CORE_OBJ) $(SHIM_OBJ) $(BACKEND_OBJ) $(PENDING_LIB) mount.h wfs_xfs.h Makefile mk/wops.mk | $(OUT)
	$(CC) $(CFLAGS) $(LIBXFS_WNO) $(SANFLAGS) $(INC) $(KINC) -I. -o $@ tests/test_wops.c \
		$(CORE_OBJ) $(SHIM_OBJ) $(BACKEND_OBJ) $(PENDING_LIB) $(LIBS)

test-wops:
ifneq ($(CROSS),)
	@echo "SKIPPED: test-wops (cross build)"
else ifeq ($(HAVE_KERNEL),)
	@echo "SKIPPED: test-wops (set KERNEL_XFS_SRC to a tree with libxfs/)"
else ifeq ($(shell command -v mkfs.xfs && command -v xfs_repair && command -v xfs_db && command -v xfs_logprint),)
	@echo "SKIPPED: test-wops (needs mkfs.xfs, xfs_repair, xfs_db, xfs_logprint: install xfsprogs)"
else
	$(MAKE) --no-print-directory $(WOPS_TEST)
	@mkdir -p $(WOPS_TMP)
	$(WOPS_TEST) $(WOPS_TMP)
endif

test-wops-san:
	$(MAKE) --no-print-directory test-wops SAN=1 OUT=$(OUT)-san

# xfsaild, the CIL push work and log I/O completion run on their own threads
test-wops-tsan:
	$(MAKE) --no-print-directory test-wops SAN=1 SANFLAGS='-fsanitize=thread -fno-omit-frame-pointer' \
		OUT=$(OUT)-tsan

.PHONY: test-wops test-wops-san test-wops-tsan
