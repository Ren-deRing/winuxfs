# SPDX-License-Identifier: GPL-2.0-only
# Kernel lib/ code (M3a T2): rbtree, from third_party/linux-lib. Included from Makefile.
# The headers are copied to $(OUT)/gen/linux-lib and patched there
# (third_party/linux-lib-patches: LLP64), then found through KINC as
# <linux/rbtree.h>. lib/rbtree.c itself is unpatched.
LINUXLIB = ../../third_party/linux-lib
LINUXLIB_PATCHES = $(sort $(wildcard ../../third_party/linux-lib-patches/*.patch))
LINUXLIB_GEN = $(OUT)/gen/linux-lib
$(LINUXLIB_STAMP): $(wildcard $(LINUXLIB)/include/linux/*.h) $(LINUXLIB_PATCHES)
	@rm -rf $(LINUXLIB_GEN) && mkdir -p $(LINUXLIB_GEN)
	cp -r $(LINUXLIB)/include $(LINUXLIB_GEN)/ && chmod -R u+w $(LINUXLIB_GEN)
	for p in $(abspath $(LINUXLIB_PATCHES)); do patch -s -d $(LINUXLIB_GEN) -p1 --no-backup-if-mismatch < $$p || exit 1; done
	touch $@
$(OUT)/lib/%.o: $(LINUXLIB)/lib/%.c $(SHIM_HDR) Makefile
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(LIBXFS_WNO) $(SANFLAGS) $(PIC) $(VIS) $(INC) $(KINC) -c -o $@ $<

RBTEST = $(OUT)/test_rbtree$(EXE)
$(RBTEST): tests/test_rbtree.c $(OUT)/lib/rbtree.o $(SHIM_OBJ) $(SHIM_HDR) Makefile
	$(CC) $(CFLAGS) $(LIBXFS_WNO) $(SANFLAGS) $(INC) $(KINC) -o $@ tests/test_rbtree.c $(OUT)/lib/rbtree.o $(SHIM_OBJ) $(LIBS)
test-rbtree: $(RBTEST)
ifeq ($(CROSS),)
	$(RBTEST)
else
	@echo "cross build: rbtree test compiled, not run (no Wine)"
endif
.PHONY: test-rbtree
