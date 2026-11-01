# SPDX-License-Identifier: GPL-2.0-only
# Read-write mount (M3a T4). Included from Makefile.
#   make test-recover          native
#   make test-recover-san      ASan+UBSan (OUT is separate)
#   make test-recover-tsan     ThreadSanitizer
# Needs mkfs.xfs, xfs_repair, xfs_db and xfs_logprint; SKIPPED visibly otherwise.
RECOVER_TEST = $(OUT)/test_recover$(EXE)
RECOVER_TMP ?= $(abspath $(shell git rev-parse --git-common-dir 2>/dev/null)/../target/tmp)
$(RECOVER_TEST): tests/test_recover.c $(CORE_OBJ) $(SHIM_OBJ) $(BACKEND_OBJ) $(PENDING_LIB) mount.h wfs_xfs.h Makefile mk/recover.mk | $(OUT)
	$(CC) $(CFLAGS) $(LIBXFS_WNO) $(SANFLAGS) $(INC) $(KINC) -I. -o $@ tests/test_recover.c \
		$(CORE_OBJ) $(SHIM_OBJ) $(BACKEND_OBJ) $(PENDING_LIB) $(LIBS)

test-recover:
ifneq ($(CROSS),)
	@echo "SKIPPED: test-recover (cross build)"
else ifeq ($(HAVE_KERNEL),)
	@echo "SKIPPED: test-recover (set KERNEL_XFS_SRC to a tree with libxfs/)"
else ifeq ($(shell command -v mkfs.xfs && command -v xfs_repair && command -v xfs_db && command -v xfs_logprint),)
	@echo "SKIPPED: test-recover (needs mkfs.xfs, xfs_repair, xfs_db, xfs_logprint: install xfsprogs)"
else
	$(MAKE) --no-print-directory $(RECOVER_TEST)
	@mkdir -p $(RECOVER_TMP)
	$(RECOVER_TEST) $(RECOVER_TMP)
endif

test-recover-san:
	$(MAKE) --no-print-directory test-recover SAN=1 OUT=$(OUT)-san

# xfsaild, the CIL push work and log I/O completion run on their own threads
test-recover-tsan:
	$(MAKE) --no-print-directory test-recover SAN=1 SANFLAGS='-fsanitize=thread -fno-omit-frame-pointer' \
		OUT=$(OUT)-tsan

.PHONY: test-recover test-recover-san test-recover-tsan
