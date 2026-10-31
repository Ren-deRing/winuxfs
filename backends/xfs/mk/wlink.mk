# SPDX-License-Identifier: GPL-2.0-only
# Read-write mount (M3a T4). Included from Makefile.
#   make test-wlink          native
#   make test-wlink-san      ASan+UBSan (OUT is separate)
#   make test-wlink-tsan     ThreadSanitizer
# Needs mkfs.xfs, xfs_repair, xfs_db and xfs_logprint; SKIPPED visibly otherwise.
WLINK_TEST = $(OUT)/test_wlink$(EXE)
WLINK_TMP ?= $(abspath $(shell git rev-parse --git-common-dir 2>/dev/null)/../target/tmp)
$(WLINK_TEST): tests/test_wlink.c $(CORE_OBJ) $(SHIM_OBJ) $(BACKEND_OBJ) $(PENDING_LIB) mount.h wfs_xfs.h Makefile mk/wlink.mk | $(OUT)
	$(CC) $(CFLAGS) $(LIBXFS_WNO) $(SANFLAGS) $(INC) $(KINC) -I. -o $@ tests/test_wlink.c \
		$(CORE_OBJ) $(SHIM_OBJ) $(BACKEND_OBJ) $(PENDING_LIB) $(LIBS)

test-wlink:
ifneq ($(CROSS),)
	@echo "SKIPPED: test-wlink (cross build)"
else ifeq ($(HAVE_KERNEL),)
	@echo "SKIPPED: test-wlink (set KERNEL_XFS_SRC to a tree with libxfs/)"
else ifeq ($(shell command -v mkfs.xfs && command -v xfs_repair && command -v xfs_db && command -v xfs_logprint),)
	@echo "SKIPPED: test-wlink (needs mkfs.xfs, xfs_repair, xfs_db, xfs_logprint: install xfsprogs)"
else
	$(MAKE) --no-print-directory $(WLINK_TEST)
	@mkdir -p $(WLINK_TMP)
	$(WLINK_TEST) $(WLINK_TMP)
endif

test-wlink-san:
	$(MAKE) --no-print-directory test-wlink SAN=1 OUT=$(OUT)-san

# xfsaild, the CIL push work and log I/O completion run on their own threads
test-wlink-tsan:
	$(MAKE) --no-print-directory test-wlink SAN=1 SANFLAGS='-fsanitize=thread -fno-omit-frame-pointer' \
		OUT=$(OUT)-tsan

.PHONY: test-wlink test-wlink-san test-wlink-tsan
