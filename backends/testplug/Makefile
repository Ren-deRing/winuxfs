# Builds the tiny test backend (and its single-threaded variant) into $(OUT).
# Usage: make [CC=x86_64-w64-mingw32-gcc EXT=dll] [OUT=dir]
CC ?= cc
EXT ?= so
OUT ?= ../../target/test-plugins
CFLAGS ?= -O2 -Wall -Wextra -Werror -std=c11
INC = -I../../include
PIC = $(if $(filter dll,$(EXT)),,-fPIC)
HDR = ../../include/winuxfs/backend.h

ALL = $(OUT)/winuxfs_backend_testplug.$(EXT) $(OUT)/winuxfs_backend_testplug_st.$(EXT)

all: $(ALL)

$(OUT):
	mkdir -p $@

$(OUT)/winuxfs_backend_testplug.$(EXT): testplug.c $(HDR) | $(OUT)
	$(CC) $(CFLAGS) $(INC) $(PIC) -shared -o $@ testplug.c

$(OUT)/winuxfs_backend_testplug_st.$(EXT): testplug.c $(HDR) | $(OUT)
	$(CC) $(CFLAGS) $(INC) $(PIC) -shared -DTESTPLUG_ST -o $@ testplug.c

clean:
	rm -f $(ALL)

.PHONY: all clean
