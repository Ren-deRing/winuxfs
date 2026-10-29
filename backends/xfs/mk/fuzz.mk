# SPDX-License-Identifier: GPL-2.0-only
# Corruption tolerance (Task 13, design 10.6). Included from Makefile.
#   make test-fuzz          corpus (known bad inputs) + unmutated walk; ASan+UBSan
#   make fuzz-sample        seed-fixed 1% sample of all mutations, both CRC modes (PR gate)
#   make fuzz-full          every mutation, both CRC modes (nightly, tens of minutes)
#   make fuzz-libfuzzer     clang libFuzzer build (WFS_FUZZ_BASE=<base.wsi> build/fuzz_mount_walk DIR)
# Knobs: FUZZ_PCT (default 1), FUZZ_SEED, FUZZ_DUMP (directory for failing .wsd).
# Everything is built with ASan+UBSan into $(OUT)-fz (no zeroed buffer slack,
# so over-reads are exact) and $(OUT)-fz-nocrc (WFS_XFS_FUZZ_SKIP_CRC: every checksum
# verification succeeds, so mutations reach the structural checks).
# The base images and bad inputs are tests/xfs/corpus (tools/gen-fuzz-corpus.sh
# regenerates them; nothing here needs xfsprogs).
FUZZ_PCT ?= 1
FUZZ_SEED ?= 0x58465321
CORPUS ?= ../../tests/xfs/corpus
FUZZ_BASE ?= $(CORPUS)/base-small.wsi
FUZZ_DUMP ?=
FUZZ_JOBS ?= $(JOBS)
ifdef FUZZ_NOCRC
CFLAGS += -DWFS_XFS_FUZZ_SKIP_CRC
endif
ifdef FUZZ_BUILD
CFLAGS += -DWFS_BUF_SLACK=0
endif
MUTATE_RUNNER = $(OUT)/mutate_runner$(EXE)
FUZZ_SRC = fuzz/fuzz_walk.h
$(MUTATE_RUNNER): fuzz/mutate_runner.c $(FUZZ_SRC) $(LIBXFS_OBJ) $(SHIM_OBJ) $(BACKEND_OBJ) $(PENDING_LIB) Makefile mk/fuzz.mk | $(OUT)
	$(CC) $(CFLAGS) $(LIBXFS_WNO) -Wno-format-truncation $(SANFLAGS) $(INC) $(KINC) -I. -Ifuzz -o $@ fuzz/mutate_runner.c \
		$(LIBXFS_OBJ) $(SHIM_OBJ) $(BACKEND_OBJ) $(PENDING_LIB) $(LIBS)

FUZZ_NEEDS = $(if $(CROSS),cross build,$(if $(HAVE_KERNEL),,no kernel tree))

fuzz-run-test: $(MUTATE_RUNNER)
	$(MUTATE_RUNNER) --unmutated $(FUZZ_BASE)
	$(MUTATE_RUNNER) --corpus $(CORPUS)

fuzz-run-sample: $(MUTATE_RUNNER)
	$(if $(FUZZ_DUMP),mkdir -p $(FUZZ_DUMP))
	$(MUTATE_RUNNER) --sample $(FUZZ_PCT) --seed $(FUZZ_SEED) --fork --jobs $(FUZZ_JOBS) $(if $(FUZZ_DUMP),--dump-dir $(FUZZ_DUMP)) $(FUZZ_BASE)

test-fuzz:
ifneq ($(FUZZ_NEEDS),)
	@echo "SKIPPED: test-fuzz ($(FUZZ_NEEDS))"
else
	$(MAKE) --no-print-directory fuzz-run-test SAN=1 FUZZ_BUILD=1 OUT=$(OUT)-fz
endif

fuzz-sample:
ifneq ($(FUZZ_NEEDS),)
	@echo "SKIPPED: fuzz-sample ($(FUZZ_NEEDS))"
else
	$(MAKE) --no-print-directory fuzz-run-sample SAN=1 FUZZ_BUILD=1 OUT=$(OUT)-fz
	$(MAKE) --no-print-directory fuzz-run-sample SAN=1 FUZZ_NOCRC=1 FUZZ_BUILD=1 OUT=$(OUT)-fz-nocrc
endif

fuzz-full:
	$(MAKE) --no-print-directory fuzz-sample FUZZ_PCT=100

# libFuzzer needs clang for the runtime and for coverage instrumentation of
# the backend objects too.
FUZZER = $(OUT)/fuzz_mount_walk
$(FUZZER): fuzz/fuzz_mount_walk.c $(FUZZ_SRC) $(LIBXFS_OBJ) $(SHIM_OBJ) $(BACKEND_OBJ) $(PENDING_LIB) Makefile mk/fuzz.mk | $(OUT)
	$(CC) $(CFLAGS) $(LIBXFS_WNO) -Wno-format-truncation -fsanitize=fuzzer,address,undefined $(INC) $(KINC) -I. -Ifuzz -o $@ fuzz/fuzz_mount_walk.c \
		$(LIBXFS_OBJ) $(SHIM_OBJ) $(BACKEND_OBJ) $(PENDING_LIB) $(LIBS)

fuzz-build-libfuzzer: $(FUZZER)

fuzz-libfuzzer:
	$(MAKE) --no-print-directory fuzz-build-libfuzzer CC=clang FUZZ_NOCRC=1 LIBXFS_WNO='$(LIBXFS_WNO) -Wno-error' \
		SAN=1 FUZZ_BUILD=1 OUT=$(OUT)-fuzzer \
		SANFLAGS='-fsanitize=address,undefined,fuzzer-no-link -fno-sanitize-recover=all -fno-sanitize=alignment,pointer-overflow -fno-omit-frame-pointer'
	@# pointer-overflow: libxfs forms `hdr + 1` on a NULL if_data for a zero-length local attr fork
	@# (xfs_attr_sf.h:41, never dereferenced): a clang-only false positive that stops the first run.
	@echo "run: WFS_FUZZ_BASE=$(FUZZ_BASE) $(OUT)-fuzzer/fuzz_mount_walk <corpus-dir> -max_len=2048"

.PHONY: test-fuzz fuzz-sample fuzz-full fuzz-libfuzzer fuzz-run-test fuzz-run-sample fuzz-build-libfuzzer
