// SPDX-License-Identifier: GPL-2.0-only
/*
 * Replay the log of an XFS image and unmount it cleanly (what the next rw mount does):
 *   replay_image IMAGE [OPTIONS]      IMAGE is modified (a dirty log becomes clean)
 * Exit status 0: mounted read-write and unmounted without error.
 */
#include "wfs_xfs.h"
#include "ops.h"
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

struct wfs_blkdev { int fd; uint64_t size; };
static void h_log(int level, const char *msg) { (void)level; fprintf(stderr, "%s\n", msg); }
static uint64_t h_size(wfs_blkdev *d) { return d->size; }
static uint32_t h_ss(wfs_blkdev *d) { (void)d; return 512; }
static int h_ro(wfs_blkdev *d) { (void)d; return 0; }
static wfs_err_t h_read(wfs_blkdev *d, uint64_t o, void *b, size_t l)
{
	return pread(d->fd, b, l, (off_t)o) == (ssize_t)l ? 0 : -WFS_EIO;
}
static wfs_err_t h_write(wfs_blkdev *d, uint64_t o, const void *b, size_t l)
{
	return pwrite(d->fd, b, l, (off_t)o) == (ssize_t)l ? 0 : -WFS_EIO;
}
static wfs_err_t h_flush(wfs_blkdev *d) { return fsync(d->fd) ? -WFS_EIO : 0; }
static const wfs_host_api api = {
	.abi_version = WFS_ABI_VERSION, .size = sizeof(wfs_host_api),
	.blk_size = h_size, .blk_sector_size = h_ss, .blk_read_only = h_ro,
	.blk_read = h_read, .blk_write = h_write, .blk_flush = h_flush, .log = h_log,
};

int main(int argc, char **argv)
{
	struct wfs_blkdev dev;
	wfs_blkdev *p[WFS_DEV_COUNT] = { 0 };
	struct wfs_xfs *x;
	struct stat sb;
	int rc;

	if (argc < 2) {
		fprintf(stderr, "usage: replay_image IMAGE [OPTIONS]\n");
		return 2;
	}
	dev.fd = open(argv[1], O_RDWR);
	if (dev.fd < 0 || fstat(dev.fd, &sb))
		return 2;
	dev.size = (uint64_t)sb.st_size;
	p[WFS_DEV_DATA] = &dev;
	rc = wfs_xfs_mount(&api, p, argc > 2 ? argv[2] : "", &x);
	if (rc) {
		fprintf(stderr, "mount failed: %d\n", rc);
		return 1;
	}
	rc = wfs_xfs_unmount(x);
	close(dev.fd);
	printf("replayed and unmounted: rc=%d\n", rc);
	return rc ? 1 : 0;
}
