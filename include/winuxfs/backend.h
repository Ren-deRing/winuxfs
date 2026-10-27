#ifndef WINUXFS_BACKEND_H
#define WINUXFS_BACKEND_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define WFS_ABI_VERSION 1
typedef int32_t wfs_err_t;
typedef uint64_t wfs_ino_t;
typedef struct wfs_fs wfs_fs;          /* 백엔드가 정의하는 마운트 인스턴스 */
typedef struct wfs_blkdev wfs_blkdev;  /* 코어가 정의하는 블록 장치 */

enum { WFS_DEV_DATA = 0, WFS_DEV_LOG = 1, WFS_DEV_RT = 2, WFS_DEV_COUNT = 3 };
enum { WFS_FT_REG = 1, WFS_FT_DIR, WFS_FT_LNK, WFS_FT_CHR, WFS_FT_BLK, WFS_FT_FIFO, WFS_FT_SOCK };
enum { WFS_CAP_READONLY = 1, WFS_CAP_SINGLE_THREADED = 2, WFS_CAP_XATTR = 4, WFS_CAP_HARDLINK = 8, WFS_CAP_SYMLINK = 16 };
enum { WFS_SET_MODE = 1, WFS_SET_UID = 2, WFS_SET_GID = 4, WFS_SET_SIZE = 8, WFS_SET_ATIME = 16, WFS_SET_MTIME = 32 };
enum { WFS_RENAME_NOREPLACE = 1, WFS_RENAME_EXCHANGE = 2 };

typedef struct { int64_t sec; uint32_t nsec; } wfs_time;
typedef struct {
    wfs_ino_t ino; uint32_t ftype; uint32_t mode;           /* mode = 권한 비트 + setuid/setgid/sticky */
    uint32_t uid, gid, nlink; uint32_t rdev_major, rdev_minor;
    uint64_t size, blocks;                                   /* blocks = 512바이트 단위 */
    wfs_time atime, mtime, ctime, btime;
} wfs_stat;
typedef struct {
    uint64_t total_bytes, free_bytes, avail_bytes;
    uint64_t total_inodes, free_inodes; uint32_t block_size, name_max;
} wfs_statfs;
typedef struct { uint64_t next_cookie; wfs_ino_t ino; uint32_t ftype; uint32_t name_len; const char *name; } wfs_dirent;
/* 콜백이 0이 아닌 값을 반환하면 readdir를 중단한다. */
typedef int (*wfs_dirent_cb)(void *ctx, const wfs_dirent *ent);

typedef struct {
    uint32_t abi_version, size;
    uint64_t (*blk_size)(wfs_blkdev *);
    uint32_t (*blk_sector_size)(wfs_blkdev *);
    int      (*blk_read_only)(wfs_blkdev *);
    wfs_err_t (*blk_read)(wfs_blkdev *, uint64_t off, void *buf, size_t len);
    wfs_err_t (*blk_write)(wfs_blkdev *, uint64_t off, const void *buf, size_t len);
    wfs_err_t (*blk_flush)(wfs_blkdev *);
    void (*log)(int level, const char *msg);                 /* 0=error 1=warn 2=info 3=debug */
} wfs_host_api;

typedef struct {
    wfs_err_t (*probe)(const wfs_host_api *, wfs_blkdev *devs[WFS_DEV_COUNT]);   /* 0 = 이 형식임, -EINVAL = 아님 */
    wfs_err_t (*mount)(const wfs_host_api *, wfs_blkdev *devs[WFS_DEV_COUNT], const char *options, wfs_fs **out);
    wfs_err_t (*unmount)(wfs_fs *);
    wfs_err_t (*statfs)(wfs_fs *, wfs_statfs *);
    wfs_err_t (*getattr)(wfs_fs *, wfs_ino_t, wfs_stat *);
    wfs_err_t (*setattr)(wfs_fs *, wfs_ino_t, const wfs_stat *, uint32_t set_mask);
    wfs_err_t (*lookup)(wfs_fs *, wfs_ino_t dir, const char *name, uint32_t len, wfs_ino_t *out, wfs_stat *st);
    void      (*forget)(wfs_fs *, wfs_ino_t, uint64_t nlookup);
    wfs_err_t (*readdir)(wfs_fs *, wfs_ino_t dir, uint64_t cookie, wfs_dirent_cb, void *ctx);   /* cookie 0 = 처음 */
    wfs_err_t (*read)(wfs_fs *, wfs_ino_t, uint64_t off, void *buf, size_t len, size_t *done);
    wfs_err_t (*write)(wfs_fs *, wfs_ino_t, uint64_t off, const void *buf, size_t len, size_t *done);
    wfs_err_t (*create)(wfs_fs *, wfs_ino_t dir, const char *name, uint32_t len, uint32_t ftype,
                        uint32_t mode, uint32_t uid, uint32_t gid, wfs_ino_t *out, wfs_stat *st);
    wfs_err_t (*unlink)(wfs_fs *, wfs_ino_t dir, const char *name, uint32_t len);
    wfs_err_t (*rmdir)(wfs_fs *, wfs_ino_t dir, const char *name, uint32_t len);
    wfs_err_t (*rename)(wfs_fs *, wfs_ino_t odir, const char *oname, uint32_t olen,
                        wfs_ino_t ndir, const char *nname, uint32_t nlen, uint32_t flags);
    wfs_err_t (*link)(wfs_fs *, wfs_ino_t ino, wfs_ino_t ndir, const char *name, uint32_t len);
    wfs_err_t (*symlink)(wfs_fs *, wfs_ino_t dir, const char *name, uint32_t len, const char *target, uint32_t tlen,
                         uint32_t uid, uint32_t gid, wfs_ino_t *out, wfs_stat *st);
    wfs_err_t (*readlink)(wfs_fs *, wfs_ino_t, char *buf, size_t cap, size_t *len);
    wfs_err_t (*sync)(wfs_fs *);
    wfs_err_t (*fsync)(wfs_fs *, wfs_ino_t, int datasync);
} wfs_ops;

typedef struct {
    uint32_t abi_version, size;
    const char *name;               /* 예: "memfs", "xfs" */
    uint32_t caps;                  /* WFS_CAP_* */
    uint32_t dev_allowed, dev_required;   /* 비트마스크: 1 << WFS_DEV_* */
    const wfs_ops *ops;
} wfs_backend_desc;

/* 플러그인이 export하는 유일한 심볼 */
const wfs_backend_desc *wfs_backend_entry(void);

#ifdef __cplusplus
}
#endif

#endif /* WINUXFS_BACKEND_H */
