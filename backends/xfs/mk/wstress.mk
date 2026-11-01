# SPDX-License-Identifier: GPL-2.0-only
# Parallel write stress (M3a T11). Included from Makefile.
#   make test-wstress          native
#   make test-wstress-san      ASan+UBSan (OUT is separate)
#   make test-wstress-tsan     ThreadSanitizer
# Needs mkfs.xfs, xfs_repair, xfs_db and xfs_logprint; SKIPPED visibly otherwise.
WSTRESS_TEST = $(OUT)/test_wstress$(EXE)
WSTRESS_TMP ?= $(abspath $(shell git rev-parse --git-common-dir 2>/dev/null)/../target/tmp)
$(WSTRESS_TEST): tests/test_wstress.c $(CORE_OBJ) $(SHIM_OBJ) $(BACKEND_OBJ) $(PENDING_LIB) mount.h wfs_xfs.h Makefile mk/wstress.mk | $(OUT)
	$(CC) $(CFLAGS) $(LIBXFS_WNO) $(SANFLAGS) $(INC) $(KINC) -I. -o $@ tests/test_wstress.c \
		$(CORE_OBJ) $(SHIM_OBJ) $(BACKEND_OBJ) $(PENDING_LIB) $(LIBS)

test-wstress:
ifneq ($(CROSS),)
	@echo "SKIPPED: test-wstress (cross build)"
else ifeq ($(HAVE_KERNEL),)
	@echo "SKIPPED: test-wstress (set KERNEL_XFS_SRC to a tree with libxfs/)"
else ifeq ($(shell command -v mkfs.xfs && command -v xfs_repair && command -v xfs_db && command -v xfs_logprint),)
	@echo "SKIPPED: test-wstress (needs mkfs.xfs, xfs_repair, xfs_db, xfs_logprint: install xfsprogs)"
else
	$(MAKE) --no-print-directory $(WSTRESS_TEST)
	@mkdir -p $(WSTRESS_TMP)
	$(WSTRESS_TEST) $(WSTRESS_TMP)
endif

test-wstress-san:
	$(MAKE) --no-print-directory test-wstress SAN=1 OUT=$(OUT)-san

# xfsaild, the CIL push work and log I/O completion run on their own threads
test-wstress-tsan:
	TSAN_OPTIONS="suppressions=$(abspath tests/tsan.supp)" $(MAKE) --no-print-directory test-wstress SAN=1 SANFLAGS='-fsanitize=thread -fno-omit-frame-pointer' \
		OUT=$(OUT)-tsan

.PHONY: test-wstress test-wstress-san test-wstress-tsan

# Host write performance baseline: make bench-write [IMG=...] [OPTS=...]  (default: a fresh 2 GiB image)
BENCH_WRITE = $(OUT)/bench_write$(EXE)
$(BENCH_WRITE): tests/bench_write.c $(CORE_OBJ) $(SHIM_OBJ) $(BACKEND_OBJ) $(PENDING_LIB) wfs_xfs.h Makefile mk/wstress.mk | $(OUT)
	$(CC) $(CFLAGS) $(LIBXFS_WNO) $(SANFLAGS) $(INC) $(KINC) -I. -o $@ tests/bench_write.c \
		$(CORE_OBJ) $(SHIM_OBJ) $(BACKEND_OBJ) $(PENDING_LIB) $(LIBS)
bench-write: $(BENCH_WRITE)
	@img=$(if $(IMG),$(IMG),$(WSTRESS_TMP)/bench_write.img); \
	if [ -z "$(IMG)" ]; then mkdir -p $(WSTRESS_TMP); rm -f $$img; truncate -s 2g $$img && \
	  mkfs.xfs -q -f -m crc=1,rmapbt=0,reflink=0,metadir=0 -n parent=0 -i sparse=1 $$img; fi; \
	$(BENCH_WRITE) $$img "$(OPTS)"; r=$$?; [ -n "$(IMG)" ] || rm -f $$img; exit $$r
.PHONY: bench-write
