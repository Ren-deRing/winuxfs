# Builds the three broken test plugins into $(OUT).
# Usage: make [CC=x86_64-w64-mingw32-gcc EXT=dll] [OUT=dir]
CC ?= cc
EXT ?= so
OUT ?= ../../target/test-plugins
CFLAGS ?= -O2 -Wall -Wextra -Werror
INC = -I../../include
PIC = $(if $(filter dll,$(EXT)),,-fPIC)
HDR = ../../include/winuxfs/backend.h ../../include/winuxfs/errno.h

ALL = $(OUT)/winuxfs_backend_bad_abi.$(EXT) \
      $(OUT)/winuxfs_backend_bad_noops.$(EXT) \
      $(OUT)/winuxfs_backend_bad_noentry.$(EXT)

all: $(ALL)

$(OUT):
	mkdir -p $@

$(OUT)/winuxfs_backend_bad_abi.$(EXT): bad.c $(HDR) | $(OUT)
	$(CC) $(CFLAGS) $(INC) $(PIC) -shared -DBAD_ABI -o $@ bad.c

$(OUT)/winuxfs_backend_bad_noops.$(EXT): bad.c $(HDR) | $(OUT)
	$(CC) $(CFLAGS) $(INC) $(PIC) -shared -DBAD_NOOPS -o $@ bad.c

$(OUT)/winuxfs_backend_bad_noentry.$(EXT): bad.c $(HDR) | $(OUT)
	$(CC) $(CFLAGS) $(INC) $(PIC) -shared -DBAD_NOENTRY -o $@ bad.c

clean:
	rm -f $(ALL)

.PHONY: all clean
