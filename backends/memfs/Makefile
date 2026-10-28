# Builds the memfs test backend into $(OUT).
# Usage: make [CC=x86_64-w64-mingw32-gcc EXT=dll] [OUT=dir]
#        make selftest [SAN=1]   # standalone C self-test (Linux), runs it;
#                                # SAN=1 adds ASan+UBSan (self-test only)
CC ?= cc
EXT ?= so
OUT ?= ../../target/plugins
CFLAGS ?= -O2 -Wall -Wextra -Werror -std=c11
INC = -I../../include
HDR = ../../include/winuxfs/backend.h ../../include/winuxfs/errno.h
comma = ,
SANFLAGS = $(if $(SAN),-fsanitize=address$(comma)undefined -fno-sanitize-recover=all -fno-omit-frame-pointer -g)
ifeq ($(EXT),dll)
# Link libgcc and winpthreads statically: the DLL depends only on Windows DLLs.
PIC =
LIBS = -static -lpthread
else
PIC = -fPIC
LIBS = -pthread
endif

LIB = $(OUT)/winuxfs_backend_memfs.$(EXT)
SELFTEST = $(OUT)/memfs_selftest$(if $(SAN),_san,)

all: $(LIB)

$(OUT):
	mkdir -p $@

$(LIB): memfs.c $(HDR) Makefile | $(OUT)
	$(CC) $(CFLAGS) $(INC) $(PIC) -shared -o $@ memfs.c $(LIBS)

# The self-test includes memfs.c directly with a small capacity cap so that
# -ENOSPC is reachable.
$(SELFTEST): selftest.c memfs.c $(HDR) Makefile | $(OUT)
	$(CC) $(CFLAGS) $(SANFLAGS) $(INC) -DMEMFS_CAP_BYTES='(1ULL << 20)' -o $@ selftest.c $(LIBS)

selftest: $(SELFTEST)
	$(SELFTEST)

clean:
	rm -f $(LIB) $(OUT)/memfs_selftest $(OUT)/memfs_selftest_san

.PHONY: all clean selftest
