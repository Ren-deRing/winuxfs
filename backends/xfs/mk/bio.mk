# SPDX-License-Identifier: GPL-2.0-only
# struct bio on the host block layer (M3a T2): make test-bio [SAN=1]. Included from Makefile.
$(OUT)/be/bio.o: LIBXFS_BE_WNO = $(LIBXFS_WNO)
BIO_TEST = $(OUT)/test_bio$(EXE)
$(BIO_TEST): tests/test_bio.c bio.c blk.h $(SHIM_OBJ) $(SHIM_HDR) Makefile mk/bio.mk
	$(CC) $(CFLAGS) $(LIBXFS_WNO) $(SANFLAGS) $(INC) $(KINC) -I. -o $@ tests/test_bio.c bio.c $(SHIM_OBJ) $(LIBS)
test-bio: $(BIO_TEST)
ifeq ($(CROSS),)
	$(BIO_TEST)
else
	@echo "cross build: bio test compiled, not run (no Wine)"
endif
.PHONY: test-bio
