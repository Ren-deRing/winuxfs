# SPDX-License-Identifier: GPL-2.0-only
# Buffer cache (Task 5): make test-buf / tsan-buf. Included from Makefile.
$(OUT)/be/buf.o: LIBXFS_BE_WNO = $(LIBXFS_WNO)
BUF_TEST = $(OUT)/test_buf$(EXE)
BUF_TSAN = $(OUT)/test_buf_tsan$(EXE)
BUF_INC = $(LIBXFS_WNO) $(INC) -I. $(KINC)
BUF_SRC = buf.c tests/test_buf.c $(SHIM_SRC)
BUF_DEPS = $(BUF_SRC) blk.h $(SHIM_HDR) Makefile mk/buf.mk

$(BUF_TEST): $(BUF_DEPS) | $(OUT)
	$(CC) $(CFLAGS) $(SANFLAGS) $(BUF_INC) -o $@ $(BUF_SRC) $(LIBS)

test-buf: $(if $(HAVE_KERNEL),$(BUF_TEST))
ifeq ($(HAVE_KERNEL),)
	@echo "SKIPPED: test-buf (set KERNEL_XFS_SRC to a tree with libxfs/)"
else ifeq ($(CROSS),)
	$(BUF_TEST)
	@$(MAKE) --no-print-directory test-buf-agf
else
	@echo "cross build: test-buf compiled, not run (no Wine); run in the VM"
endif

# ThreadSanitizer run; skipped visibly when libtsan is not installed.
tsan-buf: | $(OUT)
ifeq ($(CROSS),)
	@if echo 'int main(void){return 0;}' | $(CC) -x c -fsanitize=thread -o $(OUT)/tsan_probe - 2>/dev/null; then \
	  $(CC) $(CFLAGS) -fsanitize=thread $(BUF_INC) -o $(BUF_TSAN) $(BUF_SRC) $(LIBS) && $(BUF_TSAN); \
	else echo "SKIPPED: tsan-buf (libtsan not installed)"; fi
else
	@echo "SKIPPED: tsan-buf (cross build)"
endif

.PHONY: test-buf tsan-buf

# Real libxfs verifier (xfs_agf_buf_ops) against a mkfs.xfs image through the
# cache. Needs libxfs objects (as test-link) and mkfs.xfs; SKIPPED otherwise.
AGF_TEST = $(OUT)/test_buf_agf$(EXE)
AGF_IMG = $(OUT)/agf.img
$(AGF_TEST): tests/test_buf_agf.c $(CORE_OBJ) $(SHIM_OBJ) $(BACKEND_OBJ) $(PENDING_LIB) buf.h blk.h
	$(CC) $(CFLAGS) $(LIBXFS_WNO) $(SANFLAGS) $(BUF_INC) -o $@ tests/test_buf_agf.c \
		$(CORE_OBJ) $(SHIM_OBJ) $(BACKEND_OBJ) $(PENDING_LIB) $(LIBS)
$(AGF_IMG): | $(OUT)
	rm -f $@; truncate -s 300m $@
	mkfs.xfs -q -f -m crc=1 -b size=4096 -d agcount=2 $@

test-buf-agf:
ifneq ($(CROSS),)
	@echo "SKIPPED: test-buf-agf (cross build)"
else ifeq ($(HAVE_KERNEL),)
	@echo "SKIPPED: test-buf-agf (set KERNEL_XFS_SRC to a tree with libxfs/)"
else ifeq ($(shell command -v mkfs.xfs),)
	@echo "SKIPPED: test-buf-agf (mkfs.xfs not installed)"
else
	$(MAKE) --no-print-directory $(AGF_TEST) $(AGF_IMG)
	$(AGF_TEST) $(AGF_IMG)
endif

.PHONY: test-buf-agf
