//! Backend plugin loader (`wfs_backend_entry`) and the safe `Mount` wrapper.
//!
//! Lifetimes: a [`Backend`] and every [`Mount`] made from it share one
//! `Arc<Loaded>`, which owns the `libloading::Library`. The library is
//! unloaded only after the last of them is dropped, so a `Mount` may outlive
//! the `Backend` value it came from.

use crate::hostapi::{host_api, BlkHandle};
use crate::{BlockDevice, Errno};
use libloading::Library;
use std::collections::HashSet;
use std::ffi::{c_int, c_void, CStr, CString};
use std::fmt;
use std::mem::MaybeUninit;
use std::panic::{catch_unwind, AssertUnwindSafe};
use std::path::{Path, PathBuf};
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::{Arc, Mutex, MutexGuard};
use winuxfs_abi as abi;

/// Root inode number every backend must expose.
pub const ROOT_INO: u64 = 1;

/// Longest name (in bytes) a backend accepts or reports (`NAME_MAX`).
pub const NAME_MAX: usize = 255;

/// Buffer size `Mount::readlink` offers the backend (Linux `PATH_MAX`).
pub const READLINK_MAX: usize = 4096;

/// Highest Linux errno value (`EHWPOISON`).
///
/// Per docs/abi.md a backend failure is `-(Linux errno value)`. The "known
/// set" of errnos the core passes through unchanged is therefore the whole
/// Linux errno range `1..=MAX_ERRNO` (EPERM, EACCES, EXDEV, ENOMEM, EUCLEAN,
/// ... included), not only the constants named in `Errno`.
pub const MAX_ERRNO: i32 = 133;

/// Maps a backend return value: 0 is success, `-e` with `e` in
/// `1..=MAX_ERRNO` is `Err(e)`; any positive value or a negative value
/// outside the Linux errno range (including `i32::MIN`) is `Err(EIO)`.
pub(crate) fn ret(r: abi::wfs_err_t) -> Result<(), Errno> {
    if r == 0 {
        return Ok(());
    }
    match r.checked_neg() {
        Some(e) if (1..=MAX_ERRNO).contains(&e) => Err(Errno(e)),
        _ => Err(Errno::EIO),
    }
}

pub struct Devices {
    pub data: Option<Arc<dyn BlockDevice>>,
    pub log: Option<Arc<dyn BlockDevice>>,
    pub rt: Option<Arc<dyn BlockDevice>>,
}

impl Devices {
    fn roles(&self) -> [Option<&Arc<dyn BlockDevice>>; 3] {
        [self.data.as_ref(), self.log.as_ref(), self.rt.as_ref()]
    }
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub enum PluginError {
    Load(String),
    MissingEntry,
    AbiMismatch { plugin: u32, host: u32 },
    Invalid(&'static str),
}

impl fmt::Display for PluginError {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            PluginError::Load(m) => write!(f, "cannot load plugin: {m}"),
            PluginError::MissingEntry => write!(f, "plugin does not export wfs_backend_entry"),
            PluginError::AbiMismatch { plugin, host } => {
                write!(f, "plugin ABI version {plugin}, host expects {host}")
            }
            PluginError::Invalid(m) => write!(f, "invalid plugin: {m}"),
        }
    }
}

impl std::error::Error for PluginError {}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum FileType {
    Reg,
    Dir,
    Lnk,
    Chr,
    Blk,
    Fifo,
    Sock,
}

impl FileType {
    fn from_abi(v: u32) -> Result<FileType, Errno> {
        Ok(match v {
            abi::WFS_FT_REG => FileType::Reg,
            abi::WFS_FT_DIR => FileType::Dir,
            abi::WFS_FT_LNK => FileType::Lnk,
            abi::WFS_FT_CHR => FileType::Chr,
            abi::WFS_FT_BLK => FileType::Blk,
            abi::WFS_FT_FIFO => FileType::Fifo,
            abi::WFS_FT_SOCK => FileType::Sock,
            _ => return Err(Errno::EIO),
        })
    }

    fn to_abi(self) -> u32 {
        match self {
            FileType::Reg => abi::WFS_FT_REG,
            FileType::Dir => abi::WFS_FT_DIR,
            FileType::Lnk => abi::WFS_FT_LNK,
            FileType::Chr => abi::WFS_FT_CHR,
            FileType::Blk => abi::WFS_FT_BLK,
            FileType::Fifo => abi::WFS_FT_FIFO,
            FileType::Sock => abi::WFS_FT_SOCK,
        }
    }
}

#[derive(Debug, Clone, Copy, PartialEq, Eq, Default)]
pub struct Timespec {
    pub sec: i64,
    pub nsec: u32,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Stat {
    pub ino: u64,
    pub ftype: FileType,
    pub mode: u32,
    pub uid: u32,
    pub gid: u32,
    pub nlink: u32,
    pub size: u64,
    pub blocks: u64,
    pub atime: Timespec,
    pub mtime: Timespec,
    pub ctime: Timespec,
    pub btime: Timespec,
}

fn ts_from(t: abi::wfs_time) -> Timespec {
    Timespec {
        sec: t.sec,
        nsec: t.nsec,
    }
}

fn ts_to(t: Timespec) -> abi::wfs_time {
    abi::wfs_time {
        sec: t.sec,
        nsec: t.nsec,
    }
}

impl Stat {
    fn from_abi(s: &abi::wfs_stat) -> Result<Stat, Errno> {
        Ok(Stat {
            ino: s.ino,
            ftype: FileType::from_abi(s.ftype)?,
            mode: s.mode,
            uid: s.uid,
            gid: s.gid,
            nlink: s.nlink,
            size: s.size,
            blocks: s.blocks,
            atime: ts_from(s.atime),
            mtime: ts_from(s.mtime),
            ctime: ts_from(s.ctime),
            btime: ts_from(s.btime),
        })
    }

    fn to_abi(&self) -> abi::wfs_stat {
        abi::wfs_stat {
            ino: self.ino,
            ftype: self.ftype.to_abi(),
            mode: self.mode,
            uid: self.uid,
            gid: self.gid,
            nlink: self.nlink,
            rdev_major: 0,
            rdev_minor: 0,
            size: self.size,
            blocks: self.blocks,
            atime: ts_to(self.atime),
            mtime: ts_to(self.mtime),
            ctime: ts_to(self.ctime),
            btime: ts_to(self.btime),
        }
    }
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Statfs {
    pub total_bytes: u64,
    pub free_bytes: u64,
    pub avail_bytes: u64,
    pub total_inodes: u64,
    pub free_inodes: u64,
    pub block_size: u32,
    pub name_max: u32,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct DirEntry {
    /// Cookie to pass to `readdir` to continue after this entry.
    pub cookie: u64,
    pub ino: u64,
    pub ftype: FileType,
    pub name: Vec<u8>,
}

/// State shared by a `Backend` and all its `Mount`s.
struct Loaded {
    ops: abi::wfs_ops,
    name: String,
    caps: u32,
    dev_allowed: u32,
    dev_required: u32,
    /// Present iff the backend declared `WFS_CAP_SINGLE_THREADED`: every call
    /// into the backend (probe, mount, ops, unmount) holds it.
    serial: Option<Mutex<()>>,
    // Must stay last-dropped relative to any use of `ops`; it is only dropped
    // when the Arc<Loaded> goes away, i.e. after every Backend and Mount.
    _lib: Library,
}

impl Loaded {
    fn guard(&self) -> Option<MutexGuard<'_, ()>> {
        self.serial
            .as_ref()
            .map(|m| m.lock().unwrap_or_else(|e| e.into_inner()))
    }
}

pub struct Backend {
    inner: Arc<Loaded>,
}

impl fmt::Debug for Backend {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.debug_struct("Backend")
            .field("name", &self.inner.name)
            .field("caps", &self.inner.caps)
            .finish()
    }
}

const ENTRY: &[u8] = b"wfs_backend_entry";

/// Loads a plugin and validates its descriptor.
pub fn load_plugin(path: &Path) -> Result<Backend, PluginError> {
    // SAFETY: loading a library runs its initialisers; plugins are trusted
    // code by design (the user chose to install them).
    let lib = unsafe { Library::new(path) }.map_err(|e| PluginError::Load(e.to_string()))?;
    let entry: unsafe extern "C" fn() -> *const abi::wfs_backend_desc = unsafe {
        match lib.get::<unsafe extern "C" fn() -> *const abi::wfs_backend_desc>(ENTRY) {
            Ok(f) => *f,
            Err(_) => return Err(PluginError::MissingEntry),
        }
    };
    let desc = unsafe { entry() };
    if desc.is_null() {
        return Err(PluginError::Invalid("null descriptor"));
    }
    // Read the two leading u32 fields first: they are stable across versions,
    // and a smaller-than-expected descriptor must not be read in full.
    let (version, size) = unsafe {
        let p = desc.cast::<u32>();
        (p.read(), p.add(1).read())
    };
    if version != abi::WFS_ABI_VERSION {
        return Err(PluginError::AbiMismatch {
            plugin: version,
            host: abi::WFS_ABI_VERSION,
        });
    }
    if (size as usize) < std::mem::size_of::<abi::wfs_backend_desc>() {
        return Err(PluginError::Invalid("descriptor too small"));
    }
    let d = unsafe { &*desc };
    if d.name.is_null() {
        return Err(PluginError::Invalid("null name"));
    }
    let name = unsafe { CStr::from_ptr(d.name) }
        .to_str()
        .map_err(|_| PluginError::Invalid("name is not UTF-8"))?
        .to_owned();
    if d.ops.is_null() {
        return Err(PluginError::Invalid("null ops"));
    }
    let ops = unsafe { *d.ops };
    let mandatory = [
        ops.mount.is_some(),
        ops.unmount.is_some(),
        ops.getattr.is_some(),
        ops.lookup.is_some(),
        ops.forget.is_some(),
        ops.readdir.is_some(),
        ops.statfs.is_some(),
    ];
    if mandatory.contains(&false) {
        return Err(PluginError::Invalid("missing mandatory op"));
    }
    let all_roles = (1u32 << abi::WFS_DEV_COUNT) - 1;
    if d.dev_allowed & !all_roles != 0 || d.dev_required & !d.dev_allowed != 0 {
        return Err(PluginError::Invalid("bad device role masks"));
    }
    let serial = (d.caps & abi::WFS_CAP_SINGLE_THREADED != 0).then(|| Mutex::new(()));
    Ok(Backend {
        inner: Arc::new(Loaded {
            ops,
            name,
            caps: d.caps,
            dev_allowed: d.dev_allowed,
            dev_required: d.dev_required,
            serial,
            _lib: lib,
        }),
    })
}

fn is_plugin_name(p: &Path) -> bool {
    let Some(name) = p.file_name().and_then(|n| n.to_str()) else {
        return false;
    };
    let ext_ok = p
        .extension()
        .and_then(|e| e.to_str())
        .is_some_and(|e| e.eq_ignore_ascii_case("dll") || e == "so");
    ext_ok && name.starts_with("winuxfs_backend_")
}

/// Scans `dirs` for `winuxfs_backend_*.{dll,so}` and tries to load each one.
/// Missing directories are skipped; other directory errors are reported as
/// `Load` errors against the directory path. Results are sorted by path.
pub fn discover(dirs: &[PathBuf]) -> Vec<(PathBuf, Result<Backend, PluginError>)> {
    let mut out = Vec::new();
    for dir in dirs {
        let rd = match std::fs::read_dir(dir) {
            Ok(rd) => rd,
            Err(e) if e.kind() == std::io::ErrorKind::NotFound => continue,
            Err(e) => {
                out.push((dir.clone(), Err(PluginError::Load(e.to_string()))));
                continue;
            }
        };
        let mut paths: Vec<PathBuf> = rd
            .filter_map(|e| e.ok())
            .map(|e| e.path())
            .filter(|p| p.is_file() && is_plugin_name(p))
            .collect();
        paths.sort();
        for p in paths {
            let r = load_plugin(&p);
            out.push((p, r));
        }
    }
    out
}

/// Builds the `wfs_blkdev *devs[WFS_DEV_COUNT]` array; the handles must
/// outlive every use of the pointers.
fn handles(devs: &Devices) -> [Option<BlkHandle>; 3] {
    devs.roles().map(|d| d.map(|d| BlkHandle::new(d.clone())))
}

fn ptrs(h: &[Option<BlkHandle>; 3]) -> [*mut abi::wfs_blkdev; 3] {
    [0, 1, 2].map(|i| h[i].as_ref().map_or(std::ptr::null_mut(), |h| h.as_ptr()))
}

impl Backend {
    pub fn name(&self) -> &str {
        &self.inner.name
    }

    pub fn caps(&self) -> u32 {
        self.inner.caps
    }

    /// `Ok(true)` if the backend recognises the devices, `Ok(false)` if it
    /// returned `-EINVAL`, `Err(ENOSYS)` if it has no `probe`.
    pub fn probe(&self, devs: &Devices) -> Result<bool, Errno> {
        let f = self.inner.ops.probe.ok_or(Errno::ENOSYS)?;
        let h = handles(devs);
        let mut p = ptrs(&h);
        let r = {
            let _g = self.inner.guard();
            unsafe { f(host_api(), p.as_mut_ptr()) }
        };
        match ret(r) {
            Ok(()) => Ok(true),
            Err(Errno::EINVAL) => Ok(false),
            Err(e) => Err(e),
        }
    }

    /// Mounts. `EINVAL` if a role in `dev_required` is empty, a role not in
    /// `dev_allowed` is given, or `options` contains a NUL byte.
    pub fn mount(&self, devs: Devices, options: &str) -> Result<Mount, Errno> {
        let l = &self.inner;
        for (i, d) in devs.roles().iter().enumerate() {
            let bit = 1u32 << i;
            if (l.dev_required & bit != 0 && d.is_none())
                || (d.is_some() && l.dev_allowed & bit == 0)
            {
                return Err(Errno::EINVAL);
            }
        }
        let opts = CString::new(options).map_err(|_| Errno::EINVAL)?;
        let f = l.ops.mount.ok_or(Errno::ENOSYS)?;
        let h = handles(&devs);
        let mut p = ptrs(&h);
        let mut fs: *mut abi::wfs_fs = std::ptr::null_mut();
        let r = {
            let _g = l.guard();
            unsafe { f(host_api(), p.as_mut_ptr(), opts.as_ptr(), &mut fs) }
        };
        ret(r)?;
        if fs.is_null() {
            // Success without a filesystem handle: nothing to unmount.
            return Err(Errno::EIO);
        }
        let m = Mount {
            inner: self.inner.clone(),
            fs,
            unmounted: AtomicBool::new(false),
            _devs: h,
        };
        // ABI rule: the root inode is ROOT_INO and it is a directory. On
        // violation `m` is dropped, which unmounts it again.
        let root = m.getattr(ROOT_INO)?;
        if root.ftype != FileType::Dir {
            return Err(Errno::EIO);
        }
        Ok(m)
    }
}

/// A mounted filesystem. Dropping it calls the backend's `unmount` once.
pub struct Mount {
    inner: Arc<Loaded>,
    /// Opaque backend handle; never dereferenced by the core.
    fs: *mut abi::wfs_fs,
    /// Set once the backend's `unmount` has run; no call may follow it.
    unmounted: AtomicBool,
    /// Keeps the `wfs_blkdev` pointers handed to the backend valid until
    /// after `unmount`.
    _devs: [Option<BlkHandle>; 3],
}

// SAFETY: `fs` is an opaque handle only passed back to the backend. The ABI
// (docs/abi.md) requires backends to accept concurrent calls from any thread
// unless they declare WFS_CAP_SINGLE_THREADED, in which case every call is
// made under `Loaded::serial`. Block devices are `Send + Sync`.
unsafe impl Send for Mount {}
unsafe impl Sync for Mount {}

/// Length of a symlink target (not a name: no character rules).
fn name_len(name: &[u8]) -> Result<u32, Errno> {
    u32::try_from(name.len()).map_err(|_| Errno::ENAMETOOLONG)
}

/// Validates a directory entry name handed to the backend: at most
/// `NAME_MAX` bytes (`ENAMETOOLONG`), non-empty, no NUL, no '/' (`EINVAL`).
fn entry_name_len(name: &[u8]) -> Result<u32, Errno> {
    if name.len() > NAME_MAX {
        return Err(Errno::ENAMETOOLONG);
    }
    if name.is_empty() || name.iter().any(|&b| b == 0 || b == b'/') {
        return Err(Errno::EINVAL);
    }
    Ok(name.len() as u32)
}

fn zeroed_stat() -> abi::wfs_stat {
    // SAFETY: wfs_stat is plain integers; all-zero is a valid value.
    unsafe { MaybeUninit::zeroed().assume_init() }
}

/// Exclusive upper bound of readdir cookies (docs/abi.md).
const COOKIE_LIMIT: u64 = 1 << 63;

struct ReaddirCtx {
    out: Vec<DirEntry>,
    max: usize,
    bad: bool,
    /// The cookie this call resumed from.
    start: u64,
    seen: HashSet<u64>,
}

unsafe extern "C" fn readdir_cb(ctx: *mut c_void, ent: *const abi::wfs_dirent) -> c_int {
    if ctx.is_null() {
        return 1;
    }
    let c = &mut *(ctx as *mut ReaddirCtx);
    // A backend that ignores the stop signal must not grow the result past
    // `max`, nor add entries after a malformed one.
    if c.bad || c.out.len() >= c.max {
        return 1;
    }
    let r = catch_unwind(AssertUnwindSafe(|| {
        if ent.is_null() {
            return Err(());
        }
        let e = &*ent;
        // ABI rules: names are 1..=NAME_MAX bytes without NUL or '/', inode
        // numbers are nonzero, and every cookie is nonzero (0 means "start")
        // and new: it differs from the cookie passed in and from every
        // earlier cookie of this call. Otherwise a listing would loop.
        if e.name.is_null()
            || e.name_len == 0
            || e.name_len as usize > NAME_MAX
            || e.ino == 0
            || e.next_cookie == 0
            || e.next_cookie == c.start
            || !c.seen.insert(e.next_cookie)
        {
            return Err(());
        }
        let name = std::slice::from_raw_parts(e.name.cast::<u8>(), e.name_len as usize).to_vec();
        if name.iter().any(|&b| b == 0 || b == b'/') {
            return Err(());
        }
        let ftype = FileType::from_abi(e.ftype).map_err(|_| ())?;
        // ABI rule: cookies are < 2^63. The Windows adapter reserves the top
        // of the range for its "." / ".." markers; a larger cookie would make
        // a listing restart forever, so it is a malformed entry (EIO).
        if e.next_cookie >= COOKIE_LIMIT {
            return Err(());
        }
        Ok(DirEntry {
            cookie: e.next_cookie,
            ino: e.ino,
            ftype,
            name,
        })
    }));
    match r {
        Ok(Ok(d)) => {
            c.out.push(d);
            (c.out.len() >= c.max) as c_int
        }
        _ => {
            c.bad = true;
            1
        }
    }
}

impl Mount {
    /// Calls into the backend, holding the serial lock if required.
    fn call<R>(&self, f: impl FnOnce() -> R) -> R {
        let _g = self.inner.guard();
        f()
    }

    fn ops(&self) -> &abi::wfs_ops {
        &self.inner.ops
    }

    pub fn getattr(&self, ino: u64) -> Result<Stat, Errno> {
        let f = self.ops().getattr.ok_or(Errno::ENOSYS)?;
        let mut st = zeroed_stat();
        ret(self.call(|| unsafe { f(self.fs, ino, &mut st) }))?;
        Stat::from_abi(&st)
    }

    /// Returns the child's attributes; `Stat.ino` is the child inode.
    pub fn lookup(&self, dir: u64, name: &[u8]) -> Result<Stat, Errno> {
        let f = self.ops().lookup.ok_or(Errno::ENOSYS)?;
        let len = entry_name_len(name)?;
        let mut st = zeroed_stat();
        let mut ino = 0;
        ret(self.call(|| unsafe { f(self.fs, dir, name.as_ptr().cast(), len, &mut ino, &mut st) }))?;
        self.child_stat(ino, &st)
    }

    /// Converts the result of a successful reference-returning op
    /// (`lookup`/`create`). An out inode of 0 is `EIO`. If the stat is
    /// malformed, the reference the backend handed out is released with
    /// `forget(ino, 1)` before returning `EIO`.
    fn child_stat(&self, ino: u64, st: &abi::wfs_stat) -> Result<Stat, Errno> {
        if ino == 0 {
            return Err(Errno::EIO);
        }
        match Stat::from_abi(st) {
            Ok(mut s) => {
                s.ino = ino;
                Ok(s)
            }
            Err(e) => {
                self.forget(ino, 1);
                Err(e)
            }
        }
    }

    pub fn forget(&self, ino: u64, n: u64) {
        if let Some(f) = self.ops().forget {
            self.call(|| unsafe { f(self.fs, ino, n) });
        }
    }

    /// Up to `max` entries following `cookie` (0 = from the start).
    pub fn readdir(&self, dir: u64, cookie: u64, max: usize) -> Result<Vec<DirEntry>, Errno> {
        let f = self.ops().readdir.ok_or(Errno::ENOSYS)?;
        if max == 0 {
            return Ok(Vec::new());
        }
        let mut ctx = ReaddirCtx {
            out: Vec::new(),
            max,
            bad: false,
            start: cookie,
            seen: HashSet::new(),
        };
        let p = &mut ctx as *mut ReaddirCtx as *mut c_void;
        let r = self.call(|| unsafe { f(self.fs, dir, cookie, Some(readdir_cb), p) });
        ret(r)?;
        if ctx.bad {
            return Err(Errno::EIO);
        }
        Ok(ctx.out)
    }

    pub fn read(&self, ino: u64, off: u64, buf: &mut [u8]) -> Result<usize, Errno> {
        let f = self.ops().read.ok_or(Errno::ENOSYS)?;
        let mut done = 0usize;
        let len = buf.len();
        let p = buf.as_mut_ptr().cast();
        ret(self.call(|| unsafe { f(self.fs, ino, off, p, len, &mut done) }))?;
        if done > len {
            return Err(Errno::EIO);
        }
        Ok(done)
    }

    pub fn write(&self, ino: u64, off: u64, buf: &[u8]) -> Result<usize, Errno> {
        let f = self.ops().write.ok_or(Errno::ENOSYS)?;
        let mut done = 0usize;
        let len = buf.len();
        ret(self.call(|| unsafe { f(self.fs, ino, off, buf.as_ptr().cast(), len, &mut done) }))?;
        if done > len {
            return Err(Errno::EIO);
        }
        Ok(done)
    }

    pub fn create(
        &self,
        dir: u64,
        name: &[u8],
        ftype: FileType,
        mode: u32,
        uid: u32,
        gid: u32,
    ) -> Result<Stat, Errno> {
        let f = self.ops().create.ok_or(Errno::ENOSYS)?;
        let len = entry_name_len(name)?;
        let mut st = zeroed_stat();
        let mut ino = 0;
        let ft = ftype.to_abi();
        ret(self.call(|| unsafe {
            f(
                self.fs,
                dir,
                name.as_ptr().cast(),
                len,
                ft,
                mode,
                uid,
                gid,
                &mut ino,
                &mut st,
            )
        }))?;
        self.child_stat(ino, &st)
    }

    pub fn unlink(&self, dir: u64, name: &[u8]) -> Result<(), Errno> {
        let f = self.ops().unlink.ok_or(Errno::ENOSYS)?;
        let len = entry_name_len(name)?;
        ret(self.call(|| unsafe { f(self.fs, dir, name.as_ptr().cast(), len) }))
    }

    pub fn rmdir(&self, dir: u64, name: &[u8]) -> Result<(), Errno> {
        let f = self.ops().rmdir.ok_or(Errno::ENOSYS)?;
        let len = entry_name_len(name)?;
        ret(self.call(|| unsafe { f(self.fs, dir, name.as_ptr().cast(), len) }))
    }

    pub fn rename(
        &self,
        odir: u64,
        oname: &[u8],
        ndir: u64,
        nname: &[u8],
        flags: u32,
    ) -> Result<(), Errno> {
        let f = self.ops().rename.ok_or(Errno::ENOSYS)?;
        let (ol, nl) = (entry_name_len(oname)?, entry_name_len(nname)?);
        ret(self.call(|| unsafe {
            f(
                self.fs,
                odir,
                oname.as_ptr().cast(),
                ol,
                ndir,
                nname.as_ptr().cast(),
                nl,
                flags,
            )
        }))
    }

    /// Applies the fields of `attrs` selected by `mask` (`WFS_SET_*`).
    pub fn setattr(&self, ino: u64, attrs: &Stat, mask: u32) -> Result<(), Errno> {
        let f = self.ops().setattr.ok_or(Errno::ENOSYS)?;
        let st = attrs.to_abi();
        ret(self.call(|| unsafe { f(self.fs, ino, &st, mask) }))
    }

    pub fn statfs(&self) -> Result<Statfs, Errno> {
        let f = self.ops().statfs.ok_or(Errno::ENOSYS)?;
        // SAFETY: wfs_statfs is plain integers; all-zero is valid.
        let mut s: abi::wfs_statfs = unsafe { MaybeUninit::zeroed().assume_init() };
        ret(self.call(|| unsafe { f(self.fs, &mut s) }))?;
        Ok(Statfs {
            total_bytes: s.total_bytes,
            free_bytes: s.free_bytes,
            avail_bytes: s.avail_bytes,
            total_inodes: s.total_inodes,
            free_inodes: s.free_inodes,
            block_size: s.block_size,
            name_max: s.name_max,
        })
    }

    pub fn sync(&self) -> Result<(), Errno> {
        let f = self.ops().sync.ok_or(Errno::ENOSYS)?;
        ret(self.call(|| unsafe { f(self.fs) }))
    }

    pub fn fsync(&self, ino: u64, datasync: bool) -> Result<(), Errno> {
        let f = self.ops().fsync.ok_or(Errno::ENOSYS)?;
        ret(self.call(|| unsafe { f(self.fs, ino, datasync as c_int) }))
    }

    /// Adds the name `name` in `ndir` for the existing inode `ino`. Returns no
    /// new reference.
    pub fn link(&self, ino: u64, ndir: u64, name: &[u8]) -> Result<(), Errno> {
        let f = self.ops().link.ok_or(Errno::ENOSYS)?;
        let len = entry_name_len(name)?;
        ret(self.call(|| unsafe { f(self.fs, ino, ndir, name.as_ptr().cast(), len) }))
    }

    /// Creates a symlink; like `create`, the result carries one reference.
    pub fn symlink(
        &self,
        dir: u64,
        name: &[u8],
        target: &[u8],
        uid: u32,
        gid: u32,
    ) -> Result<Stat, Errno> {
        let f = self.ops().symlink.ok_or(Errno::ENOSYS)?;
        let len = entry_name_len(name)?;
        let tlen = name_len(target)?;
        let mut st = zeroed_stat();
        let mut ino = 0;
        ret(self.call(|| unsafe {
            f(
                self.fs,
                dir,
                name.as_ptr().cast(),
                len,
                target.as_ptr().cast(),
                tlen,
                uid,
                gid,
                &mut ino,
                &mut st,
            )
        }))?;
        self.child_stat(ino, &st)
    }

    /// Reads a symlink target (at most `READLINK_MAX` bytes; the backend
    /// truncates longer targets like readlink(2)).
    pub fn readlink(&self, ino: u64) -> Result<Vec<u8>, Errno> {
        let f = self.ops().readlink.ok_or(Errno::ENOSYS)?;
        let mut buf = vec![0u8; READLINK_MAX];
        let mut len = 0usize;
        let cap = buf.len();
        let p = buf.as_mut_ptr().cast();
        ret(self.call(|| unsafe { f(self.fs, ino, p, cap, &mut len) }))?;
        if len > cap {
            return Err(Errno::EIO);
        }
        buf.truncate(len);
        Ok(buf)
    }
}

impl Mount {
    /// Unmounts the backend and then flushes the writable block devices.
    ///
    /// The backend's `unmount` must write back everything it holds (docs/abi.md);
    /// the devices are flushed afterwards so that data reaches stable storage.
    /// Runs the backend's `unmount` at most once: later calls (and the drop)
    /// return `Ok(())`. The backend must not be called after this. Returns the
    /// unmount error if there is one, else the first flush error.
    pub fn unmount(&self) -> Result<(), Errno> {
        if self.unmounted.swap(true, Ordering::SeqCst) {
            return Ok(());
        }
        let r = match self.inner.ops.unmount {
            Some(f) => ret(self.call(|| unsafe { f(self.fs) })),
            None => Ok(()),
        };
        let mut flush = Ok(());
        for h in self._devs.iter().flatten() {
            let d = h.device();
            if !d.read_only() {
                if let Err(e) = d.flush() {
                    flush = flush.and(Err(e));
                }
            }
        }
        r.and(flush)
    }
}

impl Drop for Mount {
    fn drop(&mut self) {
        if let Err(e) = self.unmount() {
            eprintln!("warning: backend {} unmount failed: {e}", self.inner.name);
        }
    }
}
