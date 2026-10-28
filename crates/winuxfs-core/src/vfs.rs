//! Path-level filesystem layer over an inode-level [`Mount`].
//!
//! # Paths and names
//! Paths are byte strings with `/` separators; a path must start with `/`
//! (the root). `"/"` alone is the root. Every other component must be
//! non-empty and must not be `.` or `..`; violations are `EINVAL`, checked
//! before any backend call. Names are disk names (raw bytes, not necessarily
//! UTF-8); name mapping for the host (Windows) happens at the adapter
//! boundary, not here. Directory listings never contain `.` or `..`.
//! Operations that name an entry (`unlink`, `rmdir`, `rename`) reject `"/"`
//! with `EINVAL`; `create("/")` is `EEXIST`.
//!
//! # Case-insensitive mode
//! With [`VfsOptions::case_insensitive`], a component is first looked up
//! exactly; if absent, the directory is scanned with `readdir` for an entry
//! whose case fold equals the component's fold, and that entry's stored disk
//! name is used. `create` of a name whose fold matches an existing entry is
//! `EEXIST`. The fold maps each `char` through `char::to_lowercase`, which
//! approximates Unicode simple case folding: it differs for a few characters
//! (for example final sigma `ς` and `σ` stay distinct, `ẞ`/`ß` are not
//! unified with `ss`, and dotted capital `İ` lowercases to two code points).
//! Bytes that are not valid UTF-8 are compared exactly. If a directory
//! already holds several entries with equal folds (made in case-sensitive
//! mode), the exact match wins, otherwise the first one in readdir order.
//! A full scan also builds a per-directory index `fold -> disk names` (for
//! up to `INDEX_DIRS` directories), which this Vfs keeps up to date on its
//! own creates, unlinks and renames, so a miss does not rescan the
//! directory. A namespace generation counter keeps a scan that raced with a
//! change from installing an outdated index. Indexes hold no references.
//! Creates and renames in this mode are serialised so two case variants
//! cannot be created concurrently.
//!
//! Rename in this mode: if the destination resolves to the source entry
//! itself (same directory, same disk name), the entry is respelled. If it
//! resolves to another name of the same inode (hard links), the rename is a
//! successful no-op that keeps both names, like rename(2). If it resolves to
//! a different entry with another spelling and `replace` is set, the source
//! first replaces that entry under its stored name, then is respelled to
//! the requested name. These two backend calls are not atomic: if the
//! respelling fails after the replacement succeeded, the rename reports
//! success and the entry keeps the replaced entry's spelling.
//!
//! # Backend references (who owns what)
//! `Mount::lookup`/`Mount::create` return an inode holding one backend
//! reference that must be forgotten exactly once. The Vfs keeps a pin table
//! `ino -> holder count` and owns **exactly one backend reference for each
//! inode in that table**; it calls `forget(ino, 1)` exactly when the holder
//! count drops to zero (or for every remaining inode when the Vfs is
//! dropped). A reference returned by the backend for an inode that is
//! already pinned is forgotten immediately, so no inode is ever held twice.
//! The holders are:
//! - each name-cache entry (one holder per entry);
//! - each inode with at least one open handle (one holder per inode, no
//!   matter how many handles; released at the last close);
//! - transient pins taken while a path is being walked or an entry is being
//!   operated on (released when the operation returns, including on error).
//!
//! The root inode is never looked up and never forgotten: it is always
//! implicitly held and pinning it is a no-op.
//!
//! # Name cache
//! `(parent ino, key) -> (child ino, disk name)`, where the key is the name
//! (or its fold in case-insensitive mode). In case-insensitive mode a hit
//! whose disk name differs from the requested component is confirmed by an
//! exact backend lookup first, so an entry with exactly the requested name
//! wins even when another spelling with the same fold is cached. `unlink`, `rmdir` and `rename`
//! invalidate the affected entries (both old and new names) after the
//! backend call, whatever its result. A global generation counter, bumped by
//! every invalidation, keeps a lookup that raced with a namespace change from
//! inserting a stale entry. When the cache is full it is emptied. The cache
//! assumes this Vfs is the only writer of the mount's namespace.
//!
//! # Handles and concurrency
//! Handles are monotonically increasing `u64`s, never reused within a Vfs.
//! All methods take `&self`; `Vfs` is `Send + Sync`. One mutex guards the pin
//! table, cache and handle table; backend reads and writes run outside it
//! (only `forget` is called under it). Known follow-up (Task 13, under
//! load): `forget` may do backend I/O on real filesystems, and emptying a
//! full cache forgets up to `CACHE_MAX` inodes while holding the mutex,
//! stalling every other call meanwhile; collecting the inodes and
//! forgetting them after unlocking would avoid that. Using a handle concurrently with its
//! own `close` is a caller error (the backend then sees an unreferenced
//! inode and typically reports `ENOENT`).
//!
//! # Read-only mode
//! With [`VfsOptions::read_only`], `create`, `write`, `set_len`, `set_times`,
//! `unlink`, `rmdir` and `rename` return `EROFS` before touching the
//! backend.

use crate::plugin::{DirEntry, FileType, Mount, Stat, Statfs, Timespec, ROOT_INO};
use crate::Errno;
use std::collections::HashMap;
use std::sync::{Arc, Mutex, MutexGuard};
use winuxfs_abi as abi;

/// Maximum number of name-cache entries before the cache is emptied.
const CACHE_MAX: usize = 4096;
/// Page size for case-insensitive directory scans.
const SCAN_PAGE: usize = 256;
/// Maximum number of directories with a case-fold index.
const INDEX_DIRS: usize = 64;

#[derive(Debug, Clone, Copy, Default, PartialEq, Eq)]
pub struct VfsOptions {
    pub case_insensitive: bool,
    pub read_only: bool,
}

/// Opaque handle to an open node.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash, PartialOrd, Ord)]
pub struct Handle(pub u64);

struct OpenNode {
    ino: u64,
}

struct CacheEntry {
    ino: u64,
    /// The name as stored on disk.
    name: Vec<u8>,
}

struct State {
    /// Inodes the Vfs holds one backend reference for, with holder counts.
    pins: HashMap<u64, u64>,
    cache: HashMap<(u64, Vec<u8>), CacheEntry>,
    handles: HashMap<u64, OpenNode>,
    /// Open handle count per inode.
    opens: HashMap<u64, usize>,
    next_handle: u64,
    /// Bumped by every cache invalidation.
    gen: u64,
    /// Case-insensitive mode: dir ino -> fold -> disk names (readdir order).
    index: HashMap<u64, HashMap<Vec<u8>, Vec<Vec<u8>>>>,
    /// Bumped by every namespace change made through this Vfs.
    ns_gen: u64,
}

pub struct Vfs {
    mount: Arc<Mount>,
    opts: VfsOptions,
    state: Mutex<State>,
    /// Serialises create/rename in case-insensitive mode.
    ns: Mutex<()>,
}

/// A transient holder of one pin on `ino`, released on drop.
struct Pin<'a> {
    vfs: &'a Vfs,
    ino: u64,
}

impl Pin<'_> {
    /// Hands the pin to the caller without releasing it.
    fn release(self) -> u64 {
        let ino = self.ino;
        std::mem::forget(self);
        ino
    }
}

impl Drop for Pin<'_> {
    fn drop(&mut self) {
        let mut st = self.vfs.lock();
        self.vfs.unpin(&mut st, self.ino);
    }
}

/// Unicode case fold (see the module docs for its limits); invalid UTF-8
/// bytes are kept as they are.
fn fold(name: &[u8]) -> Vec<u8> {
    let mut out = Vec::with_capacity(name.len());
    let mut buf = [0u8; 4];
    for chunk in name.utf8_chunks() {
        for c in chunk.valid().chars() {
            for l in c.to_lowercase() {
                out.extend_from_slice(l.encode_utf8(&mut buf).as_bytes());
            }
        }
        out.extend_from_slice(chunk.invalid());
    }
    out
}

/// Splits a path into components; `"/"` gives none.
fn parse(path: &[u8]) -> Result<Vec<&[u8]>, Errno> {
    let rest = path.strip_prefix(b"/").ok_or(Errno::EINVAL)?;
    if rest.is_empty() {
        return Ok(Vec::new());
    }
    let comps: Vec<&[u8]> = rest.split(|&b| b == b'/').collect();
    if comps
        .iter()
        .any(|c| c.is_empty() || *c == b"." || *c == b"..")
    {
        return Err(Errno::EINVAL);
    }
    Ok(comps)
}

/// Splits a path naming an entry into (parent components, last component).
fn parse_entry(path: &[u8]) -> Result<(Vec<&[u8]>, &[u8]), Errno> {
    let mut comps = parse(path)?;
    let last = comps.pop().ok_or(Errno::EINVAL)?;
    Ok((comps, last))
}

fn attrs(size: u64, atime: Timespec, mtime: Timespec) -> Stat {
    Stat {
        ino: 0,
        ftype: FileType::Reg,
        mode: 0,
        uid: 0,
        gid: 0,
        nlink: 0,
        size,
        blocks: 0,
        atime,
        mtime,
        ctime: Timespec::default(),
        btime: Timespec::default(),
    }
}

impl Vfs {
    pub fn new(mount: Arc<Mount>, opts: VfsOptions) -> Vfs {
        Vfs {
            mount,
            opts,
            state: Mutex::new(State {
                pins: HashMap::new(),
                cache: HashMap::new(),
                handles: HashMap::new(),
                opens: HashMap::new(),
                next_handle: 1,
                gen: 0,
                index: HashMap::new(),
                ns_gen: 0,
            }),
            ns: Mutex::new(()),
        }
    }

    pub fn options(&self) -> VfsOptions {
        self.opts
    }

    fn lock(&self) -> MutexGuard<'_, State> {
        self.state.lock().unwrap_or_else(|e| e.into_inner())
    }

    fn ns_guard(&self) -> Option<MutexGuard<'_, ()>> {
        self.opts
            .case_insensitive
            .then(|| self.ns.lock().unwrap_or_else(|e| e.into_inner()))
    }

    fn writable(&self) -> Result<(), Errno> {
        if self.opts.read_only {
            Err(Errno::EROFS)
        } else {
            Ok(())
        }
    }

    fn key(&self, name: &[u8]) -> Vec<u8> {
        if self.opts.case_insensitive {
            fold(name)
        } else {
            name.to_vec()
        }
    }

    // ---- pin table (callers hold the state lock) ----

    fn pin_held(st: &mut State, ino: u64) {
        if ino != ROOT_INO {
            *st.pins.get_mut(&ino).expect("pinned inode") += 1;
        }
    }

    /// Takes over one fresh backend reference on `ino` as a new pin.
    fn adopt(&self, st: &mut State, ino: u64) {
        if ino == ROOT_INO {
            self.mount.forget(ino, 1);
            return;
        }
        match st.pins.get_mut(&ino) {
            Some(n) => {
                *n += 1;
                self.mount.forget(ino, 1);
            }
            None => {
                st.pins.insert(ino, 1);
            }
        }
    }

    fn unpin(&self, st: &mut State, ino: u64) {
        if ino == ROOT_INO {
            return;
        }
        let n = st.pins.get_mut(&ino).expect("pinned inode");
        *n -= 1;
        if *n == 0 {
            st.pins.remove(&ino);
            self.mount.forget(ino, 1);
        }
    }

    fn cache_insert(
        &self,
        st: &mut State,
        dir: u64,
        key: Vec<u8>,
        ino: u64,
        name: Vec<u8>,
        gen: u64,
    ) {
        if st.gen != gen || st.cache.contains_key(&(dir, key.clone())) {
            return;
        }
        if st.cache.len() >= CACHE_MAX {
            let old: Vec<u64> = st.cache.drain().map(|(_, e)| e.ino).collect();
            for i in old {
                self.unpin(st, i);
            }
        }
        Self::pin_held(st, ino);
        st.cache.insert((dir, key), CacheEntry { ino, name });
    }

    fn invalidate(&self, dir: u64, names: &[&[u8]]) {
        let keys: Vec<Vec<u8>> = names.iter().map(|n| self.key(n)).collect();
        let mut st = self.lock();
        st.gen += 1;
        for k in keys {
            if let Some(e) = st.cache.remove(&(dir, k)) {
                self.unpin(&mut st, e.ino);
            }
        }
    }

    // ---- lookup ----

    /// Looks up `name` in `dir`, honouring case-insensitive mode. Returns
    /// the child's attributes (holding one fresh backend reference) and its
    /// disk name.
    fn lookup_name(&self, dir: u64, name: &[u8]) -> Result<(Stat, Vec<u8>), Errno> {
        match self.mount.lookup(dir, name) {
            Ok(st) => return Ok((st, name.to_vec())),
            Err(Errno::ENOENT) if self.opts.case_insensitive => {}
            Err(e) => return Err(e),
        }
        let want = fold(name);
        let indexed = self
            .lock()
            .index
            .get(&dir)
            .map(|ix| ix.get(&want).and_then(|v| v.first().cloned()));
        let disk = match indexed {
            Some(Some(d)) => d,
            Some(None) => return Err(Errno::ENOENT),
            None => self.scan(dir, &want)?.ok_or(Errno::ENOENT)?,
        };
        match self.mount.lookup(dir, &disk) {
            Ok(st) => Ok((st, disk)),
            Err(e) => {
                self.lock().index.remove(&dir);
                Err(e)
            }
        }
    }

    /// Reads all of `dir`, installs its fold index (unless the namespace
    /// changed meanwhile) and returns the first entry whose fold is `want`.
    fn scan(&self, dir: u64, want: &[u8]) -> Result<Option<Vec<u8>>, Errno> {
        let gen = self.lock().ns_gen;
        let mut ix: HashMap<Vec<u8>, Vec<Vec<u8>>> = HashMap::new();
        let mut cookie = 0;
        loop {
            let page = self.mount.readdir(dir, cookie, SCAN_PAGE)?;
            let Some(last) = page.last() else { break };
            cookie = last.cookie;
            for e in page {
                ix.entry(fold(&e.name)).or_default().push(e.name);
            }
        }
        let found = ix.get(want).and_then(|v| v.first().cloned());
        let mut st = self.lock();
        if st.ns_gen == gen {
            if st.index.len() >= INDEX_DIRS {
                st.index.clear();
            }
            st.index.insert(dir, ix);
        }
        Ok(found)
    }

    /// Records a namespace change in `dir` for the fold index: on success
    /// `removed` names are gone and `added` exists; on failure the
    /// directory's index is dropped.
    fn ns_changed(&self, dir: u64, ok: bool, removed: &[&[u8]], added: Option<&[u8]>) {
        let mut st = self.lock();
        st.ns_gen += 1;
        if !ok {
            st.index.remove(&dir);
            return;
        }
        let Some(ix) = st.index.get_mut(&dir) else {
            return;
        };
        for n in removed {
            let k = fold(n);
            if let Some(v) = ix.get_mut(&k) {
                v.retain(|x| x.as_slice() != *n);
                if v.is_empty() {
                    ix.remove(&k);
                }
            }
        }
        if let Some(n) = added {
            ix.entry(fold(n)).or_default().push(n.to_vec());
        }
    }

    /// Resolves one component below the pinned directory `dir`.
    fn child(&self, dir: &Pin<'_>, name: &[u8]) -> Result<(Pin<'_>, Vec<u8>), Errno> {
        let key = self.key(name);
        let (gen, hit) = {
            let mut st = self.lock();
            let gen = st.gen;
            match st.cache.get(&(dir.ino, key.clone())) {
                Some(e) if e.name == name => {
                    let ino = e.ino;
                    Self::pin_held(&mut st, ino);
                    return Ok((Pin { vfs: self, ino }, name.to_vec()));
                }
                Some(e) => {
                    let (ino, disk) = (e.ino, e.name.clone());
                    Self::pin_held(&mut st, ino);
                    (gen, Some((Pin { vfs: self, ino }, disk)))
                }
                None => (gen, None),
            }
        };
        if let Some((pin, disk)) = hit {
            // Case-insensitive hit under another spelling: an entry with
            // exactly this name wins (several names may share a fold).
            return match self.mount.lookup(dir.ino, name) {
                Ok(stat) => {
                    drop(pin);
                    let mut st = self.lock();
                    self.adopt(&mut st, stat.ino);
                    Ok((
                        Pin {
                            vfs: self,
                            ino: stat.ino,
                        },
                        name.to_vec(),
                    ))
                }
                Err(Errno::ENOENT) => Ok((pin, disk)),
                Err(e) => Err(e),
            };
        }
        let (stat, disk) = self.lookup_name(dir.ino, name)?;
        let mut st = self.lock();
        self.adopt(&mut st, stat.ino);
        self.cache_insert(&mut st, dir.ino, key, stat.ino, disk.clone(), gen);
        Ok((
            Pin {
                vfs: self,
                ino: stat.ino,
            },
            disk,
        ))
    }

    fn walk(&self, comps: &[&[u8]]) -> Result<Pin<'_>, Errno> {
        let mut cur = Pin {
            vfs: self,
            ino: ROOT_INO,
        };
        for c in comps {
            cur = self.child(&cur, c)?.0;
        }
        Ok(cur)
    }

    /// Turns a pin into an open handle.
    fn register(&self, pin: Pin<'_>) -> Handle {
        let ino = pin.release();
        let mut st = self.lock();
        let id = st.next_handle;
        st.next_handle += 1;
        st.handles.insert(id, OpenNode { ino });
        let n = st.opens.entry(ino).or_insert(0);
        *n += 1;
        if *n > 1 {
            // The inode already has its open-table holder.
            self.unpin(&mut st, ino);
        }
        Handle(id)
    }

    fn ino_of(&self, h: Handle) -> Result<u64, Errno> {
        self.lock()
            .handles
            .get(&h.0)
            .map(|o| o.ino)
            .ok_or(Errno::EBADF)
    }

    // ---- public API ----

    pub fn open(&self, path: &[u8]) -> Result<(Handle, Stat), Errno> {
        let comps = parse(path)?;
        let pin = self.walk(&comps)?;
        let st = self.mount.getattr(pin.ino)?;
        Ok((self.register(pin), st))
    }

    /// Creates `path` (its parent must exist: `ENOENT` otherwise) and opens
    /// it.
    pub fn create(&self, path: &[u8], ftype: FileType, mode: u32) -> Result<(Handle, Stat), Errno> {
        self.writable()?;
        let comps = parse(path)?;
        let Some((last, parent)) = comps.split_last() else {
            return Err(Errno::EEXIST);
        };
        let dir = self.walk(parent)?;
        let _ns = self.ns_guard();
        if self.opts.case_insensitive {
            match self.lookup_name(dir.ino, last) {
                Ok((st, _)) => {
                    self.mount.forget(st.ino, 1);
                    return Err(Errno::EEXIST);
                }
                Err(Errno::ENOENT) => {}
                Err(e) => return Err(e),
            }
        }
        let gen = self.lock().gen;
        let st = self.mount.create(dir.ino, last, ftype, mode, 0, 0)?;
        if self.opts.case_insensitive {
            self.ns_changed(dir.ino, true, &[], Some(last));
        }
        let pin = {
            let mut s = self.lock();
            self.adopt(&mut s, st.ino);
            self.cache_insert(&mut s, dir.ino, self.key(last), st.ino, last.to_vec(), gen);
            Pin {
                vfs: self,
                ino: st.ino,
            }
        };
        Ok((self.register(pin), st))
    }

    /// Closes a handle; the inode's open reference is released at the last
    /// close. Unknown handles are ignored.
    pub fn close(&self, h: Handle) {
        let mut st = self.lock();
        let Some(o) = st.handles.remove(&h.0) else {
            return;
        };
        let n = st.opens.get_mut(&o.ino).expect("open count");
        *n -= 1;
        if *n == 0 {
            st.opens.remove(&o.ino);
            self.unpin(&mut st, o.ino);
        }
    }

    pub fn stat(&self, h: Handle) -> Result<Stat, Errno> {
        self.mount.getattr(self.ino_of(h)?)
    }

    pub fn read(&self, h: Handle, off: u64, buf: &mut [u8]) -> Result<usize, Errno> {
        self.mount.read(self.ino_of(h)?, off, buf)
    }

    pub fn write(&self, h: Handle, off: u64, buf: &[u8]) -> Result<usize, Errno> {
        self.writable()?;
        self.mount.write(self.ino_of(h)?, off, buf)
    }

    pub fn set_len(&self, h: Handle, len: u64) -> Result<(), Errno> {
        self.writable()?;
        let ino = self.ino_of(h)?;
        let a = attrs(len, Timespec::default(), Timespec::default());
        self.mount.setattr(ino, &a, abi::WFS_SET_SIZE)
    }

    /// Sets the given timestamps; `None` leaves one unchanged.
    pub fn set_times(
        &self,
        h: Handle,
        atime: Option<Timespec>,
        mtime: Option<Timespec>,
    ) -> Result<(), Errno> {
        self.writable()?;
        let ino = self.ino_of(h)?;
        let mut mask = 0;
        if atime.is_some() {
            mask |= abi::WFS_SET_ATIME;
        }
        if mtime.is_some() {
            mask |= abi::WFS_SET_MTIME;
        }
        if mask == 0 {
            return Ok(());
        }
        let a = attrs(0, atime.unwrap_or_default(), mtime.unwrap_or_default());
        self.mount.setattr(ino, &a, mask)
    }

    /// Up to `max` entries after `cookie` (0 = from the start), never `.` or
    /// `..`. An empty result means the end of the directory.
    pub fn read_dir(&self, h: Handle, cookie: u64, max: usize) -> Result<Vec<DirEntry>, Errno> {
        let ino = self.ino_of(h)?;
        let mut cookie = cookie;
        loop {
            let page = self.mount.readdir(ino, cookie, max)?;
            let Some(last) = page.last() else {
                return Ok(page);
            };
            cookie = last.cookie;
            let out: Vec<DirEntry> = page
                .into_iter()
                .filter(|e| e.name != b"." && e.name != b"..")
                .collect();
            if !out.is_empty() {
                return Ok(out);
            }
        }
    }

    /// Resolves the disk name of `name` in `dir` (only differs from `name`
    /// in case-insensitive mode). The returned pin keeps the child alive
    /// while the caller operates on it.
    fn disk_name<'a>(
        &'a self,
        dir: &Pin<'a>,
        name: &[u8],
    ) -> Result<(Option<Pin<'a>>, Vec<u8>), Errno> {
        if self.opts.case_insensitive {
            let (p, d) = self.child(dir, name)?;
            Ok((Some(p), d))
        } else {
            Ok((None, name.to_vec()))
        }
    }

    pub fn unlink(&self, path: &[u8]) -> Result<(), Errno> {
        self.writable()?;
        let (parent, last) = parse_entry(path)?;
        let dir = self.walk(&parent)?;
        let (_child, disk) = self.disk_name(&dir, last)?;
        let r = self.mount.unlink(dir.ino, &disk);
        self.invalidate(dir.ino, &[&disk]);
        if self.opts.case_insensitive {
            self.ns_changed(dir.ino, r.is_ok(), &[&disk], None);
        }
        r
    }

    pub fn rmdir(&self, path: &[u8]) -> Result<(), Errno> {
        self.writable()?;
        let (parent, last) = parse_entry(path)?;
        let dir = self.walk(&parent)?;
        let (child, disk) = self.disk_name(&dir, last)?;
        let r = self.mount.rmdir(dir.ino, &disk);
        self.invalidate(dir.ino, &[&disk]);
        if let Some(c) = child {
            self.ns_changed(dir.ino, r.is_ok(), &[&disk], None);
            if r.is_ok() {
                // The removed directory's inode number may be reused.
                self.lock().index.remove(&c.ino);
            }
        }
        r
    }

    /// Renames `old` to `new`. With `replace`, an existing destination is
    /// replaced (subject to the backend's type rules); without it, an
    /// existing destination is `EEXIST`.
    pub fn rename(&self, old: &[u8], new: &[u8], replace: bool) -> Result<(), Errno> {
        self.writable()?;
        let (op, olast) = parse_entry(old)?;
        let (np, nlast) = parse_entry(new)?;
        let odir = self.walk(&op)?;
        let ndir = self.walk(&np)?;
        let flags = if replace {
            0
        } else {
            abi::WFS_RENAME_NOREPLACE
        };
        if !self.opts.case_insensitive {
            let r = self.mount.rename(odir.ino, olast, ndir.ino, nlast, flags);
            self.invalidate(odir.ino, &[olast]);
            self.invalidate(ndir.ino, &[nlast]);
            return r;
        }
        let _ns = self.ns_guard();
        let (src, odisk) = self.child(&odir, olast)?;
        let target = match self.child(&ndir, nlast) {
            Ok((p, d)) => Some((p, d)),
            Err(Errno::ENOENT) => None,
            Err(e) => return Err(e),
        };
        // Outcome for the name cache and fold index: the result, the name
        // the source now has in ndir (if it moved), and the replaced entry
        // (inode, disk name), if any.
        type Outcome<'n> = (Result<(), Errno>, Option<&'n [u8]>, Option<(u64, &'n [u8])>);
        let (r, added, replaced): Outcome<'_> = match &target {
            // The target resolves to the source entry itself: a
            // case-only respelling (a no-op if the spelling is unchanged).
            Some((_, tdisk)) if odir.ino == ndir.ino && *tdisk == odisk => {
                if nlast == odisk.as_slice() {
                    (Ok(()), None, None)
                } else {
                    let r = self.mount.rename(
                        odir.ino,
                        &odisk,
                        ndir.ino,
                        nlast,
                        abi::WFS_RENAME_NOREPLACE,
                    );
                    (r, Some(nlast), None)
                }
            }
            Some(_) if !replace => (Err(Errno::EEXIST), None, None),
            // Another name of the same inode (hard links): like
            // rename(2), a successful no-op that keeps both names.
            Some((t, _)) if t.ino == src.ino => (Ok(()), None, None),
            Some((t, tdisk)) => {
                let victim = Some((t.ino, tdisk.as_slice()));
                match self.mount.rename(odir.ino, &odisk, ndir.ino, tdisk, 0) {
                    Err(e) => (Err(e), None, None),
                    Ok(()) if tdisk.as_slice() == nlast => (Ok(()), Some(nlast), victim),
                    Ok(()) => {
                        // Take the requested spelling. If that fails, the
                        // replacement has still happened: report success
                        // with the replaced entry's spelling.
                        let respelled = self.mount.rename(
                            ndir.ino,
                            tdisk,
                            ndir.ino,
                            nlast,
                            abi::WFS_RENAME_NOREPLACE,
                        );
                        let now = if respelled.is_ok() { nlast } else { tdisk };
                        (Ok(()), Some(now), victim)
                    }
                }
            }
            None => {
                let r = self.mount.rename(odir.ino, &odisk, ndir.ino, nlast, flags);
                (r, Some(nlast), None)
            }
        };
        let names: Vec<&[u8]> = target
            .iter()
            .map(|(_, d)| d.as_slice())
            .chain([nlast])
            .collect();
        self.invalidate(odir.ino, &[&odisk]);
        self.invalidate(ndir.ino, &names);
        let ok = r.is_ok();
        if added.is_some() || !ok {
            self.ns_changed(odir.ino, ok, &[&odisk], None);
        }
        let removed: Vec<&[u8]> = replaced.iter().map(|&(_, d)| d).collect();
        if added.is_some() || !ok {
            self.ns_changed(ndir.ino, ok, &removed, added);
        }
        if let Some((ino, _)) = replaced {
            // A replaced (empty) directory's inode number may be reused.
            self.lock().index.remove(&ino);
        }
        r
    }

    pub fn statfs(&self) -> Result<Statfs, Errno> {
        self.mount.statfs()
    }
}

impl Drop for Vfs {
    fn drop(&mut self) {
        let st = self.state.get_mut().unwrap_or_else(|e| e.into_inner());
        for &ino in st.pins.keys() {
            self.mount.forget(ino, 1);
        }
        st.pins.clear();
        st.cache.clear();
        st.handles.clear();
        st.opens.clear();
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn parse_paths() {
        assert_eq!(parse(b"/"), Ok(vec![]));
        assert_eq!(parse(b"/a/bc"), Ok(vec![&b"a"[..], b"bc"]));
        for bad in [&b""[..], b"a", b"/a/", b"//", b"/a//b", b"/.", b"/a/.."] {
            assert_eq!(parse(bad), Err(Errno::EINVAL), "{bad:?}");
        }
        assert_eq!(parse_entry(b"/").map(|_| ()), Err(Errno::EINVAL));
    }

    #[test]
    fn fold_is_case_insensitive_and_keeps_bad_bytes() {
        assert_eq!(fold(b"FoO"), b"foo");
        assert_eq!(fold("ÄRGER".as_bytes()), "ärger".as_bytes());
        assert_eq!(fold(&[0x46, 0xFF, 0x41]), vec![0x66, 0xFF, 0x61]);
        assert_ne!(fold(&[0xFF]), fold(&[0xFE]));
    }

    #[test]
    fn vfs_is_send_sync() {
        fn check<T: Send + Sync>() {}
        check::<Vfs>();
    }
}
