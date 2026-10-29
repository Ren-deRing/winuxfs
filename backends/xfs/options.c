// SPDX-License-Identifier: GPL-2.0-only
/*
 * Mount option parser (Task 6). Grammar: item { "," item }, item =
 *   norecovery | bufcache_bytes=N | inodecache_entries=N
 * N is a plain decimal number (digits only, no sign/space/suffix).
 * Ranges: bufcache_bytes 1 MiB..64 GiB, inodecache_entries 64..2^24.
 */
#include <stdio.h>
#include <string.h>
#include <winuxfs/errno.h>
#include "mount.h"

static void opt_err(const wfs_host_api *api, const char *what, const char *tok, size_t len)
{
	char msg[160];
	int tl = len > 48 ? 48 : (int)len;

	if (!api || !api->log)
		return;
	snprintf(msg, sizeof(msg), "xfs: mount option '%.*s%s': %s", tl, tok,
		 len > 48 ? "..." : "", what);
	api->log(0, msg);
}

/* digits only; false on empty, non-digit or uint64 overflow */
static bool parse_u64(const char *s, size_t len, uint64_t *out)
{
	uint64_t v = 0;
	size_t i;

	if (len == 0)
		return false;
	for (i = 0; i < len; i++) {
		unsigned d = (unsigned char)s[i] - '0';

		if (d > 9 || v > (UINT64_MAX - d) / 10)
			return false;
		v = v * 10 + d;
	}
	*out = v;
	return true;
}

int wfs_xfs_parse_options(const wfs_host_api *api, const char *str,
			  struct wfs_xfs_options *o)
{
	const char *p = str;

	o->norecovery = false;
	o->bufcache_bytes = WFS_XFS_BUFCACHE_DEF;
	o->inodecache_entries = WFS_XFS_INODECACHE_DEF;
	if (!p)
		return 0;
	while (*p) {
		const char *end = strchr(p, ',');
		size_t len = end ? (size_t)(end - p) : strlen(p);
		const char *eq = memchr(p, '=', len);
		size_t klen = eq ? (size_t)(eq - p) : len;
		uint64_t v;

		if (len == 0)
			goto next;
		if (klen == 10 && !memcmp(p, "norecovery", 10)) {
			if (eq) {
				opt_err(api, "norecovery takes no value", p, len);
				return -WFS_EINVAL;
			}
			o->norecovery = true;
		} else if (klen == 14 && !memcmp(p, "bufcache_bytes", 14)) {
			if (!eq || !parse_u64(eq + 1, len - klen - 1, &v)) {
				opt_err(api, "needs a decimal number", p, len);
				return -WFS_EINVAL;
			}
			if (v < WFS_XFS_BUFCACHE_MIN || v > WFS_XFS_BUFCACHE_MAX) {
				opt_err(api, "out of range (1 MiB..64 GiB)", p, len);
				return -WFS_EINVAL;
			}
			o->bufcache_bytes = v;
		} else if (klen == 18 && !memcmp(p, "inodecache_entries", 18)) {
			if (!eq || !parse_u64(eq + 1, len - klen - 1, &v)) {
				opt_err(api, "needs a decimal number", p, len);
				return -WFS_EINVAL;
			}
			if (v < WFS_XFS_INODECACHE_MIN || v > WFS_XFS_INODECACHE_MAX) {
				opt_err(api, "out of range (64..16777216)", p, len);
				return -WFS_EINVAL;
			}
			o->inodecache_entries = (uint32_t)v;
		} else {
			opt_err(api, "unknown option", p, len);
			return -WFS_EINVAL;
		}
next:
		if (!end)
			break;
		p = end + 1;
	}
	return 0;
}
