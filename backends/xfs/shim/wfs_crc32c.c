// SPDX-License-Identifier: GPL-2.0-only
#include "wfs_shim.h"
#include "crc32c_tab.inc"

u32 crc32c_le(u32 crc, const void *data, size_t len)
{
	const unsigned char *p = data;

	while (len && ((uintptr_t)p & 7)) {
		crc = crc32c_tab[0][(crc ^ *p++) & 0xff] ^ (crc >> 8);
		len--;
	}
	while (len >= 8) {
		u32 lo, hi;

		memcpy(&lo, p, 4);
		memcpy(&hi, p + 4, 4);
		lo = le32_to_cpu(lo) ^ crc;
		hi = le32_to_cpu(hi);
		crc = crc32c_tab[7][lo & 0xff] ^ crc32c_tab[6][(lo >> 8) & 0xff] ^
		      crc32c_tab[5][(lo >> 16) & 0xff] ^ crc32c_tab[4][lo >> 24] ^
		      crc32c_tab[3][hi & 0xff] ^ crc32c_tab[2][(hi >> 8) & 0xff] ^
		      crc32c_tab[1][(hi >> 16) & 0xff] ^ crc32c_tab[0][hi >> 24];
		p += 8;
		len -= 8;
	}
	while (len--)
		crc = crc32c_tab[0][(crc ^ *p++) & 0xff] ^ (crc >> 8);
	return crc;
}
