/* memfs_counting: memfs with call counters, for the core's Vfs tests.
 *
 * It includes memfs.c and wraps its ops table. Counters are library-global
 * (tests load a private copy of the library per counter-checking test):
 *   memfs_counting_refs_taken()     successful lookup + create + symlink calls,
 *                                   i.e. lookup references handed out
 *   memfs_counting_refs_forgotten() sum of n over all forget(ino, n) calls
 *   memfs_counting_mutations()      calls (successful or not) of every
 *                                   write-path op: setattr, write, create,
 *                                   unlink, rmdir, rename, link, symlink
 * Every other behaviour is memfs's.
 */
#define wfs_backend_entry memfs_plain_entry
#include "../memfs/memfs.c"
#undef wfs_backend_entry

#include <stdatomic.h>

static atomic_ullong refs_taken, refs_forgotten, mutations;

WFS_EXPORT unsigned long long memfs_counting_refs_taken(void) { return atomic_load(&refs_taken); }
WFS_EXPORT unsigned long long memfs_counting_refs_forgotten(void) { return atomic_load(&refs_forgotten); }
WFS_EXPORT unsigned long long memfs_counting_mutations(void) { return atomic_load(&mutations); }

static void mut(void) { atomic_fetch_add(&mutations, 1); }

static wfs_err_t c_lookup(wfs_fs *fs, wfs_ino_t dir, const char *name, uint32_t len, wfs_ino_t *out, wfs_stat *st)
{
    wfs_err_t r = m_lookup(fs, dir, name, len, out, st);
    if (r == 0)
        atomic_fetch_add(&refs_taken, 1);
    return r;
}

static void c_forget(wfs_fs *fs, wfs_ino_t ino, uint64_t n)
{
    atomic_fetch_add(&refs_forgotten, n);
    m_forget(fs, ino, n);
}

static wfs_err_t c_setattr(wfs_fs *fs, wfs_ino_t ino, const wfs_stat *st, uint32_t mask)
{
    mut();
    return m_setattr(fs, ino, st, mask);
}

static wfs_err_t c_write(wfs_fs *fs, wfs_ino_t ino, uint64_t off, const void *buf, size_t len, size_t *done)
{
    mut();
    return m_write(fs, ino, off, buf, len, done);
}

static wfs_err_t c_create(wfs_fs *fs, wfs_ino_t dir, const char *name, uint32_t len, uint32_t ftype,
                          uint32_t mode, uint32_t uid, uint32_t gid, wfs_ino_t *out, wfs_stat *st)
{
    mut();
    wfs_err_t r = m_create(fs, dir, name, len, ftype, mode, uid, gid, out, st);
    if (r == 0)
        atomic_fetch_add(&refs_taken, 1);
    return r;
}

static wfs_err_t c_unlink(wfs_fs *fs, wfs_ino_t dir, const char *name, uint32_t len)
{
    mut();
    return m_unlink(fs, dir, name, len);
}

static wfs_err_t c_rmdir(wfs_fs *fs, wfs_ino_t dir, const char *name, uint32_t len)
{
    mut();
    return m_rmdir(fs, dir, name, len);
}

static wfs_err_t c_rename(wfs_fs *fs, wfs_ino_t odir, const char *oname, uint32_t olen,
                          wfs_ino_t ndir, const char *nname, uint32_t nlen, uint32_t flags)
{
    mut();
    return m_rename(fs, odir, oname, olen, ndir, nname, nlen, flags);
}

static wfs_err_t c_link(wfs_fs *fs, wfs_ino_t ino, wfs_ino_t ndir, const char *name, uint32_t len)
{
    mut();
    return m_link(fs, ino, ndir, name, len);
}

static wfs_err_t c_symlink(wfs_fs *fs, wfs_ino_t dir, const char *name, uint32_t len, const char *target,
                           uint32_t tlen, uint32_t uid, uint32_t gid, wfs_ino_t *out, wfs_stat *st)
{
    mut();
    wfs_err_t r = m_symlink(fs, dir, name, len, target, tlen, uid, gid, out, st);
    if (r == 0)
        atomic_fetch_add(&refs_taken, 1);
    return r;
}

static const wfs_ops counting_ops = {
    .probe = m_probe,
    .mount = m_mount,
    .unmount = m_unmount,
    .statfs = m_statfs,
    .getattr = m_getattr,
    .setattr = c_setattr,
    .lookup = c_lookup,
    .forget = c_forget,
    .readdir = m_readdir,
    .read = m_read,
    .write = c_write,
    .create = c_create,
    .unlink = c_unlink,
    .rmdir = c_rmdir,
    .rename = c_rename,
    .link = c_link,
    .symlink = c_symlink,
    .readlink = m_readlink,
    .sync = m_sync,
    .fsync = m_fsync,
};

static const wfs_backend_desc counting_desc = {
    .abi_version = WFS_ABI_VERSION,
    .size = sizeof(wfs_backend_desc),
    .name = "memfs_counting",
    .caps = WFS_CAP_HARDLINK | WFS_CAP_SYMLINK,
    .dev_allowed = 0,
    .dev_required = 0,
    .ops = &counting_ops,
};

WFS_EXPORT const wfs_backend_desc *wfs_backend_entry(void) { return &counting_desc; }
