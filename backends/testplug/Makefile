# Builds the tiny test backend (and its single-threaded variant) and the
# counting memfs variant (memfs_counting.c) into $(OUT).
# Usage: make [CC=x86_64-w64-mingw32-gcc EXT=dll] [OUT=dir]
CC ?= cc
EXT ?= so
OUT ?= ../../target/test-plugins
CFLAGS ?= -O2 -Wall -Wextra -Werror -std=c11
INC = -I../../include
PIC = $(if $(filter dll,$(EXT)),,-fPIC)
HDR = ../../include/winuxfs/backend.h ../../include/winuxfs/errno.h
# memfs needs pthreads (winpthreads linked statically into a DLL).
MEMFS_LIBS = $(if $(filter dll,$(EXT)),-static -lpthread,-pthread)

ALL = $(OUT)/winuxfs_backend_testplug.$(EXT) $(OUT)/winuxfs_backend_testplug_st.$(EXT) \
      $(OUT)/winuxfs_backend_memfs_counting.$(EXT)

all: $(ALL)

$(OUT):
	mkdir -p $@

$(OUT)/winuxfs_backend_testplug.$(EXT): testplug.c $(HDR) | $(OUT)
	$(CC) $(CFLAGS) $(INC) $(PIC) -shared -o $@ testplug.c

$(OUT)/winuxfs_backend_testplug_st.$(EXT): testplug.c $(HDR) | $(OUT)
	$(CC) $(CFLAGS) $(INC) $(PIC) -shared -DTESTPLUG_ST -o $@ testplug.c

$(OUT)/winuxfs_backend_memfs_counting.$(EXT): memfs_counting.c ../memfs/memfs.c $(HDR) | $(OUT)
	$(CC) $(CFLAGS) $(INC) $(PIC) -shared -o $@ memfs_counting.c $(MEMFS_LIBS)

clean:
	rm -f $(ALL)

.PHONY: all clean
