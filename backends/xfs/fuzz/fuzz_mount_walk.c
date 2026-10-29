// SPDX-License-Identifier: GPL-2.0-only
/*
 * libFuzzer harness (design 10.6): mount -> walk the whole tree -> unmount on
 * a base image with a few sectors overwritten by the fuzz input.
 *
 *   WFS_FUZZ_BASE=tests/xfs/corpus/base-small.wsi ./fuzz_mount_walk corpus/ -max_len=4096
 *
 * Input = records { u16 sector (index into the base's read set, mod count),
 * u16 offset (mod 512), u16 length (clamped to the sector), bytes }, at most
 * FZ_MAX_OVL of them. Only sectors the unmutated walk reads are mutated.
 * Any sanitizer report, hang, refused allocation, leak or contract violation
 * aborts.
 */
#include "fuzz_walk.h"

static struct fz_img base;

int LLVMFuzzerInitialize(int *argc, char ***argv)
{
	const char *p = getenv("WFS_FUZZ_BASE");

	(void)argc; (void)argv;
	if (!p || fz_load_wsi(&base, p) || base.n == 0) {
		fprintf(stderr, "set WFS_FUZZ_BASE to a base .wsi (tests/xfs/corpus/base-*.wsi)\n");
		exit(2);
	}
	return 0;
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
	struct fz_ovl ovl[FZ_MAX_OVL];
	int n = 0;
	struct fz_res r;

	while (size >= 6 && n < FZ_MAX_OVL) {
		uint16_t si = (uint16_t)(data[0] | data[1] << 8), off = (uint16_t)((data[2] | data[3] << 8) % FZ_SEC);
		size_t len = (size_t)(data[4] | data[5] << 8) % (FZ_SEC - off + 1);
		const struct fz_sec *s = &base.sec[si % base.n];

		data += 6;
		size -= 6;
		if (len > size)
			len = size;
		ovl[n].role = s->role;
		ovl[n].no = s->no;
		memcpy(ovl[n].d, s->d, FZ_SEC);
		memcpy(ovl[n].d + off, data, len);
		data += len;
		size -= len;
		n++;
	}
	fz_run(&base, ovl, n, 20.0, &r);
	if (fz_fail(&r)) {
		fprintf(stderr, "fuzz failure: %s\n", r.first);
		abort();
	}
	return 0;
}
