# SPDX-License-Identifier: GPL-2.0-only
# Dirty-log detection (Task 7).  Included from Makefile.
#   make test-logcheck            build and run (native); SKIPPED visibly if the
#                                 kernel tree or mkfs.xfs/xfs_logprint is missing
#   make test-logcheck SAN=1      ASan+UBSan
#   make CROSS=x86_64-w64-mingw32- test-logcheck-build   compile-only
# kernel-derived object: not -Wextra clean
$(OUT)/be/logcheck.o: LIBXFS_BE_WNO = $(LIBXFS_WNO)
LOGCHECK_TEST = $(OUT)/test_logcheck$(EXE)
LOGCHECK_SRC = logcheck.c tests/test_logcheck.c
LOGCHECK_FLAGS = -I. $(CFLAGS) $(LIBXFS_WNO) $(SANFLAGS) $(INC) $(KINC)

$(LOGCHECK_TEST): $(LOGCHECK_SRC) logcheck.h $(SHIM_SRC) $(SHIM_HDR) mk/logcheck.mk | $(OUT)
	$(CC) $(LOGCHECK_FLAGS) -o $@ $(LOGCHECK_SRC) $(SHIM_SRC) $(LIBS)

test-logcheck-build:
ifneq ($(HAVE_KERNEL),)
	$(MAKE) --no-print-directory $(LOGCHECK_TEST) KERNEL_XFS_SRC=$(KERNEL_XFS_SRC)
else
	@echo "SKIPPED: logcheck (set KERNEL_XFS_SRC to a tree with libxfs/)"
endif

test-logcheck: test-logcheck-build
ifneq ($(HAVE_KERNEL),)
ifeq ($(CROSS),)
	$(LOGCHECK_TEST)
else
	@echo "cross build: logcheck test compiled, not run (no Wine); run in the VM"
endif
endif

.PHONY: test-logcheck test-logcheck-build

# ASan+UBSan run (separate output dir so it never reuses the plain binary).
test-logcheck-san:
	$(MAKE) --no-print-directory test-logcheck SAN=1 OUT=$(OUT)-san
.PHONY: test-logcheck-san
