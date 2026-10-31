# SPDX-License-Identifier: GPL-2.0-only
# Read-write mount (M3a T4). Included from Makefile.
#   make test-wdata          native
#   make test-wdata-san      ASan+UBSan (OUT is separate)
#   make test-wdata-tsan     ThreadSanitizer
# Needs mkfs.xfs, xfs_repair, xfs_db and xfs_logprint; SKIPPED visibly otherwise.
WDATA_TEST = $(OUT)/test_wdata$(EXE)
WDATA_TMP ?= $(abspath $(shell git rev-parse --git-common-dir 2>/dev/null)/../target/tmp)
$(WDATA_TEST): tests/test_wdata.c $(CORE_OBJ) $(SHIM_OBJ) $(BACKEND_OBJ) $(PENDING_LIB) mount.h wfs_xfs.h Makefile mk/wdata.mk | $(OUT)
	$(CC) $(CFLAGS) $(LIBXFS_WNO) $(SANFLAGS) $(INC) $(KINC) -I. -o $@ tests/test_wdata.c \
		$(CORE_OBJ) $(SHIM_OBJ) $(BACKEND_OBJ) $(PENDING_LIB) $(LIBS)

test-wdata:
ifneq ($(CROSS),)
	@echo "SKIPPED: test-wdata (cross build)"
else ifeq ($(HAVE_KERNEL),)
	@echo "SKIPPED: test-wdata (set KERNEL_XFS_SRC to a tree with libxfs/)"
else ifeq ($(shell command -v mkfs.xfs && command -v xfs_repair && command -v xfs_db && command -v xfs_logprint),)
	@echo "SKIPPED: test-wdata (needs mkfs.xfs, xfs_repair, xfs_db, xfs_logprint: install xfsprogs)"
else
	$(MAKE) --no-print-directory $(WDATA_TEST)
	@mkdir -p $(WDATA_TMP)
	$(WDATA_TEST) $(WDATA_TMP)
endif

test-wdata-san:
	$(MAKE) --no-print-directory test-wdata SAN=1 OUT=$(OUT)-san

# xfsaild, the CIL push work and log I/O completion run on their own threads
test-wdata-tsan:
	$(MAKE) --no-print-directory test-wdata SAN=1 SANFLAGS='-fsanitize=thread -fno-omit-frame-pointer' \
		OUT=$(OUT)-tsan

.PHONY: test-wdata test-wdata-san test-wdata-tsan
