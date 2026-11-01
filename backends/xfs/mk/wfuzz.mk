# SPDX-License-Identifier: GPL-2.0-only
# Fuzzing after writes (M3a T11). Included from Makefile.
#   make test-wfuzz          native
#   make test-wfuzz-san      ASan+UBSan (WFUZZ_ITERS, WFUZZ_SEED, WFUZZ_ONLY)
#   make test-wfuzz-nocrc    ASan+UBSan with checksum verification skipped
#   make test-wfuzz-all      both
# Needs mkfs.xfs, xfs_repair, xfs_db and xfs_logprint; SKIPPED visibly otherwise.
WFUZZ_TEST = $(OUT)/test_wfuzz$(EXE)
WFUZZ_TMP ?= $(abspath $(shell git rev-parse --git-common-dir 2>/dev/null)/../target/tmp)
$(WFUZZ_TEST): tests/test_wfuzz.c $(CORE_OBJ) $(SHIM_OBJ) $(BACKEND_OBJ) $(PENDING_LIB) mount.h wfs_xfs.h Makefile mk/wfuzz.mk | $(OUT)
	$(CC) $(CFLAGS) $(LIBXFS_WNO) $(SANFLAGS) $(INC) $(KINC) -I. -o $@ tests/test_wfuzz.c \
		$(CORE_OBJ) $(SHIM_OBJ) $(BACKEND_OBJ) $(PENDING_LIB) $(LIBS)

test-wfuzz:
ifneq ($(CROSS),)
	@echo "SKIPPED: test-wfuzz (cross build)"
else ifeq ($(HAVE_KERNEL),)
	@echo "SKIPPED: test-wfuzz (set KERNEL_XFS_SRC to a tree with libxfs/)"
else ifeq ($(shell command -v mkfs.xfs && command -v xfs_repair && command -v xfs_db && command -v xfs_logprint),)
	@echo "SKIPPED: test-wfuzz (needs mkfs.xfs, xfs_repair, xfs_db, xfs_logprint: install xfsprogs)"
else
	$(MAKE) --no-print-directory $(WFUZZ_TEST)
	@mkdir -p $(WFUZZ_TMP)
	$(WFUZZ_TEST) $(WFUZZ_TMP)
endif

# ASan+UBSan, exact buffer sizes (no slack: over-reads are exact), as fuzz.mk builds
test-wfuzz-san:
	$(MAKE) --no-print-directory test-wfuzz SAN=1 FUZZ_BUILD=1 OUT=$(OUT)-wf

# the same with every checksum verification succeeding: mutations reach the structural checks
test-wfuzz-nocrc:
	$(MAKE) --no-print-directory test-wfuzz SAN=1 FUZZ_BUILD=1 FUZZ_NOCRC=1 OUT=$(OUT)-wf-nocrc

test-wfuzz-all: test-wfuzz-san test-wfuzz-nocrc

.PHONY: test-wfuzz test-wfuzz-san test-wfuzz-nocrc test-wfuzz-all
