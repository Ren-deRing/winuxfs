# SPDX-License-Identifier: GPL-2.0-only
# The write suites on rmapbt+reflink images (M3d). Included from Makefile.
#   make test-wrmap          every write suite with WFEAT = rmapbt=1,reflink=1 (native)
#   make test-wrmap-san      the same under ASan+UBSan
# WFEAT is read by the tests' PLAIN macro (tests/test_w*.c, test_crash.c, test_recover.c).
WRMAP_FEAT ?= -m crc=1,rmapbt=1,reflink=1,metadir=0 -n parent=0 -i sparse=1
WRMAP_SUITES = wops wdata wlink recover crash wstress

test-wrmap:
	@for t in $(WRMAP_SUITES); do \
		WFEAT='$(WRMAP_FEAT)' $(MAKE) --no-print-directory test-$$t || exit 1; \
	done

test-wrmap-san:
	@for t in $(WRMAP_SUITES); do \
		WFEAT='$(WRMAP_FEAT)' $(MAKE) --no-print-directory test-$$t-san || exit 1; \
	done

.PHONY: test-wrmap test-wrmap-san

# The same suites on parent-pointer images (the newest mkfs.xfs default: parent=1 with rmapbt/reflink).
#   make test-wpptr          native;  make test-wpptr-san  ASan+UBSan
WPPTR_FEAT ?= -m crc=1,rmapbt=1,reflink=1,metadir=0 -n parent=1 -i sparse=1
test-wpptr:
	@for t in $(WRMAP_SUITES); do \
		WFEAT='$(WPPTR_FEAT)' $(MAKE) --no-print-directory test-$$t || exit 1; \
	done

test-wpptr-san:
	@for t in $(WRMAP_SUITES); do \
		WFEAT='$(WPPTR_FEAT)' $(MAKE) --no-print-directory test-$$t-san || exit 1; \
	done

.PHONY: test-wpptr test-wpptr-san
