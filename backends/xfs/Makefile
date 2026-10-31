# SPDX-License-Identifier: GPL-2.0-only
# XFS backend build (shim base; Task 4 adds libxfs and the plugin).
#   make test-shim                      build and run the shim tests (native)
#   make CROSS=x86_64-w64-mingw32- test-shim-build
#                                       compile-only cross build (no Wine here)
#   make test-shim SAN=1                ASan+UBSan
# KERNEL_XFS_SRC: tree holding libxfs/ (vendored: ../../third_party/linux-xfs,
# or a pinned kernel checkout's fs/xfs). The on-disk struct check test is
# compiled only when it exists, and SKIPPED visibly otherwise.
# Parallel by default (the libxfs objects are independent); JOBS=1 forces serial.
JOBS ?= $(shell nproc 2>/dev/null || echo 4)
MAKEFLAGS += -j$(JOBS)
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
LINUXLIB_STAMP = $(OUT)/gen/linux-lib/.patched
SHIM_HDR = $(wildcard shim/*.h shim/linux/*.h shim/*.inc) ../../include/winuxfs/errno.h $(LINUXLIB_STAMP)
LIBS = $(if $(CROSS),,-pthread)

TEST = $(OUT)/test_shim$(EXE)
PROBE = $(OUT)/ondisk_probe.o
HAVE_KERNEL = $(and $(KERNEL_XFS_SRC),$(wildcard $(KERNEL_XFS_SRC)/libxfs/xfs_ondisk.h))
KINC = -I$(KERNEL_XFS_SRC)/libxfs -I$(KERNEL_XFS_SRC) -I$(OUT)/gen/linux-lib/include

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

# ---- Task 4: libxfs read path objects and the plugin ----
# The 29 read-path libxfs files (design 5.1), compiled with kernel-code warning
# relaxations, -ffunction-sections/-fdata-sections (in CFLAGS) so the linker
# can --gc-sections the write path away.
LIBXFS_NAMES = xfs_sb xfs_ag xfs_group xfs_types xfs_bit xfs_ialloc xfs_ialloc_btree \
	xfs_alloc xfs_alloc_btree xfs_btree xfs_bmap xfs_bmap_btree xfs_iext_tree \
	xfs_inode_buf xfs_inode_fork xfs_inode_util xfs_da_btree xfs_dir2 xfs_dir2_block \
	xfs_dir2_data xfs_dir2_leaf xfs_dir2_node xfs_dir2_sf xfs_symlink_remote \
	xfs_attr_leaf xfs_rtgroup xfs_metafile xfs_rmap_btree xfs_refcount_btree
LIBXFS_OBJ = $(addprefix $(OUT)/libxfs/,$(addsuffix .o,$(LIBXFS_NAMES)))
# Backend code (entry, stubs, later tasks' files); mk/*.mk fragments append to
# BACKEND_SRC (prerequisite lists expand at parse time, so add files here, not in mk/). Weak PENDING stubs live in stubs_pending.c.
BACKEND_SRC += entry_stub.c stubs_ro.c buf.c bio.c logcheck.c mount.c options.c icache.c inomap.c statfs.c read.c dir.c xfs_dir2_readdir.c
BACKEND_OBJ = $(addprefix $(OUT)/be/,$(BACKEND_SRC:.c=.o))
# PENDING stubs: one object per stub in an archive that is searched last, so
# a real definition in any directly linked object replaces the stub.
PENDING_SYMS = $(sort $(shell grep -o 'WFS_ONLY_[A-Za-z0-9_]*' stubs_pending.c | sed 's/^WFS_ONLY_//'))
PENDING_OBJ = $(addprefix $(OUT)/pending/,$(addsuffix .o,$(PENDING_SYMS)))
PENDING_LIB = $(OUT)/libxfs_pending.a
SHIM_OBJ = $(patsubst shim/%.c,$(OUT)/shim/%.o,$(SHIM_SRC))
# Kernel lib/ objects (mk/linuxlib.mk).
LIB_OBJ = $(OUT)/lib/rbtree.o
EXT = $(if $(CROSS),dll,so)
PLUGIN_DIR ?= ../../target/plugins$(if $(CROSS),-win,)
PLUGIN = $(PLUGIN_DIR)/winuxfs_backend_xfs.$(EXT)
PIC = $(if $(CROSS),,-fPIC)
VIS = -fvisibility=hidden
# Hidden visibility everywhere; only wfs_backend_entry is exported
# (WFS_EXPORT in entry_stub.c).
# -fno-sanitize=alignment: kernel on-disk unions (xfs_btree_ptr __be64 inside
# 4-byte aligned blocks) are accessed unaligned by design on x86/ARM64.
$(OUT)/libxfs/%.o: $(KERNEL_XFS_SRC)/libxfs/%.c $(SHIM_HDR) Makefile
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(LIBXFS_WNO) $(SANFLAGS) $(if $(SAN),-fno-sanitize=alignment) $(PIC) $(VIS) $(INC) $(KINC) -c -o $@ $<
$(OUT)/shim/%.o: shim/%.c $(SHIM_HDR) Makefile
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(SANFLAGS) $(PIC) $(VIS) $(INC) -c -o $@ $<
# stubs include kernel headers: kernel-code warning relaxations apply
$(OUT)/be/stubs_%.o: LIBXFS_BE_WNO = $(LIBXFS_WNO)
$(OUT)/be/%.o: %.c $(SHIM_HDR) Makefile
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(LIBXFS_BE_WNO) $(SANFLAGS) $(PIC) $(VIS) $(INC) $(KINC) -c -o $@ $<

$(OUT)/pending/%.o: stubs_pending.c stubs.h $(SHIM_HDR) Makefile
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(LIBXFS_WNO) $(SANFLAGS) $(PIC) $(VIS) $(INC) $(KINC) -DWFS_ONLY_$* -c -o $@ $<
$(PENDING_LIB): $(PENDING_OBJ)
	rm -f $@
	$(CROSS)ar rcs $@ $^

libxfs-objs: $(LIBXFS_OBJ)

$(PLUGIN): $(LIBXFS_OBJ) $(SHIM_OBJ) $(LIB_OBJ) $(BACKEND_OBJ) $(PENDING_LIB)
	@mkdir -p $(PLUGIN_DIR)
	$(CC) $(SANFLAGS) -shared -Wl,--gc-sections -Wl,--no-undefined -o $@ $^ $(if $(CROSS),-static -lpthread,-pthread)

plugin: $(PLUGIN)

# Every libxfs section kept (no gc): the linker must resolve ALL symbols of the
# 29 files, not only those reachable from today's entry point, so later tasks
# never meet a new unresolved symbol. Not shipped.
FULL = $(OUT)/full_check.$(EXT)
$(FULL): $(LIBXFS_OBJ) $(SHIM_OBJ) $(LIB_OBJ) $(BACKEND_OBJ) $(PENDING_LIB)
	$(CC) $(SANFLAGS) -shared -Wl,--no-gc-sections -Wl,--no-undefined -o $@ $^ $(if $(CROSS),-static -lpthread,-pthread)

link-full: $(FULL)

# Stub behaviour test (native only): stubs return errors, log, never abort.
STUBTEST = $(OUT)/test_stubs$(EXE)
$(STUBTEST): tests/test_stubs.c stubs_ro.c stubs_pending.c stubs.h $(SHIM_SRC) $(SHIM_HDR) Makefile | $(OUT)
	$(CC) $(CFLAGS) $(LIBXFS_WNO) $(SANFLAGS) -DWFS_ASSERT_FATAL -DWFS_PENDING_ALL $(INC) $(KINC) -o $@ \
		tests/test_stubs.c stubs_ro.c stubs_pending.c $(SHIM_SRC) $(LIBS)

test-stubs: $(STUBTEST)
ifeq ($(CROSS),)
	$(STUBTEST)
else
	@echo "cross build: stub test compiled, not run (no Wine)"
endif

clean-objs:
	rm -rf $(OUT)/libxfs

test-link:
	sh tests/test_link.sh

clean:
	rm -rf ../../target/xfs-shim ../../target/xfs-shim-win ../../target/plugins/winuxfs_backend_xfs.so ../../target/plugins-win/winuxfs_backend_xfs.dll

.PHONY: all clean probe-libxfs test-shim test-shim-build probe libxfs-objs plugin link-full clean-objs test-link test-stubs

-include $(wildcard $(dir $(lastword $(MAKEFILE_LIST)))mk/*.mk)
