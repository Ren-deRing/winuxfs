/* Standalone self-test for memfs, meant to run under ASan/UBSan
 * (`make selftest SAN=1`). It includes memfs.c directly; the Makefile builds
 * it with MEMFS_CAP_BYTES = 1 MiB so -ENOSPC is reachable. Exits 0 on success. */
#include "memfs.c"

#include <stdio.h>

static int failures;

#define CHECK(c)                                                                 \
    do {                                                                         \
        if (!(c)) {                                                              \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #c); \
            failures++;                                                          \
        }                                                                        \
    } while (0)
#define EQ(a, b) CHECK((long long)(a) == (long long)(b))

static const wfs_ops *O;

static wfs_ino_t mk(wfs_fs *fs, wfs_ino_t dir, const char *name, uint32_t ftype)
{
    wfs_ino_t ino = 0;
    wfs_stat st;
    wfs_err_t r = O->create(fs, dir, name, (uint32_t)strlen(name), ftype, 0644, 0, 0, &ino, &st);
    EQ(r, 0);
    EQ(st.ino, ino);
    return ino;
}

struct collect {
    int n, max;
    uint64_t last;
    char names[64][16];
};

static int collect_cb(void *ctx, const wfs_dirent *e)
{
    struct collect *c = ctx;
    if (e->name_len < 16) {
        memcpy(c->names[c->n], e->name, e->name_len);
        c->names[c->n][e->name_len] = 0;
    }
    c->last = e->next_cookie;
    c->n++;
    return c->n >= c->max;
}

static void test_io(wfs_fs *fs)
{
    wfs_ino_t f = mk(fs, ROOT_INO, "f", WFS_FT_REG);
    size_t done = 99;
    char buf[64];
    /* zero-length IO */
    EQ(O->write(fs, f, 5, "x", 0, &done), 0);
    EQ(done, 0);
    EQ(O->read(fs, f, 0, buf, 0, &done), 0);
    EQ(done, 0);
    /* write across a chunk boundary */
    char big[MEMFS_CHUNK + 10];
    for (size_t i = 0; i < sizeof big; i++)
        big[i] = (char)(i % 200 + 1);
    EQ(O->write(fs, f, MEMFS_CHUNK - 5, big, sizeof big, &done), 0);
    EQ(done, sizeof big);
    char back[sizeof big + 20];
    EQ(O->read(fs, f, MEMFS_CHUNK - 15, back, sizeof back, &done), 0);
    EQ(done, sizeof big + 10);
    CHECK(memcmp(back, "\0\0\0\0\0\0\0\0\0\0", 10) == 0);
    CHECK(memcmp(back + 10, big, sizeof big) == 0);
    /* reads at and past EOF, including offsets that would overflow off+len */
    wfs_stat st;
    O->getattr(fs, f, &st);
    EQ(O->read(fs, f, st.size, buf, sizeof buf, &done), 0);
    EQ(done, 0);
    EQ(O->read(fs, f, UINT64_MAX, buf, sizeof buf, &done), 0);
    EQ(done, 0);
    EQ(O->write(fs, f, UINT64_MAX, "abc", 3, &done), -WFS_EFBIG);
    EQ(O->write(fs, f, MEMFS_MAX_FILE - 1, "abc", 3, &done), 0);
    EQ(done, 1);
    O->getattr(fs, f, &st);
    EQ(st.size, MEMFS_MAX_FILE);
    /* truncate in the middle of a chunk, then re-extend: tail reads zero */
    wfs_stat a;
    memset(&a, 0, sizeof a);
    a.size = MEMFS_CHUNK - 2;
    EQ(O->setattr(fs, f, &a, WFS_SET_SIZE), 0);
    a.size = MEMFS_CHUNK + 100;
    EQ(O->setattr(fs, f, &a, WFS_SET_SIZE), 0);
    EQ(O->read(fs, f, MEMFS_CHUNK - 4, buf, 8, &done), 0);
    EQ(done, 8);
    EQ(buf[0], big[1]);
    EQ(buf[1], big[2]);
    CHECK(memcmp(buf + 2, "\0\0\0\0\0\0", 6) == 0);
    O->forget(fs, f, 1);
    EQ(O->unlink(fs, ROOT_INO, "f", 1), 0);
    EQ(O->getattr(fs, f, &st), -WFS_ENOENT);
    EQ(fs->used, 0);
}

static void test_enospc(wfs_fs *fs)
{
    wfs_ino_t f = mk(fs, ROOT_INO, "fill", WFS_FT_REG);
    static char block[MEMFS_CHUNK];
    memset(block, 7, sizeof block);
    size_t done;
    uint64_t off = 0;
    wfs_err_t r;
    while ((r = O->write(fs, f, off, block, sizeof block, &done)) == 0 && done == sizeof block)
        off += sizeof block;
    EQ(r, -WFS_ENOSPC);
    EQ(off, MEMFS_CAP_BYTES);
    wfs_statfs s;
    O->statfs(fs, &s);
    EQ(s.free_bytes, 0);
    /* A write that hits the cap part-way reports the bytes it did write. */
    wfs_stat a;
    memset(&a, 0, sizeof a);
    a.size = MEMFS_CAP_BYTES - 2 * MEMFS_CHUNK + 10; /* keeps all but the last chunk */
    EQ(O->setattr(fs, f, &a, WFS_SET_SIZE), 0);
    EQ(fs->used, MEMFS_CAP_BYTES - MEMFS_CHUNK);
    EQ(O->write(fs, f, MEMFS_CAP_BYTES - 2 * MEMFS_CHUNK, block, MEMFS_CHUNK, &done), 0);
    EQ(done, MEMFS_CHUNK); /* existing chunk: no allocation */
    char three[3 * MEMFS_CHUNK];
    memset(three, 9, sizeof three);
    EQ(O->write(fs, f, MEMFS_CAP_BYTES - 2 * MEMFS_CHUNK, three, sizeof three, &done), 0);
    EQ(done, 2 * MEMFS_CHUNK); /* one existing + one new chunk, then the cap */
    EQ(fs->used, MEMFS_CAP_BYTES);
    wfs_ino_t l;
    wfs_stat st;
    O->forget(fs, f, 1);
    EQ(O->unlink(fs, ROOT_INO, "fill", 4), 0);
    EQ(fs->used, 0);
    EQ(O->symlink(fs, ROOT_INO, "l", 1, "tgt", 3, 0, 0, &l, &st), 0);
    EQ(fs->used, 3);
    O->forget(fs, l, 1);
    EQ(O->unlink(fs, ROOT_INO, "l", 1), 0);
    EQ(fs->used, 0);
}

static void test_names_and_rename(wfs_fs *fs)
{
    wfs_ino_t ino;
    wfs_stat st;
    char longname[300];
    memset(longname, 'n', sizeof longname);
    EQ(O->create(fs, ROOT_INO, longname, 256, WFS_FT_REG, 0644, 0, 0, &ino, &st), -WFS_ENAMETOOLONG);
    EQ(O->create(fs, ROOT_INO, "a/b", 3, WFS_FT_REG, 0644, 0, 0, &ino, &st), -WFS_EINVAL);
    EQ(O->create(fs, ROOT_INO, "a\0b", 3, WFS_FT_REG, 0644, 0, 0, &ino, &st), -WFS_EINVAL);
    EQ(O->create(fs, ROOT_INO, "..", 2, WFS_FT_REG, 0644, 0, 0, &ino, &st), -WFS_EINVAL);
    EQ(O->create(fs, ROOT_INO, "x", 0, WFS_FT_REG, 0644, 0, 0, &ino, &st), -WFS_EINVAL);
    EQ(O->create(fs, ROOT_INO, "x", 1, WFS_FT_LNK, 0644, 0, 0, &ino, &st), -WFS_EINVAL);
    wfs_ino_t d = mk(fs, ROOT_INO, "d", WFS_FT_DIR);
    wfs_ino_t sub = mk(fs, d, "sub", WFS_FT_DIR);
    wfs_ino_t f = mk(fs, sub, "f", WFS_FT_REG);
    EQ(O->rename(fs, ROOT_INO, "d", 1, sub, "d2", 2, 0), -WFS_EINVAL);
    EQ(O->rename(fs, d, "sub", 3, ROOT_INO, "sub", 3, 0), 0);
    EQ(O->rename(fs, sub, "f", 1, ROOT_INO, "d", 1, 0), -WFS_EISDIR);
    EQ(O->rmdir(fs, ROOT_INO, "d", 1), 0);
    /* d is referenced (create): lookups inside fail, it frees on forget. */
    EQ(O->lookup(fs, d, "x", 1, &ino, &st), -WFS_ENOENT);
    O->forget(fs, d, 1);
    EQ(O->getattr(fs, d, &st), -WFS_ENOENT);
    /* Replace a file by rename while the victim is still referenced. */
    wfs_ino_t g = mk(fs, ROOT_INO, "g", WFS_FT_REG);
    size_t done;
    O->write(fs, g, 0, "victim", 6, &done);
    EQ(O->rename(fs, sub, "f", 1, ROOT_INO, "g", 1, 0), 0);
    char buf[8];
    EQ(O->read(fs, g, 0, buf, sizeof buf, &done), 0);
    EQ(done, 6);
    O->forget(fs, g, 1);
    EQ(O->getattr(fs, g, &st), -WFS_ENOENT);
    EQ(O->lookup(fs, ROOT_INO, "g", 1, &ino, &st), 0);
    EQ(ino, f);
    O->forget(fs, ino, 2);
    /* hard link + readlink truncation */
    EQ(O->link(fs, f, sub, "f2", 2), 0);
    wfs_ino_t l;
    EQ(O->symlink(fs, ROOT_INO, "lnk", 3, "abcdef", 6, 0, 0, &l, &st), 0);
    char t[4];
    size_t tl;
    EQ(O->readlink(fs, l, t, sizeof t, &tl), 0);
    EQ(tl, 4);
    CHECK(memcmp(t, "abcd", 4) == 0);
    O->forget(fs, l, 1);
    O->forget(fs, sub, 1);
    /* left in place on purpose: unmount must free everything (LeakSanitizer) */
}

static void test_readdir_paging(wfs_fs *fs)
{
    wfs_ino_t d = mk(fs, ROOT_INO, "pg", WFS_FT_DIR);
    char name[16];
    for (int i = 0; i < 50; i++) {
        snprintf(name, sizeof name, "e%02d", i);
        O->forget(fs, mk(fs, d, name, WFS_FT_REG), 1);
    }
    int seen[50] = {0};
    int total = 0;
    uint64_t cookie = 0;
    for (;;) {
        struct collect c = {0, 7, 0, {{0}}};
        EQ(O->readdir(fs, d, cookie, collect_cb, &c), 0);
        if (c.n == 0)
            break;
        CHECK(c.last > cookie);
        cookie = c.last;
        for (int i = 0; i < c.n; i++) {
            int k = atoi(c.names[i] + 1);
            seen[k]++;
            total++;
            EQ(O->unlink(fs, d, c.names[i], (uint32_t)strlen(c.names[i])), 0);
        }
    }
    EQ(total, 50);
    for (int i = 0; i < 50; i++)
        EQ(seen[i], 1);
    EQ(O->rmdir(fs, ROOT_INO, "pg", 2), 0);
    O->forget(fs, d, 1);
}

int main(void)
{
    O = wfs_backend_entry()->ops;
    wfs_fs *fs;
    EQ(O->mount(NULL, NULL, "", &fs), 0);
    test_io(fs);
    test_enospc(fs);
    test_names_and_rename(fs);
    test_readdir_paging(fs);
    /* many files to exercise hash table growth */
    for (int i = 0; i < 3000; i++) {
        char n[16];
        snprintf(n, sizeof n, "h%d", i);
        O->forget(fs, mk(fs, ROOT_INO, n, WFS_FT_REG), 1);
    }
    EQ(fs->icount >= 3000, 1);
    EQ(O->unmount(fs), 0);
    if (failures) {
        fprintf(stderr, "memfs selftest: %d failure(s)\n", failures);
        return 1;
    }
    printf("memfs selftest: OK\n");
    return 0;
}
