// SPDX-License-Identifier: GPL-2.0-only
/*
 * Real libxfs verifier through the cache: xfs_agf_buf_ops (which reports
 * corruption only via xfs_verifier_error) on a mkfs.xfs image. A good AGF
 * is cached; a flipped byte (bad CRC) and a bad magic with a fixed-up CRC
 * (corruption) fail the read and are not cached. argv[1] = image path.
 */
#include "buf.h"
#include "xfs_format.h"
#include "xfs_log_format.h"
#include "xfs_shared.h"
#include "xfs_trans_resv.h"
#include "xfs_mount.h"
#include "xfs_sb.h"
#include "xfs_alloc.h"
#include "xfs_rmap_btree.h"
#include "xfs_refcount_btree.h"
#include "xfs_error.h"
#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>

static int failures, checks;
#define CHECK(c) do { checks++; if (!(c)) { failures++; \
	fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #c); } } while (0)

static int fd, reads;
static uint64_t flip_off = (uint64_t)-1;	/* byte to corrupt in reads */
static int fix_crc;

static int rd(void *ctx, int role, uint64_t off, void *buf, size_t len)
{
	(void)ctx; (void)role;
	reads++;
	if (pread(fd, buf, len, (off_t)off) != (ssize_t)len)
		return -WFS_EIO;
	if (flip_off >= off && flip_off < off + len)
		((unsigned char *)buf)[flip_off - off] ^= 0xFF;
	if (fix_crc && flip_off >= off && flip_off < off + len)
		xfs_update_cksum(buf, len, XFS_AGF_CRC_OFF);
	return 0;
}

int main(int argc, char **argv)
{
	static struct xfs_mount mp;
	static struct xfs_buftarg bt;
	struct wfs_xfs_blk blk = { .read = rd };
	struct wfs_xfs_bcache *c;
	struct xfs_buf *bp;
	unsigned char sb[512];
	xfs_daddr_t agf;
	int bbs;

	if (argc < 2 || (fd = open(argv[1], O_RDONLY)) < 0) {
		fprintf(stderr, "usage: test_buf_agf IMAGE\n");
		return 2;
	}
	CHECK(pread(fd, sb, sizeof(sb), 0) == sizeof(sb));
	xfs_sb_from_disk(&mp.m_sb, (struct xfs_dsb *)sb);
	mp.m_features |= xfs_sb_version_to_features(&mp.m_sb);
	xfs_sb_mount_common(&mp, &mp.m_sb);
	xfs_alloc_compute_maxlevels(&mp);
	xfs_rmapbt_compute_maxlevels(&mp);
	xfs_refcountbt_compute_maxlevels(&mp);
	CHECK(xfs_has_crc(&mp));
	blk.sector_size[0] = mp.m_sb.sb_sectsize;
	blk.size_bytes[0] = (uint64_t)mp.m_sb.sb_dblocks * mp.m_sb.sb_blocksize;
	c = wfs_xfs_bcache_create(&blk, WFS_XFS_BUFCACHE_DEFAULT_BYTES);
	bt.bt_mount = &mp;
	CHECK(wfs_xfs_buftarg_bind(&bt, c, WFS_DEV_DATA) == 0);
	mp.m_ddev_targp = &bt;
	agf = XFS_AGF_DADDR(&mp);
	bbs = XFS_FSS_TO_BB(&mp, 1);

	CHECK(xfs_buf_read(&bt, agf, bbs, 0, &bp, &xfs_agf_buf_ops) == 0);
	CHECK(bp && ((struct xfs_agf *)bp->b_addr)->agf_magicnum == cpu_to_be32(XFS_AGF_MAGIC));
	xfs_buf_relse(bp);
	CHECK(xfs_buf_read(&bt, agf, bbs, 0, &bp, &xfs_agf_buf_ops) == 0 && reads == 1);
	xfs_buf_relse(bp);
	wfs_xfs_bcache_purge(c);

	flip_off = (uint64_t)agf * 512 + 100;		/* inside the AGF, CRC now wrong */
	reads = 0;
	CHECK(xfs_buf_read(&bt, agf, bbs, 0, &bp, &xfs_agf_buf_ops) == -EFSCORRUPTED && bp == NULL);
	CHECK(xfs_buf_incore(&bt, agf, bbs, 0, &bp) == -ENOENT);
	CHECK(wfs_xfs_bcache_usage(c) == 0);
	CHECK(xfs_buf_read(&bt, agf, bbs, 0, &bp, &xfs_agf_buf_ops) == -EFSCORRUPTED && reads == 2);

	flip_off = (uint64_t)agf * 512;			/* magic number, CRC fixed up */
	fix_crc = 1;
	CHECK(xfs_buf_read(&bt, agf, bbs, 0, &bp, &xfs_agf_buf_ops) == -EFSCORRUPTED && bp == NULL);
	CHECK(wfs_xfs_bcache_usage(c) == 0);
	fix_crc = 0;
	flip_off = (uint64_t)-1;
	CHECK(xfs_buf_read(&bt, agf, bbs, 0, &bp, &xfs_agf_buf_ops) == 0);	/* good again */
	if (bp)
		xfs_buf_relse(bp);
	printf("test_buf_agf: %d checks, %d failures\n", checks, failures);
	return failures ? 1 : 0;
}
