# SPDX-License-Identifier: GPL-2.0-only
# Inode cache + getattr (Task 8): make test-icache [SAN=1]. Included from Makefile.
$(OUT)/be/icache.o $(OUT)/be/inomap.o: LIBXFS_BE_WNO = $(LIBXFS_WNO)
ICACHE_TEST = $(OUT)/test_icache$(EXE)
$(ICACHE_TEST): tests/test_icache.c $(LIBXFS_OBJ) $(SHIM_OBJ) $(BACKEND_OBJ) $(PENDING_LIB) icache.h wfs_xfs.h Makefile mk/icache.mk | $(OUT)
	$(CC) $(CFLAGS) $(LIBXFS_WNO) $(SANFLAGS) $(INC) $(KINC) -I. -o $@ tests/test_icache.c \
		$(LIBXFS_OBJ) $(SHIM_OBJ) $(BACKEND_OBJ) $(PENDING_LIB) $(LIBS)

test-icache:
ifneq ($(CROSS),)
	@echo "SKIPPED: test-icache (cross build)"
else ifeq ($(HAVE_KERNEL),)
	@echo "SKIPPED: test-icache (set KERNEL_XFS_SRC to a tree with libxfs/)"
else ifeq ($(shell command -v mkfs.xfs),)
	@echo "SKIPPED: test-icache (mkfs.xfs not installed: install xfsprogs)"
else ifeq ($(shell command -v xfs_db),)
	@echo "SKIPPED: test-icache (xfs_db not installed: install xfsprogs)"
else
	$(MAKE) --no-print-directory $(ICACHE_TEST)
	$(ICACHE_TEST)
endif

test-icache-san:
	$(MAKE) --no-print-directory test-icache SAN=1 OUT=$(OUT)-san

.PHONY: test-icache test-icache-san
