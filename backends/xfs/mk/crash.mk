# SPDX-License-Identifier: GPL-2.0-only
# Read-write mount (M3a T4). Included from Makefile.
#   make test-crash          native
#   make test-crash-san      ASan+UBSan (OUT is separate)
#   make test-crash-tsan     ThreadSanitizer
# Needs mkfs.xfs, xfs_repair, xfs_db and xfs_logprint; SKIPPED visibly otherwise.
CRASH_TEST = $(OUT)/test_crash$(EXE)
CRASH_TMP ?= $(abspath $(shell git rev-parse --git-common-dir 2>/dev/null)/../target/tmp)
$(CRASH_TEST): tests/test_crash.c $(CORE_OBJ) $(SHIM_OBJ) $(BACKEND_OBJ) $(PENDING_LIB) mount.h wfs_xfs.h Makefile mk/crash.mk | $(OUT)
	$(CC) $(CFLAGS) $(LIBXFS_WNO) $(SANFLAGS) $(INC) $(KINC) -I. -o $@ tests/test_crash.c \
		$(CORE_OBJ) $(SHIM_OBJ) $(BACKEND_OBJ) $(PENDING_LIB) $(LIBS)

test-crash:
ifneq ($(CROSS),)
	@echo "SKIPPED: test-crash (cross build)"
else ifeq ($(HAVE_KERNEL),)
	@echo "SKIPPED: test-crash (set KERNEL_XFS_SRC to a tree with libxfs/)"
else ifeq ($(shell command -v mkfs.xfs && command -v xfs_repair && command -v xfs_db && command -v xfs_logprint),)
	@echo "SKIPPED: test-crash (needs mkfs.xfs, xfs_repair, xfs_db, xfs_logprint: install xfsprogs)"
else
	$(MAKE) --no-print-directory $(CRASH_TEST)
	@mkdir -p $(CRASH_TMP)
	$(CRASH_TEST) $(CRASH_TMP)
endif

test-crash-san:
	$(MAKE) --no-print-directory test-crash SAN=1 OUT=$(OUT)-san

# xfsaild, the CIL push work and log I/O completion run on their own threads
test-crash-tsan:
	$(MAKE) --no-print-directory test-crash SAN=1 SANFLAGS='-fsanitize=thread -fno-omit-frame-pointer' \
		OUT=$(OUT)-tsan

.PHONY: test-crash test-crash-san test-crash-tsan
