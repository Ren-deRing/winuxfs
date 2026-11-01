// SPDX-License-Identifier: GPL-2.0-only
/*
 * Write performance baseline on the host (not a pass/fail test), file-backed device
 * (the page cache makes device latency ~0, so this measures the backend's own cost).
 *   bench_write IMAGE [OPTIONS]      IMAGE: a mkfs.xfs image of >= 2 GiB (it is modified)
 * Reports files/s for create and unlink, MiB/s for sequential writes with 1 MiB and
 * 4 KiB requests and for overwrite, and the unmount (final flush) time, each with the
 * number of device writes and flushes.
 */
#include "wfs_xfs.h"
#include "ops.h"
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

struct wfs_blkdev { int fd; uint64_t size; };
static unsigned long nw, nwb, nf;
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
	__atomic_fetch_add(&nw, 1, __ATOMIC_RELAXED);
	__atomic_fetch_add(&nwb, l, __ATOMIC_RELAXED);
	return pwrite(d->fd, b, l, (off_t)o) == (ssize_t)l ? 0 : -WFS_EIO;
}
static wfs_err_t h_flush(wfs_blkdev *d)
{
	(void)d;
	__atomic_fetch_add(&nf, 1, __ATOMIC_RELAXED);
	return 0;
}
static const wfs_host_api api = {
	.abi_version = WFS_ABI_VERSION, .size = sizeof(wfs_host_api),
	.blk_size = h_size, .blk_sector_size = h_ss, .blk_read_only = h_ro,
	.blk_read = h_read, .blk_write = h_write, .blk_flush = h_flush, .log = h_log,
};

static double now(void)
{
	struct timespec t;

	clock_gettime(CLOCK_MONOTONIC, &t);
	return (double)t.tv_sec + (double)t.tv_nsec / 1e9;
}
static void phase(const char *what, double t0, double units, const char *unit, unsigned long w0,
		  unsigned long f0, unsigned long b0)
{
	double dt = now() - t0;

	printf("%-32s %7.2f s %10.1f %-8s dev writes %7lu (%.1f MiB) flushes %lu\n", what, dt,
	       units / dt, unit, nw - w0, (double)(nwb - b0) / 1048576.0, nf - f0);
}
#define P0 unsigned long w0 = nw, f0 = nf, b0 = nwb; double t0 = now()
#define NFILES 5000

static int fill_big(struct wfs_xfs *x, wfs_ino_t f, const uint8_t *buf)
{
	size_t done;
	int i;

	for (i = 0; i < 512; i++)
		if (wfs_xfs_op_write(x, f, (uint64_t)i << 20, buf, 1 << 20, &done) || done != (1u << 20))
			return 1;
	return 0;
}

int main(int argc, char **argv)
{
	struct wfs_blkdev dev;
	wfs_blkdev *p[WFS_DEV_COUNT] = { 0 };
	struct wfs_xfs *x;
	struct stat sb;
	wfs_ino_t d, f;
	wfs_stat st;
	static uint8_t buf[1 << 20];
	char name[64];
	int i, rc;
	size_t done;

	if (argc < 2) {
		fprintf(stderr, "usage: bench_write IMAGE [OPTIONS]\n");
		return 2;
	}
	dev.fd = open(argv[1], O_RDWR);
	if (dev.fd < 0 || fstat(dev.fd, &sb))
		return 2;
	dev.size = (uint64_t)sb.st_size;
	p[WFS_DEV_DATA] = &dev;
	rc = wfs_xfs_mount(&api, p, argc > 2 ? argv[2] : "", &x);
	if (rc) {
		fprintf(stderr, "mount: %d\n", rc);
		return 1;
	}
	memset(buf, 0x5a, sizeof(buf));
	if (wfs_xfs_op_create(x, 1, "bench", 5, WFS_FT_DIR, 0755, 0, 0, &d, &st)) {
		fprintf(stderr, "mkdir failed\n");
		return 1;
	}
	{
		P0;
		for (i = 0; i < NFILES; i++) {
			snprintf(name, sizeof(name), "f%d", i);
			if (wfs_xfs_op_create(x, d, name, (uint32_t)strlen(name), WFS_FT_REG, 0644, 0, 0, &f, &st))
				return 1;
		}
		phase("create 5000 empty files", t0, NFILES, "files/s", w0, f0, b0);
	}
	if (wfs_xfs_op_create(x, d, "big", 3, WFS_FT_REG, 0644, 0, 0, &f, &st))
		return 1;
	{
		P0;
		if (fill_big(x, f, buf))
			return 1;
		phase("write 512 MiB, 1 MiB requests", t0, 512, "MiB/s", w0, f0, b0);
	}
	{
		P0;
		if (fill_big(x, f, buf))
			return 1;
		phase("overwrite 512 MiB", t0, 512, "MiB/s", w0, f0, b0);
	}
	if (wfs_xfs_op_create(x, d, "small", 5, WFS_FT_REG, 0644, 0, 0, &f, &st))
		return 1;
	{
		P0;
		for (i = 0; i < 16384; i++)
			if (wfs_xfs_op_write(x, f, (uint64_t)i * 4096, buf, 4096, &done) || done != 4096)
				return 1;
		phase("write 64 MiB, 4 KiB requests", t0, 64, "MiB/s", w0, f0, b0);
	}
	{
		P0;
		for (i = 0; i < NFILES; i++) {
			snprintf(name, sizeof(name), "f%d", i);
			if (wfs_xfs_op_unlink(x, d, name, (uint32_t)strlen(name)))
				return 1;
		}
		phase("unlink 5000 files", t0, NFILES, "files/s", w0, f0, b0);
	}
	{
		P0;
		rc = wfs_xfs_unmount(x);
		phase("unmount (flush everything)", t0, 1, "ops/s", w0, f0, b0);
	}
	close(dev.fd);
	return rc ? 1 : 0;
}
