// SPDX-License-Identifier: GPL-2.0-only
/*
 * Performance baseline on the host (not a pass/fail test): time readdir of a
 * big directory with and without a lookup+forget per entry (what the Vfs does
 * for every entry through stat_at), and a sequential read of one file.
 *   bench_dir IMAGE DIRPATH [FILEPATH]
 */
#include "wfs_xfs.h"
#include "icache.h"
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

struct wfs_blkdev { int fd; uint64_t size; };
static void h_log(int level, const char *msg) { (void)level; (void)msg; }
static uint64_t h_size(wfs_blkdev *d) { return d->size; }
static uint32_t h_ss(wfs_blkdev *d) { (void)d; return 512; }
static int h_ro(wfs_blkdev *d) { (void)d; return 1; }
static unsigned long n_reads, n_bytes;
static wfs_err_t h_read(wfs_blkdev *d, uint64_t off, void *buf, size_t len)
{
	n_reads++;
	n_bytes += len;
	return pread(d->fd, buf, len, (off_t)off) == (ssize_t)len ? 0 : -WFS_EIO;
}
static wfs_err_t h_write(wfs_blkdev *d, uint64_t o, const void *b, size_t l)
{ (void)d; (void)o; (void)b; (void)l; return -WFS_EROFS; }
static wfs_err_t h_flush(wfs_blkdev *d) { (void)d; return 0; }
static const wfs_host_api api = {
	.abi_version = WFS_ABI_VERSION, .size = sizeof(wfs_host_api),
	.blk_size = h_size, .blk_sector_size = h_ss, .blk_read_only = h_ro,
	.blk_read = h_read, .blk_write = h_write, .blk_flush = h_flush, .log = h_log,
};

static double now(void)
{
	struct timespec t;

	clock_gettime(CLOCK_MONOTONIC, &t);
	return t.tv_sec * 1e3 + t.tv_nsec / 1e6;
}

struct names { char (*n)[256]; uint32_t *len; size_t cnt, cap; };
static int cb(void *ctx, const wfs_dirent *e)
{
	struct names *l = ctx;

	if (l->cnt == l->cap) {
		l->cap = l->cap ? 2 * l->cap : 1024;
		l->n = realloc(l->n, l->cap * 256);
		l->len = realloc(l->len, l->cap * 4);
	}
	memcpy(l->n[l->cnt], e->name, e->name_len);
	l->len[l->cnt++] = e->name_len;
	return 0;
}

static wfs_ino_t resolve(struct wfs_xfs *x, const char *path)
{
	wfs_ino_t cur = WFS_ROOT_INO, out;
	wfs_stat st;
	char buf[512], *tok, *save = NULL;

	snprintf(buf, sizeof(buf), "%s", path);
	for (tok = strtok_r(buf, "/", &save); tok; tok = strtok_r(NULL, "/", &save)) {
		if (wfs_xfs_op_lookup(x, cur, tok, (uint32_t)strlen(tok), &out, &st))
			return 0;
		if (cur != WFS_ROOT_INO)
			wfs_xfs_op_forget(x, cur, 1);
		cur = out;
	}
	return cur;
}

int main(int argc, char **argv)
{
	wfs_blkdev dev = { 0 };
	wfs_blkdev *p[WFS_DEV_COUNT] = { 0 };
	struct wfs_xfs *x;
	struct stat sb;
	struct names l = { 0 };
	double t;
	int r, i;
	wfs_ino_t dir;

	if (argc < 3)
		return 2;
	dev.fd = open(argv[1], O_RDONLY);
	if (dev.fd < 0 || fstat(dev.fd, &sb))
		return 2;
	dev.size = (uint64_t)sb.st_size;
	p[WFS_DEV_DATA] = &dev;
	t = now();
	if (wfs_xfs_mount(&api, p, NULL, &x)) {
		fprintf(stderr, "mount failed\n");
		return 1;
	}
	printf("mount_ms %.2f\n", now() - t);
	dir = resolve(x, argv[2]);
	if (!dir)
		return 1;
	for (r = 0; r < 4; r++) {
		l.cnt = 0;
		t = now();
		if (wfs_xfs_op_readdir(x, dir, 0, cb, &l))
			return 1;
		printf("run%d readdir_only_ms %.2f entries %zu (blk_reads so far %lu, %lu bytes)\n", r,
		       now() - t, l.cnt, n_reads, n_bytes);
		t = now();
		for (i = 0; i < (int)l.cnt; i++) {
			wfs_ino_t ino;
			wfs_stat st;

			if (wfs_xfs_op_lookup(x, dir, l.n[i], l.len[i], &ino, &st))
				return 1;
			wfs_xfs_op_forget(x, ino, 1);
		}
		printf("run%d lookup_forget_all_ms %.2f (blk_reads so far %lu, %lu bytes)\n", r,
		       now() - t, n_reads, n_bytes);
	}
	if (argc > 3) {
		wfs_ino_t f = resolve(x, argv[3]);
		static char buf[1 << 20];
		uint64_t off = 0;
		size_t got;

		t = now();
		for (;;) {
			got = sizeof(buf);
			if (wfs_xfs_op_read(x, f, off, buf, got, &got) || !got)
				break;
			off += got;
		}
		t = now() - t;
		printf("seqread_bytes %llu ms %.1f MiB/s %.1f\n", (unsigned long long)off, t,
		       off / 1048576.0 / (t / 1e3));
	}
	wfs_xfs_unmount(x);
	return 0;
}
