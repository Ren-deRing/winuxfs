# SPDX-License-Identifier: GPL-2.0-only
# Buffer cache (Task 5): make test-buf / tsan-buf. Included from Makefile.
BUF_TEST = $(OUT)/test_buf$(EXE)
BUF_TSAN = $(OUT)/test_buf_tsan$(EXE)
BUF_INC = $(LIBXFS_WNO) $(INC) -I. -I$(KERNEL_XFS_SRC)/libxfs -I$(KERNEL_XFS_SRC)
BUF_SRC = buf.c tests/test_buf.c $(SHIM_SRC)
BUF_DEPS = $(BUF_SRC) blk.h $(SHIM_HDR) Makefile mk/buf.mk

$(BUF_TEST): $(BUF_DEPS) | $(OUT)
	$(CC) $(CFLAGS) $(SANFLAGS) $(BUF_INC) -o $@ $(BUF_SRC) $(LIBS)

test-buf: $(if $(HAVE_KERNEL),$(BUF_TEST))
ifeq ($(HAVE_KERNEL),)
	@echo "SKIPPED: test-buf (set KERNEL_XFS_SRC to a tree with libxfs/)"
else ifeq ($(CROSS),)
	$(BUF_TEST)
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
