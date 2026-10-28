# SPDX-License-Identifier: GPL-2.0-only
# XFS backend build (shim base; Task 4 adds libxfs and the plugin).
#   make test-shim                      build and run the shim tests (native)
#   make CROSS=x86_64-w64-mingw32- test-shim-build
#                                       compile-only cross build (no Wine here)
#   make test-shim SAN=1                ASan+UBSan
# KERNEL_XFS_SRC: tree holding libxfs/ (vendored: ../../third_party/linux-xfs,
# or a pinned kernel checkout's fs/xfs). The on-disk struct check test is
# compiled only when it exists, and SKIPPED visibly otherwise.
CROSS ?=
CC = $(CROSS)gcc
OUT ?= ../../target/xfs-shim$(if $(CROSS),-win,)
EXE = $(if $(CROSS),.exe,)
CFLAGS ?= -O2 -g -std=gnu11 -Wall -Wextra -Werror -ffunction-sections -fdata-sections
# Vendored kernel code is not -Wextra clean (the kernel never builds with it):
# shim code and tests keep the strict flags, kernel-derived objects add these.
LIBXFS_WNO = -Wno-sign-compare -Wno-unused-parameter -Wno-missing-field-initializers \
	-Wno-type-limits -Wno-implicit-fallthrough -Wno-empty-body -Wno-override-init -Wno-parentheses -Wno-address-of-packed-member
comma = ,
SANFLAGS = $(if $(SAN),-fsanitize=address$(comma)undefined -fno-sanitize-recover=all -fno-omit-frame-pointer)
KERNEL_XFS_SRC ?= $(firstword $(wildcard ../../third_party/linux-xfs))
INC = -Ishim -I../../include
SHIM_SRC = $(wildcard shim/*.c)
SHIM_HDR = $(wildcard shim/*.h shim/*.inc) ../../include/winuxfs/errno.h
LIBS = $(if $(CROSS),,-pthread)

TEST = $(OUT)/test_shim$(EXE)
PROBE = $(OUT)/ondisk_probe.o
HAVE_KERNEL = $(and $(KERNEL_XFS_SRC),$(wildcard $(KERNEL_XFS_SRC)/libxfs/xfs_ondisk.h))
KINC = -I$(KERNEL_XFS_SRC)/libxfs -I$(KERNEL_XFS_SRC)

all: test-shim-build

$(OUT):
	mkdir -p $@

$(TEST): tests/test_shim.c $(SHIM_SRC) $(SHIM_HDR) Makefile | $(OUT)
	$(CC) $(CFLAGS) $(SANFLAGS) $(INC) -o $@ tests/test_shim.c $(SHIM_SRC) $(LIBS)

# On-disk struct sizes/offsets, checked at compile time against the real
# xfs_ondisk.h (compile only; the object is never linked).
$(PROBE): tests/ondisk_probe.c $(SHIM_HDR) Makefile | $(OUT)
	$(CC) $(CFLAGS) $(LIBXFS_WNO) $(INC) $(KINC) -c -o $@ tests/ondisk_probe.c

probe:
ifneq ($(HAVE_KERNEL),)
	$(MAKE) --no-print-directory $(PROBE) KERNEL_XFS_SRC=$(KERNEL_XFS_SRC)
	@echo "xfs_check_ondisk_structs: static asserts OK ($(if $(CROSS),mingw,native))"
else
	@echo "SKIPPED: xfs_check_ondisk_structs (set KERNEL_XFS_SRC to a tree with libxfs/)"
endif

test-shim-build: $(TEST) probe

test-shim: $(TEST) probe
ifeq ($(CROSS),)
	$(TEST)
else
	@echo "cross build: tests compiled, not run (no Wine); run in the VM"
endif

# Progress toward Task 4: -fsyntax-only on each vendored read-path libxfs .c
# against the shim (shim dir first so it shadows xfs_mount.h etc.). Reports
# pass/fail per file and totals; never fails the make (it is a gauge).
LIBXFS_C = $(sort $(wildcard $(KERNEL_XFS_SRC)/libxfs/*.c))
probe-libxfs:
	@pass=0; fail=0; \
	for f in $(LIBXFS_C); do \
	  if $(CC) $(CFLAGS) $(LIBXFS_WNO) -fsyntax-only $(INC) $(KINC) $$f 2>../../target/probe.err; then \
	    pass=$$((pass+1)); echo "PASS $$(basename $$f)"; \
	  else fail=$$((fail+1)); echo "FAIL $$(basename $$f): $$(grep -m1 -E 'error' ../../target/probe.err)"; fi; \
	done; echo "probe-libxfs: $$pass pass, $$fail fail of $$((pass+fail))"

clean:
	rm -rf ../../target/xfs-shim ../../target/xfs-shim-win

.PHONY: all clean probe-libxfs test-shim test-shim-build probe

-include $(wildcard $(dir $(lastword $(MAKEFILE_LIST)))mk/*.mk)
