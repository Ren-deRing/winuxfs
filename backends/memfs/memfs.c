/* memfs: in-memory test backend for winuxfs (ABI v1, include/winuxfs/backend.h).
 *
 * Model
 *   - Root directory is inode 1. Inode numbers come from a 64-bit counter and
 *     are never reused, so a number cannot be recycled while any reference
 *     (name link or host lookup reference) exists.
 *   - An inode lives while nlink > 0 or nlookup > 0. lookup/create/symlink add
 *     one lookup reference; forget(n) drops n. An unlinked inode that is still
 *     referenced stays fully readable and writable until the last forget.
 *   - Regular file data is sparse: a sorted array of fixed-size chunks
 *     (MEMFS_CHUNK bytes). Only allocated chunks (plus symlink targets) count
 *     against the MEMFS_CAP_BYTES capacity (1 GiB); exceeding it is -ENOSPC.
 *     The maximum file size is 2^41 bytes (-EFBIG beyond it).
 *   - Directory entries live in one fs-wide hash table keyed by
 *     (parent ino, name) for lookup, and in a per-directory array ordered by a
 *     creation sequence number for readdir. The sequence number is the readdir
 *     cookie: cookie 0 = start; each entry's next_cookie is its own sequence
 *     number; a resumed readdir returns entries with sequence > cookie. Since
 *     sequence numbers only grow, deleting (or adding) entries during a paged
 *     iteration neither duplicates nor skips the remaining entries. A rename
 *     gives the moved entry a fresh sequence number.
 *   - readdir never emits "." or "..": the host synthesises them.
 *   - Thread safety: one coarse mutex per mount (pthreads; winpthreads on
 *     mingw-w64). The readdir callback runs with the lock held, so it must not
 *     call back into the backend (the core's callback does not).
 *   - Error codes are Linux errno values, from winuxfs/errno.h (WFS_E*): the C
 *     runtime's <errno.h> values differ on Windows (e.g. ENOTEMPTY is 41).
 *   - Timestamps come from clock_gettime(CLOCK_REALTIME). Reads do not update
 *     atime (noatime semantics).
 */
#define _POSIX_C_SOURCE 200809L
#include "winuxfs/backend.h"
#include "winuxfs/errno.h"
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef _WIN32
#define WFS_EXPORT __declspec(dllexport)
#else
#define WFS_EXPORT __attribute__((visibility("default")))
#endif

#ifndef MEMFS_CAP_BYTES
#define MEMFS_CAP_BYTES (1ULL << 30) /* overridable only for the self-test */
#endif
#define MEMFS_CHUNK 4096u
#define MEMFS_MAX_FILE (1ULL << 41)
#define MEMFS_NAME_MAX 255u
#define MEMFS_LINK_MAX 4095u /* longest symlink target (PATH_MAX - 1) */
#define MEMFS_NLINK_MAX 0xFFFFFFFFu
#define ROOT_INO 1

struct chunk {
    uint64_t idx; /* chunk number: byte offset / MEMFS_CHUNK */
    unsigned char *data;
};

struct dentry;

struct inode {
    wfs_ino_t ino;
    uint32_t ftype, mode, uid, gid, nlink;
    uint64_t nlookup;
    uint64_t size;
    wfs_time atime, mtime, ctime, btime;
    /* WFS_FT_REG: sparse data, sorted by idx */
    struct chunk *chunks;
    size_t nchunks, capchunks;
    /* WFS_FT_LNK */
    char *target;
    /* WFS_FT_DIR: entries sorted by seq; parent dir (root: itself) */
    struct dentry **ents;
    size_t nents, capents;
    wfs_ino_t parent;
    struct inode *hnext; /* inode hash chain */
};

struct dentry {
    wfs_ino_t dir;
    uint64_t seq;
    struct inode *node;
    struct dentry *hnext; /* dentry hash chain */
    uint32_t len;
    char name[];
};

struct wfs_fs {
    pthread_mutex_t lock;
    struct inode **itab; /* inode hash: ino -> inode */
    size_t ibuckets, icount;
    struct dentry **dtab; /* dentry hash: (dir, name) -> dentry */
    size_t dbuckets, dcount;
    wfs_ino_t next_ino;
    uint64_t next_seq;
    uint64_t used; /* bytes allocated toward MEMFS_CAP_BYTES */
};

/* ---------- helpers ---------- */

static wfs_time now(void)
{
    struct timespec ts;
    wfs_time t = {0, 0};
    if (clock_gettime(CLOCK_REALTIME, &ts) == 0) {
        t.sec = (int64_t)ts.tv_sec;
        t.nsec = (uint32_t)ts.tv_nsec;
    }
    return t;
}

static uint64_t mix64(uint64_t x)
{
    x ^= x >> 33;
    x *= 0xff51afd7ed558ccdULL;
    x ^= x >> 33;
    x *= 0xc4ceb9fe1a85ec53ULL;
    x ^= x >> 33;
    return x;
}

static uint64_t name_hash(wfs_ino_t dir, const char *name, uint32_t len)
{
    uint64_t h = 0xcbf29ce484222325ULL ^ mix64(dir);
    for (uint32_t i = 0; i < len; i++) {
        h ^= (unsigned char)name[i];
        h *= 0x100000001b3ULL;
    }
    return mix64(h);
}

/* 0, -EINVAL (empty, '/', NUL, ".", "..") or -ENAMETOOLONG. */
static wfs_err_t check_name(const char *name, uint32_t len)
{
    if (len == 0 || name == NULL)
        return -WFS_EINVAL;
    if (len > MEMFS_NAME_MAX)
        return -WFS_ENAMETOOLONG;
    if (memchr(name, '/', len) || memchr(name, '\0', len))
        return -WFS_EINVAL;
    if ((len == 1 && name[0] == '.') || (len == 2 && name[0] == '.' && name[1] == '.'))
        return -WFS_EINVAL;
    return 0;
}

/* Grows *arr (of elements of size `elem`) to hold at least `need` items. */
static int grow(void **arr, size_t *cap, size_t need, size_t elem)
{
    if (need <= *cap)
        return 0;
    size_t nc = *cap ? *cap : 8;
    while (nc < need) {
        if (nc > ((size_t)-1) / 2 / elem)
            return -1;
        nc *= 2;
    }
    void *p = realloc(*arr, nc * elem);
    if (!p)
        return -1;
    *arr = p;
    *cap = nc;
    return 0;
}

/* ---------- inode table ---------- */

static struct inode *iget(wfs_fs *fs, wfs_ino_t ino)
{
    struct inode *n = fs->itab[mix64(ino) & (fs->ibuckets - 1)];
    while (n && n->ino != ino)
        n = n->hnext;
    return n;
}

static void irehash(wfs_fs *fs)
{
    size_t nb = fs->ibuckets * 2;
    struct inode **t = calloc(nb, sizeof *t);
    if (!t)
        return; /* keep the old table: just longer chains */
    for (size_t i = 0; i < fs->ibuckets; i++) {
        struct inode *n = fs->itab[i];
        while (n) {
            struct inode *next = n->hnext;
            size_t b = mix64(n->ino) & (nb - 1);
            n->hnext = t[b];
            t[b] = n;
            n = next;
        }
    }
    free(fs->itab);
    fs->itab = t;
    fs->ibuckets = nb;
}

static void iinsert(wfs_fs *fs, struct inode *n)
{
    if (fs->icount >= fs->ibuckets)
        irehash(fs);
    size_t b = mix64(n->ino) & (fs->ibuckets - 1);
    n->hnext = fs->itab[b];
    fs->itab[b] = n;
    fs->icount++;
}

static void iremove(wfs_fs *fs, struct inode *n)
{
    struct inode **pp = &fs->itab[mix64(n->ino) & (fs->ibuckets - 1)];
    while (*pp && *pp != n)
        pp = &(*pp)->hnext;
    if (*pp) {
        *pp = n->hnext;
        fs->icount--;
    }
}

static void free_chunks_from(wfs_fs *fs, struct inode *n, size_t pos)
{
    for (size_t i = pos; i < n->nchunks; i++) {
        free(n->chunks[i].data);
        fs->used -= MEMFS_CHUNK;
    }
    if (pos < n->nchunks)
        n->nchunks = pos;
}

static void inode_free(wfs_fs *fs, struct inode *n)
{
    free_chunks_from(fs, n, 0);
    free(n->chunks);
    if (n->target)
        fs->used -= n->size;
    free(n->target);
    free(n->ents); /* entries themselves are owned by the dentry table */
    free(n);
}

/* Releases an inode with no names and no lookup references (never root). */
static void maybe_free(wfs_fs *fs, struct inode *n)
{
    if (n->ino == ROOT_INO || n->nlink > 0 || n->nlookup > 0)
        return;
    iremove(fs, n);
    inode_free(fs, n);
}

static struct inode *inode_new(wfs_fs *fs, uint32_t ftype, uint32_t mode, uint32_t uid, uint32_t gid)
{
    struct inode *n = calloc(1, sizeof *n);
    if (!n)
        return NULL;
    n->ino = fs->next_ino++;
    n->ftype = ftype;
    n->mode = mode & 07777;
    n->uid = uid;
    n->gid = gid;
    n->atime = n->mtime = n->ctime = n->btime = now();
    iinsert(fs, n);
    return n;
}

/* ---------- directory entries ---------- */

static struct dentry *dfind(wfs_fs *fs, wfs_ino_t dir, const char *name, uint32_t len)
{
    struct dentry *d = fs->dtab[name_hash(dir, name, len) & (fs->dbuckets - 1)];
    while (d && !(d->dir == dir && d->len == len && memcmp(d->name, name, len) == 0))
        d = d->hnext;
    return d;
}

static void drehash(wfs_fs *fs)
{
    size_t nb = fs->dbuckets * 2;
    struct dentry **t = calloc(nb, sizeof *t);
    if (!t)
        return;
    for (size_t i = 0; i < fs->dbuckets; i++) {
        struct dentry *d = fs->dtab[i];
        while (d) {
            struct dentry *next = d->hnext;
            size_t b = name_hash(d->dir, d->name, d->len) & (nb - 1);
            d->hnext = t[b];
            t[b] = d;
            d = next;
        }
    }
    free(fs->dtab);
    fs->dtab = t;
    fs->dbuckets = nb;
}

/* Index of the first entry of `dir` with seq > cookie. */
static size_t ents_after(const struct inode *dir, uint64_t cookie)
{
    size_t lo = 0, hi = dir->nents;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (dir->ents[mid]->seq <= cookie)
            lo = mid + 1;
        else
            hi = mid;
    }
    return lo;
}

/* Adds an entry `name` -> node in dir (name must be absent). Does not touch
 * link counts. Returns 0 or -ENOMEM. */
static wfs_err_t dadd(wfs_fs *fs, struct inode *dir, const char *name, uint32_t len, struct inode *node)
{
    if (grow((void **)&dir->ents, &dir->capents, dir->nents + 1, sizeof *dir->ents))
        return -WFS_ENOMEM;
    struct dentry *d = malloc(sizeof *d + len);
    if (!d)
        return -WFS_ENOMEM;
    d->dir = dir->ino;
    d->seq = fs->next_seq++;
    d->node = node;
    d->len = len;
    memcpy(d->name, name, len);
    /* seq is the largest so far: appending keeps ents sorted. */
    dir->ents[dir->nents++] = d;
    if (fs->dcount >= fs->dbuckets)
        drehash(fs);
    size_t b = name_hash(d->dir, d->name, d->len) & (fs->dbuckets - 1);
    d->hnext = fs->dtab[b];
    fs->dtab[b] = d;
    fs->dcount++;
    return 0;
}

/* Removes and frees an entry. Does not touch link counts. */
static void dremove(wfs_fs *fs, struct inode *dir, struct dentry *d)
{
    struct dentry **pp = &fs->dtab[name_hash(d->dir, d->name, d->len) & (fs->dbuckets - 1)];
    while (*pp && *pp != d)
        pp = &(*pp)->hnext;
    if (*pp) {
        *pp = d->hnext;
        fs->dcount--;
    }
    size_t i = ents_after(dir, d->seq - 1);
    if (i < dir->nents && dir->ents[i] == d) {
        memmove(&dir->ents[i], &dir->ents[i + 1], (dir->nents - i - 1) * sizeof *dir->ents);
        dir->nents--;
    }
    free(d);
}

/* Resolves a live directory inode: -ENOENT if missing or removed, -ENOTDIR. */
static wfs_err_t get_dir(wfs_fs *fs, wfs_ino_t ino, struct inode **out)
{
    struct inode *n = iget(fs, ino);
    if (!n)
        return -WFS_ENOENT;
    if (n->ftype != WFS_FT_DIR)
        return -WFS_ENOTDIR;
    if (n->nlink == 0)
        return -WFS_ENOENT;
    *out = n;
    return 0;
}

/* ---------- stat ---------- */

static void fill_stat(const struct inode *n, wfs_stat *st)
{
    memset(st, 0, sizeof *st);
    st->ino = n->ino;
    st->ftype = n->ftype;
    st->mode = n->mode;
    st->uid = n->uid;
    st->gid = n->gid;
    st->nlink = n->nlink;
    st->size = n->size;
    st->blocks = (uint64_t)n->nchunks * (MEMFS_CHUNK / 512);
    if (n->ftype == WFS_FT_LNK)
        st->blocks = (n->size + 511) / 512;
    st->atime = n->atime;
    st->mtime = n->mtime;
    st->ctime = n->ctime;
    st->btime = n->btime;
}

/* ---------- regular file data ---------- */

/* Index of the first chunk with idx >= key. */
static size_t chunk_pos(const struct inode *n, uint64_t key)
{
    size_t lo = 0, hi = n->nchunks;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (n->chunks[mid].idx < key)
            lo = mid + 1;
        else
            hi = mid;
    }
    return lo;
}

/* Returns the chunk `idx`, allocating a zeroed one if needed; NULL with *err
 * set on -ENOSPC/-ENOMEM. */
static unsigned char *chunk_get(wfs_fs *fs, struct inode *n, uint64_t idx, wfs_err_t *err)
{
    size_t p = chunk_pos(n, idx);
    if (p < n->nchunks && n->chunks[p].idx == idx)
        return n->chunks[p].data;
    if (fs->used > MEMFS_CAP_BYTES - MEMFS_CHUNK) {
        *err = -WFS_ENOSPC;
        return NULL;
    }
    if (grow((void **)&n->chunks, &n->capchunks, n->nchunks + 1, sizeof *n->chunks)) {
        *err = -WFS_ENOMEM;
        return NULL;
    }
    unsigned char *data = calloc(1, MEMFS_CHUNK);
    if (!data) {
        *err = -WFS_ENOMEM;
        return NULL;
    }
    memmove(&n->chunks[p + 1], &n->chunks[p], (n->nchunks - p) * sizeof *n->chunks);
    n->chunks[p].idx = idx;
    n->chunks[p].data = data;
    n->nchunks++;
    fs->used += MEMFS_CHUNK;
    return data;
}

/* Sets the size of a regular file; drops data past the new end. */
static void truncate_to(wfs_fs *fs, struct inode *n, uint64_t size)
{
    if (size < n->size) {
        uint64_t keep = (size + MEMFS_CHUNK - 1) / MEMFS_CHUNK; /* chunks [0, keep) stay */
        free_chunks_from(fs, n, chunk_pos(n, keep));
        uint32_t tail = (uint32_t)(size % MEMFS_CHUNK);
        if (tail) {
            size_t p = chunk_pos(n, size / MEMFS_CHUNK);
            if (p < n->nchunks && n->chunks[p].idx == size / MEMFS_CHUNK)
                memset(n->chunks[p].data + tail, 0, MEMFS_CHUNK - tail);
        }
    }
    n->size = size;
}

/* ---------- ops ---------- */

static wfs_err_t m_probe(const wfs_host_api *h, wfs_blkdev *d[WFS_DEV_COUNT])
{
    (void)h;
    (void)d;
    return -WFS_EINVAL; /* device-less: never claims a device */
}

static wfs_err_t m_mount(const wfs_host_api *h, wfs_blkdev *d[WFS_DEV_COUNT], const char *opts, wfs_fs **out)
{
    (void)h;
    (void)d;
    (void)opts; /* no options */
    wfs_fs *fs = calloc(1, sizeof *fs);
    if (!fs)
        return -WFS_ENOMEM;
    fs->ibuckets = 64;
    fs->dbuckets = 64;
    fs->itab = calloc(fs->ibuckets, sizeof *fs->itab);
    fs->dtab = calloc(fs->dbuckets, sizeof *fs->dtab);
    if (!fs->itab || !fs->dtab || pthread_mutex_init(&fs->lock, NULL) != 0) {
        free(fs->itab);
        free(fs->dtab);
        free(fs);
        return -WFS_ENOMEM;
    }
    fs->next_ino = ROOT_INO;
    fs->next_seq = 1; /* cookie 0 means "from the start" */
    struct inode *root = inode_new(fs, WFS_FT_DIR, 0755, 0, 0);
    if (!root) {
        pthread_mutex_destroy(&fs->lock);
        free(fs->itab);
        free(fs->dtab);
        free(fs);
        return -WFS_ENOMEM;
    }
    root->nlink = 2;
    root->parent = ROOT_INO;
    *out = fs;
    return 0;
}

static wfs_err_t m_unmount(wfs_fs *fs)
{
    for (size_t i = 0; i < fs->dbuckets; i++) {
        struct dentry *d = fs->dtab[i];
        while (d) {
            struct dentry *next = d->hnext;
            free(d);
            d = next;
        }
    }
    for (size_t i = 0; i < fs->ibuckets; i++) {
        struct inode *n = fs->itab[i];
        while (n) {
            struct inode *next = n->hnext;
            inode_free(fs, n);
            n = next;
        }
    }
    free(fs->dtab);
    free(fs->itab);
    pthread_mutex_destroy(&fs->lock);
    free(fs);
    return 0;
}

static wfs_err_t m_statfs(wfs_fs *fs, wfs_statfs *s)
{
    pthread_mutex_lock(&fs->lock);
    uint64_t free_bytes = MEMFS_CAP_BYTES - fs->used;
    s->total_bytes = MEMFS_CAP_BYTES;
    s->free_bytes = free_bytes;
    s->avail_bytes = free_bytes;
    /* No inode limit; report one inode per free block as "free". */
    s->free_inodes = free_bytes / MEMFS_CHUNK;
    s->total_inodes = fs->icount + s->free_inodes;
    s->block_size = MEMFS_CHUNK;
    s->name_max = MEMFS_NAME_MAX;
    pthread_mutex_unlock(&fs->lock);
    return 0;
}

static wfs_err_t m_getattr(wfs_fs *fs, wfs_ino_t ino, wfs_stat *st)
{
    pthread_mutex_lock(&fs->lock);
    struct inode *n = iget(fs, ino);
    if (n)
        fill_stat(n, st);
    pthread_mutex_unlock(&fs->lock);
    return n ? 0 : -WFS_ENOENT;
}

static int valid_time(const wfs_time *t) { return t->nsec < 1000000000u; }

static wfs_err_t m_setattr(wfs_fs *fs, wfs_ino_t ino, const wfs_stat *st, uint32_t mask)
{
    wfs_err_t r = 0;
    pthread_mutex_lock(&fs->lock);
    struct inode *n = iget(fs, ino);
    if (!n) {
        r = -WFS_ENOENT;
        goto out;
    }
    if (((mask & WFS_SET_ATIME) && !valid_time(&st->atime)) ||
        ((mask & WFS_SET_MTIME) && !valid_time(&st->mtime))) {
        r = -WFS_EINVAL;
        goto out;
    }
    if (mask & WFS_SET_SIZE) {
        if (n->ftype == WFS_FT_DIR) {
            r = -WFS_EISDIR;
            goto out;
        }
        if (n->ftype != WFS_FT_REG) {
            r = -WFS_EINVAL;
            goto out;
        }
        if (st->size > MEMFS_MAX_FILE) {
            r = -WFS_EFBIG;
            goto out;
        }
    }
    /* All checks passed: apply. */
    wfs_time t = now();
    if (mask & WFS_SET_SIZE) {
        truncate_to(fs, n, st->size);
        n->mtime = t;
    }
    if (mask & WFS_SET_MODE)
        n->mode = st->mode & 07777;
    if (mask & WFS_SET_UID)
        n->uid = st->uid;
    if (mask & WFS_SET_GID)
        n->gid = st->gid;
    if (mask & WFS_SET_ATIME)
        n->atime = st->atime;
    if (mask & WFS_SET_MTIME)
        n->mtime = st->mtime;
    n->ctime = t;
out:
    pthread_mutex_unlock(&fs->lock);
    return r;
}

static wfs_err_t m_lookup(wfs_fs *fs, wfs_ino_t dir, const char *name, uint32_t len, wfs_ino_t *out, wfs_stat *st)
{
    wfs_err_t r = check_name(name, len);
    if (r)
        return r;
    pthread_mutex_lock(&fs->lock);
    struct inode *d;
    r = get_dir(fs, dir, &d);
    if (r == 0) {
        struct dentry *e = dfind(fs, dir, name, len);
        if (!e) {
            r = -WFS_ENOENT;
        } else {
            e->node->nlookup++;
            *out = e->node->ino;
            fill_stat(e->node, st);
        }
    }
    pthread_mutex_unlock(&fs->lock);
    return r;
}

static void m_forget(wfs_fs *fs, wfs_ino_t ino, uint64_t nlookup)
{
    pthread_mutex_lock(&fs->lock);
    struct inode *n = iget(fs, ino);
    if (n) {
        n->nlookup -= nlookup < n->nlookup ? nlookup : n->nlookup;
        maybe_free(fs, n);
    }
    pthread_mutex_unlock(&fs->lock);
}

static wfs_err_t m_readdir(wfs_fs *fs, wfs_ino_t dir, uint64_t cookie, wfs_dirent_cb cb, void *ctx)
{
    if (!cb)
        return -WFS_EINVAL;
    pthread_mutex_lock(&fs->lock);
    struct inode *d = iget(fs, dir);
    wfs_err_t r = 0;
    if (!d)
        r = -WFS_ENOENT;
    else if (d->ftype != WFS_FT_DIR)
        r = -WFS_ENOTDIR;
    else {
        /* No "." / ".." entries: the host synthesises them. */
        for (size_t i = ents_after(d, cookie); i < d->nents; i++) {
            const struct dentry *e = d->ents[i];
            wfs_dirent de;
            de.next_cookie = e->seq;
            de.ino = e->node->ino;
            de.ftype = e->node->ftype;
            de.name_len = e->len;
            de.name = e->name;
            if (cb(ctx, &de) != 0)
                break;
        }
    }
    pthread_mutex_unlock(&fs->lock);
    return r;
}

static wfs_err_t file_check(const struct inode *n)
{
    if (!n)
        return -WFS_ENOENT;
    if (n->ftype == WFS_FT_DIR)
        return -WFS_EISDIR;
    if (n->ftype != WFS_FT_REG)
        return -WFS_EINVAL;
    return 0;
}

static wfs_err_t m_read(wfs_fs *fs, wfs_ino_t ino, uint64_t off, void *buf, size_t len, size_t *done)
{
    *done = 0;
    pthread_mutex_lock(&fs->lock);
    struct inode *n = iget(fs, ino);
    wfs_err_t r = file_check(n);
    if (r == 0 && off < n->size && len > 0) {
        uint64_t avail = n->size - off;
        size_t total = avail < (uint64_t)len ? (size_t)avail : len;
        unsigned char *dst = buf;
        size_t pos = 0;
        size_t p = chunk_pos(n, off / MEMFS_CHUNK);
        while (pos < total) {
            uint64_t o = off + pos;
            uint64_t idx = o / MEMFS_CHUNK;
            uint32_t in = (uint32_t)(o % MEMFS_CHUNK);
            size_t k = MEMFS_CHUNK - in;
            if (k > total - pos)
                k = total - pos;
            while (p < n->nchunks && n->chunks[p].idx < idx)
                p++;
            if (p < n->nchunks && n->chunks[p].idx == idx)
                memcpy(dst + pos, n->chunks[p].data + in, k);
            else
                memset(dst + pos, 0, k);
            pos += k;
        }
        *done = total;
    }
    pthread_mutex_unlock(&fs->lock);
    return r;
}

static wfs_err_t m_write(wfs_fs *fs, wfs_ino_t ino, uint64_t off, const void *buf, size_t len, size_t *done)
{
    *done = 0;
    pthread_mutex_lock(&fs->lock);
    struct inode *n = iget(fs, ino);
    wfs_err_t r = file_check(n);
    if (r || len == 0)
        goto out;
    if (off >= MEMFS_MAX_FILE) {
        r = -WFS_EFBIG;
        goto out;
    }
    /* Clamp to the max file size (a straddling write is cut short). */
    uint64_t room = MEMFS_MAX_FILE - off;
    size_t total = room < (uint64_t)len ? (size_t)room : len;
    const unsigned char *src = buf;
    size_t pos = 0;
    while (pos < total) {
        uint64_t o = off + pos;
        uint32_t in = (uint32_t)(o % MEMFS_CHUNK);
        size_t k = MEMFS_CHUNK - in;
        if (k > total - pos)
            k = total - pos;
        wfs_err_t e = 0;
        unsigned char *c = chunk_get(fs, n, o / MEMFS_CHUNK, &e);
        if (!c) {
            if (pos == 0)
                r = e;
            break;
        }
        memcpy(c + in, src + pos, k);
        pos += k;
    }
    if (pos > 0) {
        if (off + pos > n->size)
            n->size = off + pos;
        n->mtime = n->ctime = now();
    }
    *done = pos;
out:
    pthread_mutex_unlock(&fs->lock);
    return r;
}

/* Shared tail of create/symlink: makes the inode and its entry in d. */
static wfs_err_t make_node(wfs_fs *fs, struct inode *d, const char *name, uint32_t len, uint32_t ftype,
                           uint32_t mode, uint32_t uid, uint32_t gid, struct inode **out)
{
    if (dfind(fs, d->ino, name, len))
        return -WFS_EEXIST;
    if (ftype == WFS_FT_DIR && d->nlink >= MEMFS_NLINK_MAX)
        return -WFS_EMLINK;
    struct inode *n = inode_new(fs, ftype, mode, uid, gid);
    if (!n)
        return -WFS_ENOMEM;
    wfs_err_t r = dadd(fs, d, name, len, n);
    if (r) {
        iremove(fs, n);
        inode_free(fs, n);
        return r;
    }
    if (ftype == WFS_FT_DIR) {
        n->nlink = 2;
        n->parent = d->ino;
        d->nlink++;
    } else {
        n->nlink = 1;
    }
    n->nlookup = 1;
    d->mtime = d->ctime = n->btime;
    *out = n;
    return 0;
}

static wfs_err_t m_create(wfs_fs *fs, wfs_ino_t dir, const char *name, uint32_t len, uint32_t ftype,
                          uint32_t mode, uint32_t uid, uint32_t gid, wfs_ino_t *out, wfs_stat *st)
{
    wfs_err_t r = check_name(name, len);
    if (r)
        return r;
    switch (ftype) {
    case WFS_FT_REG: case WFS_FT_DIR: case WFS_FT_CHR: case WFS_FT_BLK: case WFS_FT_FIFO: case WFS_FT_SOCK:
        break;
    default: /* WFS_FT_LNK goes through symlink */
        return -WFS_EINVAL;
    }
    pthread_mutex_lock(&fs->lock);
    struct inode *d, *n;
    r = get_dir(fs, dir, &d);
    if (r == 0)
        r = make_node(fs, d, name, len, ftype, mode, uid, gid, &n);
    if (r == 0) {
        *out = n->ino;
        fill_stat(n, st);
    }
    pthread_mutex_unlock(&fs->lock);
    return r;
}

/* Drops one name link from n (which was just unlinked from `parent`). */
static void drop_link(wfs_fs *fs, struct inode *parent, struct inode *n, wfs_time t)
{
    if (n->ftype == WFS_FT_DIR) {
        n->nlink = 0;
        parent->nlink--;
    } else {
        n->nlink--;
    }
    n->ctime = t;
    maybe_free(fs, n);
}

static wfs_err_t remove_entry(wfs_fs *fs, wfs_ino_t dir, const char *name, uint32_t len, int want_dir)
{
    wfs_err_t r = check_name(name, len);
    if (r)
        return r;
    pthread_mutex_lock(&fs->lock);
    struct inode *d;
    r = get_dir(fs, dir, &d);
    if (r)
        goto out;
    struct dentry *e = dfind(fs, dir, name, len);
    if (!e) {
        r = -WFS_ENOENT;
        goto out;
    }
    struct inode *n = e->node;
    if (want_dir && n->ftype != WFS_FT_DIR) {
        r = -WFS_ENOTDIR;
        goto out;
    }
    if (!want_dir && n->ftype == WFS_FT_DIR) {
        r = -WFS_EISDIR;
        goto out;
    }
    if (want_dir && n->nents > 0) {
        r = -WFS_ENOTEMPTY;
        goto out;
    }
    wfs_time t = now();
    dremove(fs, d, e);
    d->mtime = d->ctime = t;
    drop_link(fs, d, n, t);
out:
    pthread_mutex_unlock(&fs->lock);
    return r;
}

static wfs_err_t m_unlink(wfs_fs *fs, wfs_ino_t dir, const char *name, uint32_t len)
{
    return remove_entry(fs, dir, name, len, 0);
}

static wfs_err_t m_rmdir(wfs_fs *fs, wfs_ino_t dir, const char *name, uint32_t len)
{
    return remove_entry(fs, dir, name, len, 1);
}

static wfs_err_t m_rename(wfs_fs *fs, wfs_ino_t odir, const char *oname, uint32_t olen,
                          wfs_ino_t ndir, const char *nname, uint32_t nlen, uint32_t flags)
{
    if (flags & ~(uint32_t)WFS_RENAME_NOREPLACE)
        return -WFS_EINVAL; /* WFS_RENAME_EXCHANGE is not supported */
    wfs_err_t r = check_name(oname, olen);
    if (r == 0)
        r = check_name(nname, nlen);
    if (r)
        return r;
    pthread_mutex_lock(&fs->lock);
    struct inode *od, *nd;
    r = get_dir(fs, odir, &od);
    if (r == 0)
        r = get_dir(fs, ndir, &nd);
    if (r)
        goto out;
    struct dentry *src = dfind(fs, odir, oname, olen);
    if (!src) {
        r = -WFS_ENOENT;
        goto out;
    }
    struct inode *n = src->node;
    int is_dir = n->ftype == WFS_FT_DIR;
    if (is_dir && odir != ndir) {
        /* The target dir must not be n itself or inside n's subtree. */
        for (struct inode *p = nd;; p = iget(fs, p->parent)) {
            if (!p) {
                r = -WFS_ENOENT; /* broken parent chain: cannot happen */
                goto out;
            }
            if (p == n) {
                r = -WFS_EINVAL;
                goto out;
            }
            if (p->ino == ROOT_INO)
                break;
        }
    }
    struct dentry *dst = dfind(fs, ndir, nname, nlen);
    if (dst) {
        struct inode *victim = dst->node;
        if (flags & WFS_RENAME_NOREPLACE) {
            r = -WFS_EEXIST;
            goto out;
        }
        if (victim == n)
            goto out; /* same inode (same name or hard links): no-op */
        if (is_dir && victim->ftype != WFS_FT_DIR) {
            r = -WFS_ENOTDIR;
            goto out;
        }
        if (!is_dir && victim->ftype == WFS_FT_DIR) {
            r = -WFS_EISDIR;
            goto out;
        }
        if (victim->ftype == WFS_FT_DIR && victim->nents > 0) {
            r = -WFS_ENOTEMPTY;
            goto out;
        }
    }
    if (is_dir && odir != ndir && !dst && nd->nlink >= MEMFS_NLINK_MAX) {
        r = -WFS_EMLINK;
        goto out;
    }
    /* Add the new entry first, so an allocation failure changes nothing. It
     * briefly shares its key with `dst`; dremove() unlinks by pointer. */
    r = dadd(fs, nd, nname, nlen, n);
    if (r)
        goto out;
    wfs_time t = now();
    struct inode *victim = dst ? dst->node : NULL;
    if (dst)
        dremove(fs, nd, dst);
    dremove(fs, od, src);
    if (victim)
        drop_link(fs, nd, victim, t);
    if (is_dir && odir != ndir) {
        od->nlink--;
        nd->nlink++;
        n->parent = ndir;
    }
    n->ctime = t;
    od->mtime = od->ctime = t;
    nd->mtime = nd->ctime = t;
out:
    pthread_mutex_unlock(&fs->lock);
    return r;
}

static wfs_err_t m_link(wfs_fs *fs, wfs_ino_t ino, wfs_ino_t ndir, const char *name, uint32_t len)
{
    wfs_err_t r = check_name(name, len);
    if (r)
        return r;
    pthread_mutex_lock(&fs->lock);
    struct inode *n = iget(fs, ino), *d;
    if (!n) {
        r = -WFS_ENOENT;
        goto out;
    }
    if (n->ftype == WFS_FT_DIR) {
        r = -WFS_EPERM;
        goto out;
    }
    r = get_dir(fs, ndir, &d);
    if (r)
        goto out;
    if (n->nlink == 0) {
        r = -WFS_ENOENT;
        goto out;
    }
    if (n->nlink >= MEMFS_NLINK_MAX) {
        r = -WFS_EMLINK;
        goto out;
    }
    if (dfind(fs, ndir, name, len)) {
        r = -WFS_EEXIST;
        goto out;
    }
    r = dadd(fs, d, name, len, n);
    if (r)
        goto out;
    wfs_time t = now();
    n->nlink++;
    n->ctime = t;
    d->mtime = d->ctime = t;
out:
    pthread_mutex_unlock(&fs->lock);
    return r;
}

static wfs_err_t m_symlink(wfs_fs *fs, wfs_ino_t dir, const char *name, uint32_t len, const char *target,
                           uint32_t tlen, uint32_t uid, uint32_t gid, wfs_ino_t *out, wfs_stat *st)
{
    wfs_err_t r = check_name(name, len);
    if (r)
        return r;
    if (tlen == 0 || target == NULL)
        return -WFS_ENOENT;
    if (tlen > MEMFS_LINK_MAX)
        return -WFS_ENAMETOOLONG;
    char *copy = malloc(tlen);
    if (!copy)
        return -WFS_ENOMEM;
    memcpy(copy, target, tlen);
    pthread_mutex_lock(&fs->lock);
    struct inode *d, *n = NULL;
    r = get_dir(fs, dir, &d);
    if (r == 0 && fs->used > MEMFS_CAP_BYTES - tlen)
        r = -WFS_ENOSPC;
    if (r == 0)
        r = make_node(fs, d, name, len, WFS_FT_LNK, 0777, uid, gid, &n);
    if (r == 0) {
        n->target = copy;
        copy = NULL;
        n->size = tlen;
        fs->used += tlen;
        *out = n->ino;
        fill_stat(n, st);
    }
    pthread_mutex_unlock(&fs->lock);
    free(copy);
    return r;
}

/* Copies up to `cap` bytes of the target (no NUL); *len = bytes copied. */
static wfs_err_t m_readlink(wfs_fs *fs, wfs_ino_t ino, char *buf, size_t cap, size_t *len)
{
    *len = 0;
    pthread_mutex_lock(&fs->lock);
    struct inode *n = iget(fs, ino);
    wfs_err_t r = 0;
    if (!n)
        r = -WFS_ENOENT;
    else if (n->ftype != WFS_FT_LNK)
        r = -WFS_EINVAL;
    else {
        size_t k = n->size < cap ? (size_t)n->size : cap;
        if (k)
            memcpy(buf, n->target, k);
        *len = k;
    }
    pthread_mutex_unlock(&fs->lock);
    return r;
}

static wfs_err_t m_sync(wfs_fs *fs)
{
    (void)fs;
    return 0;
}

static wfs_err_t m_fsync(wfs_fs *fs, wfs_ino_t ino, int datasync)
{
    (void)datasync;
    pthread_mutex_lock(&fs->lock);
    int ok = iget(fs, ino) != NULL;
    pthread_mutex_unlock(&fs->lock);
    return ok ? 0 : -WFS_ENOENT;
}

static const wfs_ops ops = {
    .probe = m_probe,
    .mount = m_mount,
    .unmount = m_unmount,
    .statfs = m_statfs,
    .getattr = m_getattr,
    .setattr = m_setattr,
    .lookup = m_lookup,
    .forget = m_forget,
    .readdir = m_readdir,
    .read = m_read,
    .write = m_write,
    .create = m_create,
    .unlink = m_unlink,
    .rmdir = m_rmdir,
    .rename = m_rename,
    .link = m_link,
    .symlink = m_symlink,
    .readlink = m_readlink,
    .sync = m_sync,
    .fsync = m_fsync,
};

static const wfs_backend_desc desc = {
    .abi_version = WFS_ABI_VERSION,
    .size = sizeof(wfs_backend_desc),
    .name = "memfs",
    .caps = WFS_CAP_HARDLINK | WFS_CAP_SYMLINK,
    .dev_allowed = 0,
    .dev_required = 0,
    .ops = &ops,
};

WFS_EXPORT const wfs_backend_desc *wfs_backend_entry(void) { return &desc; }
