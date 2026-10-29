# SPDX-License-Identifier: GPL-2.0-only
# Concurrency + cache pressure (Task 14): make test-concurrent [SAN=1|TSAN=1].
# Included from Makefile; shares the fixture cache with mount.mk (FIXDIR).
#   make test-concurrent        native
#   make test-concurrent-san    ASan+UBSan
#   make test-concurrent-tsan   ThreadSanitizer (separate OUT; no UBSan)
CONC_TEST = $(OUT)/test_concurrent$(EXE)
# TSan replaces the ASan/UBSan flags of every object (SANFLAGS is recursive).
ifneq ($(TSAN),)
SANFLAGS = -fsanitize=thread -fno-omit-frame-pointer
endif
$(CONC_TEST): tests/test_concurrent.c $(LIBXFS_OBJ) $(SHIM_OBJ) $(BACKEND_OBJ) $(PENDING_LIB) icache.h ops.h wfs_xfs.h Makefile mk/concurrent.mk | $(OUT)
	$(CC) $(CFLAGS) $(LIBXFS_WNO) -Wno-format-truncation $(SANFLAGS) $(INC) $(KINC) -I. -o $@ tests/test_concurrent.c \
		$(LIBXFS_OBJ) $(SHIM_OBJ) $(BACKEND_OBJ) $(PENDING_LIB) $(LIBS)

test-concurrent:
ifneq ($(CROSS),)
	@echo "SKIPPED: test-concurrent (cross build)"
else ifeq ($(HAVE_KERNEL),)
	@echo "SKIPPED: test-concurrent (set KERNEL_XFS_SRC to a tree with libxfs/)"
else ifeq ($(shell command -v mkfs.xfs),)
	@echo "SKIPPED: test-concurrent (mkfs.xfs not installed: install xfsprogs)"
else ifeq ($(shell command -v xfs_db),)
	@echo "SKIPPED: test-concurrent (xfs_db not installed: install xfsprogs)"
else
	$(MAKE) --no-print-directory $(CONC_TEST) $(FIXDIR)/.done
	$(CONC_TEST) $(FIXDIR)
endif

test-concurrent-san:
	$(MAKE) --no-print-directory test-concurrent SAN=1 OUT=$(OUT)-san

# TSan needs every object instrumented: its own OUT, and the sanitizer flag
# goes into SANFLAGS so libxfs, shim and backend objects all get it.
# SKIPPED visibly when libtsan is not installed.
test-concurrent-tsan:
ifneq ($(CROSS),)
	@echo "SKIPPED: test-concurrent-tsan (cross build)"
else ifeq ($(shell echo 'int main(void){return 0;}' | $(CC) -x c -fsanitize=thread -o /dev/null - 2>/dev/null && echo ok),)
	@echo "SKIPPED: test-concurrent-tsan (libtsan not installed)"
else
	$(MAKE) --no-print-directory test-concurrent TSAN=1 OUT=$(OUT)-tsan
endif

.PHONY: test-concurrent test-concurrent-san test-concurrent-tsan
