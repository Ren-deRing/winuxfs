// SPDX-License-Identifier: GPL-2.0-only
//! Windows half of the adapter: the WinFsp file system object, its
//! callbacks and teardown. See the parent module for the design.

use std::collections::HashSet;
use std::ffi::{c_void, OsStr};
use std::os::windows::ffi::OsStrExt;
use std::sync::{Arc, Mutex};
use std::time::SystemTime;

use windows_sys::Win32::Foundation::{CloseHandle, GetLastError, LocalFree, HANDLE};
use windows_sys::Win32::Security::Authorization::{
    ConvertSidToStringSidW, ConvertStringSecurityDescriptorToSecurityDescriptorW, SDDL_REVISION_1,
};
use windows_sys::Win32::Security::{
    GetSecurityDescriptorLength, GetTokenInformation, TokenUser, TOKEN_QUERY, TOKEN_USER,
};
use windows_sys::Win32::System::Threading::{GetCurrentProcess, OpenProcessToken};

use winuxfs_core::namemap;
use winuxfs_core::plugin::{FileType, Stat, Timespec, ROOT_INO};
use winuxfs_core::vfs::{Handle, Vfs};
use winuxfs_core::Errno;

use super::*;
use crate::winfsp_ffi::*;
use crate::{WinFsp, WinFspError};

/// `CreateOptions` bits checked by `Open`.
const FILE_DIRECTORY_FILE: u32 = 0x1;
const FILE_NON_DIRECTORY_FILE: u32 = 0x40;
/// Access rights that modify a file; refused on a read-only mount.
const WRITE_ACCESS: u32 = 0x2 /* FILE_WRITE_DATA */
    | 0x4 /* FILE_APPEND_DATA */
    | 0x10 /* FILE_WRITE_EA */
    | 0x100 /* FILE_WRITE_ATTRIBUTES */
    | 0x1_0000 /* DELETE */
    | 0x4_0000 /* WRITE_DAC */
    | 0x8_0000 /* WRITE_OWNER */;
/// `Cleanup` flag: the file is to be deleted (last handle, delete pending).
const FSP_CLEANUP_DELETE: u32 = 0x1;
/// `SetBasicInfo`: leave the attributes unchanged.
const INVALID_FILE_ATTRIBUTES: u32 = u32::MAX;
/// Longest name component Windows may send, in UTF-16 units.
const MAX_COMPONENT: u16 = 255;
/// Metadata cache timeout handed to WinFsp, milliseconds.
const FILE_INFO_TIMEOUT_MS: u32 = 1000;
/// Directory entries fetched from the Vfs per `read_dir` call.
const DIR_PAGE: usize = 128;
/// Name WinFsp reports as the file system type.
const FS_NAME: &str = "winuxfs";

/// State shared by all callbacks of one mount, reached through
/// `FSP_FILE_SYSTEM::user_context`.
struct Context {
    vfs: Arc<Vfs>,
    fsp: WinFsp,
    read_only: bool,
    /// The Vfs is case-insensitive: `Create`/`Open` report the on-disk
    /// spelling as the normalized name.
    case_insensitive: bool,
    /// Label, at most [`VOLUME_LABEL_CHARS`] units.
    label: Vec<u16>,
    /// Self-relative security descriptor returned for every file.
    sd: Vec<u8>,
    /// Vfs handles WinFsp currently holds as file contexts.
    open: Mutex<HashSet<u64>>,
}

/// A mounted file system. Dropping it removes the mount point, stops the
/// dispatcher (waiting for running callbacks), deletes the WinFsp file
/// system, closes the Vfs handles Windows still held and releases the Vfs.
/// Handles that applications still hold on the volume fail from then on.
pub struct MountedFs {
    fsp: WinFsp,
    fs: *mut FSP_FILE_SYSTEM,
    ctx: *mut Context,
    interface: *mut FSP_FILE_SYSTEM_INTERFACE,
    mount_point: String,
}

// SAFETY: the raw pointers are owned by this value; WinFsp's teardown calls
// used in Drop may run on any thread, and Context is Sync.
unsafe impl Send for MountedFs {}

impl std::fmt::Debug for MountedFs {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.debug_struct("MountedFs")
            .field("mount_point", &self.mount_point)
            .finish_non_exhaustive()
    }
}

impl MountedFs {
    /// The mount point given to [`mount_vfs`].
    pub fn mount_point(&self) -> &str {
        &self.mount_point
    }
}

impl Drop for MountedFs {
    fn drop(&mut self) {
        // SAFETY: fs is live until file_system_delete; the dispatcher is
        // stopped (no callback runs any more) before ctx and the interface
        // table are freed, and both were created by Box::into_raw in
        // mount_vfs and are freed exactly once here.
        unsafe {
            self.fsp.file_system_remove_mount_point(self.fs);
            self.fsp.file_system_stop_dispatcher(self.fs);
            self.fsp.file_system_delete(self.fs);
            free_context(self.ctx);
            drop(Box::from_raw(self.interface));
        }
    }
}

/// Closes the Vfs handles WinFsp never closed, then drops the context.
///
/// # Safety
/// `ctx` came from `Box::into_raw` and no callback can run any more.
unsafe fn free_context(ctx: *mut Context) {
    // SAFETY: forwarded to the caller.
    let ctx = unsafe { Box::from_raw(ctx) };
    let left: Vec<u64> = lock(&ctx.open).drain().collect();
    for h in left {
        ctx.vfs.close(Handle(h));
    }
}

fn lock<T>(m: &Mutex<T>) -> std::sync::MutexGuard<'_, T> {
    m.lock().unwrap_or_else(|e| e.into_inner())
}

/// NUL-terminated UTF-16 copy of `s`.
fn wide(s: &str) -> Vec<u16> {
    OsStr::new(s).encode_wide().chain(Some(0)).collect()
}

fn os_error(what: &'static str) -> WinFspError {
    // SAFETY: no preconditions.
    WinFspError::Os {
        what,
        code: unsafe { GetLastError() },
    }
}

/// String form (`S-1-5-...`) of the current process user's SID.
fn current_user_sid() -> Result<String, WinFspError> {
    let mut token: HANDLE = std::ptr::null_mut();
    // SAFETY: valid out pointer; GetCurrentProcess is a pseudo handle.
    if unsafe { OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &mut token) } == 0 {
        return Err(os_error("OpenProcessToken"));
    }
    let res = (|| {
        let mut need = 0u32;
        // SAFETY: size query with a null buffer.
        unsafe { GetTokenInformation(token, TokenUser, std::ptr::null_mut(), 0, &mut need) };
        if need == 0 {
            return Err(os_error("GetTokenInformation"));
        }
        // u64 storage keeps TOKEN_USER aligned.
        let mut buf = vec![0u64; (need as usize).div_ceil(8)];
        // SAFETY: buf has at least `need` bytes.
        if unsafe {
            GetTokenInformation(token, TokenUser, buf.as_mut_ptr().cast(), need, &mut need)
        } == 0
        {
            return Err(os_error("GetTokenInformation"));
        }
        // SAFETY: GetTokenInformation(TokenUser) filled a TOKEN_USER; its
        // SID pointer points into buf.
        let sid = unsafe { (*(buf.as_ptr() as *const TOKEN_USER)).User.Sid };
        let mut s: *mut u16 = std::ptr::null_mut();
        // SAFETY: sid is valid while buf lives; s receives a LocalAlloc'd
        // NUL-terminated string.
        if unsafe { ConvertSidToStringSidW(sid, &mut s) } == 0 {
            return Err(os_error("ConvertSidToStringSidW"));
        }
        // SAFETY: s is NUL-terminated; freed right after copying.
        let out = String::from_utf16_lossy(unsafe { wstr(s) });
        unsafe { LocalFree(s.cast()) };
        Ok(out)
    })();
    // SAFETY: token came from OpenProcessToken.
    unsafe { CloseHandle(token) };
    res
}

/// The fixed descriptor of this milestone: owner and group are the mounting
/// user, who gets full access, as do SYSTEM and Administrators. On a
/// read-only mount they get read and execute access only, so Windows' own
/// access check refuses write opens (WinFsp's `ReadOnlyVolume` flag only
/// reports the volume as read-only; it does not refuse write opens).
fn security_descriptor(read_only: bool) -> Result<Vec<u8>, WinFspError> {
    let sid = current_user_sid()?;
    let r = if read_only { "FRFX" } else { "FA" };
    let sddl = wide(&format!(
        "O:{sid}G:{sid}D:P(A;;{r};;;{sid})(A;;{r};;;SY)(A;;{r};;;BA)"
    ));
    let mut psd: *mut c_void = std::ptr::null_mut();
    let mut len = 0u32;
    // SAFETY: sddl is NUL-terminated; psd receives a LocalAlloc'd
    // self-relative descriptor of `len` bytes.
    let ok = unsafe {
        ConvertStringSecurityDescriptorToSecurityDescriptorW(
            sddl.as_ptr(),
            SDDL_REVISION_1,
            &mut psd,
            &mut len,
        )
    };
    if ok == 0 {
        return Err(os_error(
            "ConvertStringSecurityDescriptorToSecurityDescriptorW",
        ));
    }
    // SAFETY: psd is a valid descriptor; it is copied, then freed once.
    let out = unsafe {
        let n = (len as usize).min(GetSecurityDescriptorLength(psd) as usize);
        let v = std::slice::from_raw_parts(psd as *const u8, n).to_vec();
        LocalFree(psd);
        v
    };
    Ok(out)
}

/// Mounts `vfs` at `mount_point` (`X:` or an empty directory) and starts
/// serving it. The Vfs's case mode must match `opts.case_sensitive`.
pub fn mount_vfs(
    winfsp: &WinFsp,
    vfs: Arc<Vfs>,
    mount_point: &str,
    opts: &MountOptions,
) -> Result<MountedFs, WinFspError> {
    if vfs.options().case_insensitive == opts.case_sensitive {
        return Err(WinFspError::InvalidArgument(
            "case_sensitive does not match the Vfs case mode",
        ));
    }
    if vfs.options().read_only != opts.read_only {
        return Err(WinFspError::InvalidArgument(
            "read_only does not match the Vfs read-only mode",
        ));
    }
    if mount_point.is_empty() || mount_point.contains('\0') {
        return Err(WinFspError::InvalidArgument("empty or invalid mount point"));
    }
    let mut label: Vec<u16> = opts.volume_label.encode_utf16().collect();
    label.truncate(VOLUME_LABEL_CHARS);

    let mut params = FSP_FSCTL_VOLUME_PARAMS {
        version: size_of_u16::<FSP_FSCTL_VOLUME_PARAMS>(),
        sector_size: ALLOC_UNIT as u16,
        sectors_per_allocation_unit: 1,
        max_component_length: MAX_COMPONENT,
        file_info_timeout: FILE_INFO_TIMEOUT_MS,
        ..Default::default()
    };
    let now = SystemTime::now()
        .duration_since(SystemTime::UNIX_EPOCH)
        .map(|d| Timespec {
            sec: d.as_secs() as i64,
            nsec: d.subsec_nanos(),
        })
        .unwrap_or_default();
    params.volume_creation_time = filetime(now);
    params.volume_serial_number =
        (params.volume_creation_time ^ (params.volume_creation_time >> 32)) as u32;
    let mut flags = volume_flags::CASE_PRESERVED_NAMES
        | volume_flags::UNICODE_ON_DISK
        | volume_flags::POST_CLEANUP_WHEN_MODIFIED_ONLY
        | volume_flags::UM_FILE_CONTEXT_IS_USER_CONTEXT2
        | volume_flags::DIRECTORY_MARKER_AS_NEXT_OFFSET;
    if opts.case_sensitive {
        flags |= volume_flags::CASE_SENSITIVE_SEARCH;
    }
    if opts.read_only {
        flags |= volume_flags::READ_ONLY_VOLUME;
    }
    params.flags = flags;
    for (d, s) in params
        .file_system_name
        .iter_mut()
        .zip(FS_NAME.encode_utf16())
    {
        *d = s;
    }

    let ctx = Box::into_raw(Box::new(Context {
        vfs,
        fsp: winfsp.clone(),
        read_only: opts.read_only,
        case_insensitive: !opts.case_sensitive,
        label,
        sd: security_descriptor(opts.read_only)?,
        open: Mutex::new(HashSet::new()),
    }));
    let interface = Box::into_raw(Box::new(FSP_FILE_SYSTEM_INTERFACE {
        get_volume_info: Some(get_volume_info),
        get_security_by_name: Some(get_security_by_name),
        create: Some(create),
        open: Some(open),
        overwrite: Some(overwrite),
        cleanup: Some(cleanup),
        close: Some(close),
        read: Some(read),
        write: Some(write),
        flush: Some(flush),
        get_file_info: Some(get_file_info),
        set_basic_info: Some(set_basic_info),
        set_file_size: Some(set_file_size),
        can_delete: Some(can_delete),
        rename: Some(rename),
        get_security: Some(get_security),
        read_directory: Some(read_directory),
        ..Default::default()
    }));
    // Frees ctx and interface on the error paths (no dispatcher running).
    let fail = |e: WinFspError| {
        // SAFETY: both came from Box::into_raw above and WinFsp no longer
        // uses them (the file system was deleted or never created).
        unsafe {
            free_context(ctx);
            drop(Box::from_raw(interface));
        }
        e
    };

    let mut device = wide(FSP_FSCTL_DISK_DEVICE_NAME);
    let mut fs: *mut FSP_FILE_SYSTEM = std::ptr::null_mut();
    // SAFETY: device is NUL-terminated, params is valid, interface stays
    // valid until file_system_delete (owned by MountedFs or freed in fail).
    let st = unsafe { winfsp.file_system_create(device.as_mut_ptr(), &params, interface, &mut fs) };
    if st < 0 || fs.is_null() {
        return Err(fail(WinFspError::Status {
            what: "FspFileSystemCreate",
            status: st,
        }));
    }
    // SAFETY: fs is live; user_context is ours.
    unsafe { (*fs).user_context = ctx.cast() };

    let mut mp = wide(mount_point);
    // SAFETY: fs is live and mp is NUL-terminated.
    let st = unsafe { winfsp.file_system_set_mount_point(fs, mp.as_mut_ptr()) };
    if st < 0 {
        // SAFETY: no dispatcher was started; fs is not used again.
        unsafe { winfsp.file_system_delete(fs) };
        return Err(fail(WinFspError::Status {
            what: "FspFileSystemSetMountPoint",
            status: st,
        }));
    }
    // SAFETY: fs is live; the context is in place.
    let st = unsafe { winfsp.file_system_start_dispatcher(fs, 0) };
    if st < 0 {
        // SAFETY: the dispatcher did not start; delete also removes the
        // mount point.
        unsafe { winfsp.file_system_delete(fs) };
        return Err(fail(WinFspError::Status {
            what: "FspFileSystemStartDispatcher",
            status: st,
        }));
    }
    Ok(MountedFs {
        fsp: winfsp.clone(),
        fs,
        ctx,
        interface,
        mount_point: mount_point.to_string(),
    })
}

const fn size_of_u16<T>() -> u16 {
    let n = std::mem::size_of::<T>();
    assert!(n <= u16::MAX as usize);
    n as u16
}

// ---- callback plumbing ----

/// The UTF-16 units of a NUL-terminated string.
///
/// # Safety
/// `p` is null or points to a NUL-terminated UTF-16 string that outlives
/// the returned slice.
unsafe fn wstr<'a>(p: *const u16) -> &'a [u16] {
    if p.is_null() {
        return &[];
    }
    let mut n = 0;
    // SAFETY: forwarded to the caller; we stop at the NUL.
    unsafe {
        while *p.add(n) != 0 {
            n += 1;
        }
        std::slice::from_raw_parts(p, n)
    }
}

/// # Safety
/// `fs` is a file system created by [`mount_vfs`] whose context is set.
unsafe fn ctx<'a>(fs: *mut FSP_FILE_SYSTEM) -> &'a Context {
    // SAFETY: forwarded to the caller; the context outlives the dispatcher.
    unsafe { &*((*fs).user_context as *const Context) }
}

fn handle(file_context: *mut c_void) -> Handle {
    Handle(file_context as usize as u64)
}

/// Status for a failed lookup of `path`: a missing (or non-directory)
/// parent gives `STATUS_OBJECT_PATH_NOT_FOUND`, as NTFS does.
fn lookup_status(c: &Context, path: &[u8], e: Errno) -> NTSTATUS {
    if e != Errno::ENOENT && e != Errno::ENOTDIR {
        return errno_to_ntstatus(e);
    }
    let parent = match path.iter().rposition(|&b| b == b'/') {
        Some(0) | None => return STATUS_OBJECT_NAME_NOT_FOUND,
        Some(i) => &path[..i],
    };
    match c.vfs.open(parent) {
        Ok((h, st)) => {
            c.vfs.close(h);
            if st.ftype == FileType::Dir {
                STATUS_OBJECT_NAME_NOT_FOUND
            } else {
                STATUS_OBJECT_PATH_NOT_FOUND
            }
        }
        Err(_) => STATUS_OBJECT_PATH_NOT_FOUND,
    }
}

/// Opens `name` for a short query and returns its attributes.
fn stat_path(c: &Context, name: &[u16]) -> Result<Stat, NTSTATUS> {
    let path = vfs_path(name).map_err(|_| STATUS_OBJECT_NAME_INVALID)?;
    let (h, st) = c.vfs.open(&path).map_err(|e| lookup_status(c, &path, e))?;
    c.vfs.close(h);
    Ok(st)
}

/// Copies the fixed descriptor out, WinFsp style: with a too small buffer
/// the required size is stored and `STATUS_BUFFER_OVERFLOW` returned.
///
/// # Safety
/// `size` is null or valid; `buf` is null or has `*size` writable bytes.
unsafe fn copy_sd(c: &Context, buf: PSECURITY_DESCRIPTOR, size: *mut usize) -> NTSTATUS {
    if size.is_null() {
        return STATUS_SUCCESS;
    }
    // SAFETY: forwarded to the caller.
    unsafe {
        if *size < c.sd.len() {
            *size = c.sd.len();
            return STATUS_BUFFER_OVERFLOW;
        }
        if !buf.is_null() {
            std::ptr::copy_nonoverlapping(c.sd.as_ptr(), buf.cast::<u8>(), c.sd.len());
        }
        *size = c.sd.len();
    }
    STATUS_SUCCESS
}

// ---- callbacks ----

unsafe extern "system" fn get_volume_info(
    fs: *mut FSP_FILE_SYSTEM,
    info: *mut FSP_FSCTL_VOLUME_INFO,
) -> NTSTATUS {
    guard("GetVolumeInfo", || {
        // SAFETY: WinFsp passes our file system and a valid out pointer.
        let (c, info) = unsafe { (ctx(fs), &mut *info) };
        match c.vfs.statfs() {
            Ok(s) => {
                info.total_size = s.total_bytes;
                info.free_size = s.free_bytes.min(s.total_bytes);
                info.volume_label = [0; VOLUME_LABEL_CHARS];
                info.volume_label[..c.label.len()].copy_from_slice(&c.label);
                info.volume_label_length = (c.label.len() * 2) as u16;
                STATUS_SUCCESS
            }
            Err(e) => errno_to_ntstatus(e),
        }
    })
}

unsafe extern "system" fn get_security_by_name(
    fs: *mut FSP_FILE_SYSTEM,
    name: PWSTR,
    attributes: *mut u32,
    sd: PSECURITY_DESCRIPTOR,
    sd_size: *mut usize,
) -> NTSTATUS {
    guard("GetSecurityByName", || {
        // SAFETY: WinFsp passes our file system and a NUL-terminated name.
        let (c, name) = unsafe { (ctx(fs), wstr(name)) };
        let st = match stat_path(c, name) {
            Ok(st) => st,
            Err(status) => return status,
        };
        if !attributes.is_null() {
            // SAFETY: non-null out pointer from WinFsp.
            unsafe { *attributes = file_info(&st, c.read_only).file_attributes };
        }
        // SAFETY: WinFsp's buffer contract matches copy_sd's.
        unsafe { copy_sd(c, sd, sd_size) }
    })
}

/// Fills the `Create`/`Open` result: the file context, the file
/// information and, on a case-insensitive volume, the normalized name (the
/// on-disk spelling of the whole path) so that WinFsp does not fall back to
/// an upper-cased name, which later `Cleanup`/`Rename` calls would carry.
///
/// # Safety
/// `file_context` and `info` are the out pointers WinFsp passed to
/// `Create`/`Open` (`info` really points to an `FSP_FSCTL_OPEN_FILE_INFO`).
unsafe fn finish_open(
    c: &Context,
    h: Handle,
    st: &Stat,
    disk: &[u8],
    file_context: *mut *mut c_void,
    info: *mut FSP_FSCTL_FILE_INFO,
) {
    lock(&c.open).insert(h.0);
    // SAFETY: forwarded to the caller.
    unsafe {
        *file_context = h.0 as usize as *mut c_void;
        *info = file_info(st, c.read_only);
        if c.case_insensitive {
            let oi = info as *mut FSP_FSCTL_OPEN_FILE_INFO;
            let name = windows_path(disk);
            let bytes = name.len() * 2;
            if !(*oi).normalized_name.is_null() && bytes <= usize::from((*oi).normalized_name_size)
            {
                std::ptr::copy_nonoverlapping(name.as_ptr(), (*oi).normalized_name, name.len());
                (*oi).normalized_name_size = bytes as u16;
            } else if !(*oi).normalized_name.is_null() {
                // WinFsp falls back to the upcased name; leave a trace.
                use std::io::Write;
                let _ = writeln!(
                    std::io::stderr(),
                    "winuxfs: debug: NormalizedName buffer too small ({} > {} bytes) for {}",
                    bytes,
                    (*oi).normalized_name_size,
                    String::from_utf16_lossy(&name)
                );
            }
        }
    }
}

/// Stores the file information for `h` in `info`.
///
/// # Safety
/// `info` is a valid out pointer.
unsafe fn store_info(c: &Context, h: Handle, info: *mut FSP_FSCTL_FILE_INFO) -> NTSTATUS {
    match c.vfs.stat(h) {
        Ok(st) => {
            // SAFETY: forwarded to the caller.
            unsafe { *info = file_info(&st, c.read_only) };
            STATUS_SUCCESS
        }
        Err(e) => errno_to_ntstatus(e),
    }
}

/// Applies a Windows attribute word to `h` (only READONLY is stored).
fn apply_attributes(c: &Context, h: Handle, attributes: u32) -> Result<(), Errno> {
    let st = c.vfs.stat(h)?;
    match mode_for_attributes(st.mode, st.ftype, attributes) {
        Some(m) => c.vfs.set_mode(h, m),
        None => Ok(()),
    }
}

fn status(r: Result<(), Errno>) -> NTSTATUS {
    match r {
        Ok(()) => STATUS_SUCCESS,
        Err(e) => errno_to_ntstatus(e),
    }
}

unsafe extern "system" fn open(
    fs: *mut FSP_FILE_SYSTEM,
    name: PWSTR,
    create_options: u32,
    granted_access: u32,
    file_context: *mut *mut c_void,
    info: *mut FSP_FSCTL_FILE_INFO,
) -> NTSTATUS {
    guard("Open", || {
        // SAFETY: WinFsp passes our file system and a NUL-terminated name.
        let (c, name) = unsafe { (ctx(fs), wstr(name)) };
        // Backstop for the read-only descriptor: the owner is implicitly
        // granted WRITE_DAC, and backup privileges bypass the DACL.
        if c.read_only && granted_access & WRITE_ACCESS != 0 {
            return STATUS_MEDIA_WRITE_PROTECTED;
        }
        let path = match vfs_path(name) {
            Ok(p) => p,
            Err(_) => return STATUS_OBJECT_NAME_INVALID,
        };
        let (h, st, disk) = match c.vfs.open_resolved(&path) {
            Ok(x) => x,
            Err(e) => return lookup_status(c, &path, e),
        };
        let is_dir = st.ftype == FileType::Dir;
        if create_options & FILE_DIRECTORY_FILE != 0 && !is_dir {
            c.vfs.close(h);
            return STATUS_NOT_A_DIRECTORY;
        }
        if create_options & FILE_NON_DIRECTORY_FILE != 0 && is_dir {
            c.vfs.close(h);
            return STATUS_FILE_IS_A_DIRECTORY;
        }
        // SAFETY: WinFsp's out pointers.
        unsafe { finish_open(c, h, &st, &disk, file_context, info) };
        STATUS_SUCCESS
    })
}

unsafe extern "system" fn create(
    fs: *mut FSP_FILE_SYSTEM,
    name: PWSTR,
    create_options: u32,
    _granted_access: u32,
    attributes: u32,
    _sd: PSECURITY_DESCRIPTOR,
    _allocation_size: u64,
    file_context: *mut *mut c_void,
    info: *mut FSP_FSCTL_FILE_INFO,
) -> NTSTATUS {
    guard("Create", || {
        // SAFETY: WinFsp passes our file system and a NUL-terminated name.
        let (c, name) = unsafe { (ctx(fs), wstr(name)) };
        if c.read_only {
            return STATUS_MEDIA_WRITE_PROTECTED;
        }
        let path = match vfs_path(name) {
            Ok(p) => p,
            Err(_) => return STATUS_OBJECT_NAME_INVALID,
        };
        let is_dir = create_options & FILE_DIRECTORY_FILE != 0;
        let ftype = if is_dir { FileType::Dir } else { FileType::Reg };
        let mode = create_mode(is_dir, attributes);
        let (h, st, disk) = match c.vfs.create_resolved(&path, ftype, mode) {
            Ok(x) => x,
            Err(e @ (Errno::ENOENT | Errno::ENOTDIR)) => return lookup_status(c, &path, e),
            Err(e) => return errno_to_ntstatus(e),
        };
        // SAFETY: WinFsp's out pointers.
        unsafe { finish_open(c, h, &st, &disk, file_context, info) };
        STATUS_SUCCESS
    })
}

unsafe extern "system" fn overwrite(
    fs: *mut FSP_FILE_SYSTEM,
    file_context: *mut c_void,
    attributes: u32,
    replace_attributes: BOOLEAN,
    _allocation_size: u64,
    info: *mut FSP_FSCTL_FILE_INFO,
) -> NTSTATUS {
    guard("Overwrite", || {
        // SAFETY: WinFsp passes our file system.
        let c = unsafe { ctx(fs) };
        let h = handle(file_context);
        if let Err(e) = c.vfs.set_len(h, 0) {
            return errno_to_ntstatus(e);
        }
        // Only READONLY is stored. Replacing applies the new word; merging
        // (OR) can only set it.
        if replace_attributes != 0 || attributes & FILE_ATTRIBUTE_READONLY != 0 {
            if let Err(e) = apply_attributes(c, h, attributes) {
                return errno_to_ntstatus(e);
            }
        }
        // SAFETY: valid out pointer.
        unsafe { store_info(c, h, info) }
    })
}

unsafe extern "system" fn cleanup(
    fs: *mut FSP_FILE_SYSTEM,
    file_context: *mut c_void,
    name: PWSTR,
    flags: u32,
) {
    guard_void("Cleanup", || {
        // Only deletion needs work: the backend keeps the times itself and
        // allocation size is not tracked separately from the file size.
        if flags & FSP_CLEANUP_DELETE == 0 || name.is_null() {
            return;
        }
        // SAFETY: WinFsp passes our file system and, with the delete flag,
        // the file's current (normalized) name.
        let (c, name) = unsafe { (ctx(fs), wstr(name)) };
        let h = handle(file_context);
        let Ok(path) = vfs_path(name) else {
            log_cleanup_error(name, Errno::EINVAL);
            return;
        };
        let r = match c.vfs.stat(h) {
            Ok(st) if st.ftype == FileType::Dir => c.vfs.rmdir(&path),
            Ok(_) => c.vfs.unlink(&path),
            Err(e) => Err(e),
        };
        if let Err(e) = r {
            log_cleanup_error(name, e);
        }
    });
}

/// Cleanup cannot report errors to Windows; leave a trace on stderr.
fn log_cleanup_error(name: &[u16], e: Errno) {
    use std::io::Write;
    let _ = writeln!(
        std::io::stderr(),
        "winuxfs: delete of {} failed: {e}",
        String::from_utf16_lossy(name)
    );
}

unsafe extern "system" fn write(
    fs: *mut FSP_FILE_SYSTEM,
    file_context: *mut c_void,
    buffer: *mut c_void,
    offset: u64,
    length: u32,
    write_to_end: BOOLEAN,
    constrained_io: BOOLEAN,
    transferred: *mut u32,
    info: *mut FSP_FSCTL_FILE_INFO,
) -> NTSTATUS {
    guard("Write", || {
        // SAFETY: WinFsp passes our file system and valid out pointers.
        let c = unsafe { ctx(fs) };
        unsafe { *transferred = 0 };
        let h = handle(file_context);
        let size = match c.vfs.stat(h) {
            Ok(st) => st.size,
            Err(e) => return errno_to_ntstatus(e),
        };
        let (off, len) = write_range(offset, length, size, write_to_end != 0, constrained_io != 0);
        if len > 0 {
            // SAFETY: WinFsp's buffer holds `length` >= len readable bytes.
            let buf = unsafe { std::slice::from_raw_parts(buffer.cast::<u8>(), len) };
            let mut done = 0;
            while done < len {
                match c.vfs.write(h, off + done as u64, &buf[done..]) {
                    Ok(0) => break,
                    Ok(n) => done += n.min(len - done),
                    Err(e) if done == 0 => return errno_to_ntstatus(e),
                    Err(_) => break,
                }
            }
            if done == 0 {
                return STATUS_DISK_FULL;
            }
            // SAFETY: valid out pointer.
            unsafe { *transferred = done as u32 };
        }
        // SAFETY: valid out pointer.
        unsafe { store_info(c, h, info) }
    })
}

unsafe extern "system" fn flush(
    fs: *mut FSP_FILE_SYSTEM,
    file_context: *mut c_void,
    info: *mut FSP_FSCTL_FILE_INFO,
) -> NTSTATUS {
    guard("Flush", || {
        // SAFETY: WinFsp passes our file system.
        let c = unsafe { ctx(fs) };
        if file_context.is_null() {
            return status(c.vfs.sync());
        }
        let h = handle(file_context);
        if let Err(e) = c.vfs.fsync(h, false) {
            return errno_to_ntstatus(e);
        }
        if info.is_null() {
            return STATUS_SUCCESS;
        }
        // SAFETY: non-null out pointer from WinFsp.
        unsafe { store_info(c, h, info) }
    })
}

unsafe extern "system" fn set_basic_info(
    fs: *mut FSP_FILE_SYSTEM,
    file_context: *mut c_void,
    attributes: u32,
    _creation_time: u64,
    last_access_time: u64,
    last_write_time: u64,
    _change_time: u64,
    info: *mut FSP_FSCTL_FILE_INFO,
) -> NTSTATUS {
    guard("SetBasicInfo", || {
        // SAFETY: WinFsp passes our file system.
        let c = unsafe { ctx(fs) };
        let h = handle(file_context);
        // Creation and change times cannot be set through the Vfs; they are
        // accepted and ignored. Of the attributes only READONLY is stored.
        if attributes != INVALID_FILE_ATTRIBUTES {
            if let Err(e) = apply_attributes(c, h, attributes) {
                return errno_to_ntstatus(e);
            }
        }
        let atime = timespec_from_filetime(last_access_time);
        let mtime = timespec_from_filetime(last_write_time);
        if atime.is_some() || mtime.is_some() {
            if let Err(e) = c.vfs.set_times(h, atime, mtime) {
                return errno_to_ntstatus(e);
            }
        }
        // SAFETY: valid out pointer.
        unsafe { store_info(c, h, info) }
    })
}

unsafe extern "system" fn set_file_size(
    fs: *mut FSP_FILE_SYSTEM,
    file_context: *mut c_void,
    new_size: u64,
    set_allocation_size: BOOLEAN,
    info: *mut FSP_FSCTL_FILE_INFO,
) -> NTSTATUS {
    guard("SetFileSize", || {
        // SAFETY: WinFsp passes our file system.
        let c = unsafe { ctx(fs) };
        let h = handle(file_context);
        let size = match c.vfs.stat(h) {
            Ok(st) => st.size,
            Err(e) => return errno_to_ntstatus(e),
        };
        // Allocation size is not stored: growing it is a no-op, shrinking
        // it below the file size truncates the file (as on NTFS). Setting
        // the file size grows with zeros or truncates.
        let target = if set_allocation_size != 0 {
            (new_size < size).then_some(new_size)
        } else {
            (new_size != size).then_some(new_size)
        };
        if let Some(n) = target {
            if let Err(e) = c.vfs.set_len(h, n) {
                return errno_to_ntstatus(e);
            }
        }
        // SAFETY: valid out pointer.
        unsafe { store_info(c, h, info) }
    })
}

unsafe extern "system" fn can_delete(
    fs: *mut FSP_FILE_SYSTEM,
    file_context: *mut c_void,
    name: PWSTR,
) -> NTSTATUS {
    guard("CanDelete", || {
        // SAFETY: WinFsp passes our file system and a NUL-terminated name.
        let (c, name) = unsafe { (ctx(fs), wstr(name)) };
        if c.read_only {
            return STATUS_MEDIA_WRITE_PROTECTED;
        }
        if matches!(vfs_path(name).as_deref(), Ok(b"/")) {
            return STATUS_ACCESS_DENIED;
        }
        let h = handle(file_context);
        match c.vfs.stat(h) {
            Ok(st) if st.ftype == FileType::Dir => match c.vfs.read_dir(h, 0, 1) {
                Ok(e) if e.is_empty() => STATUS_SUCCESS,
                Ok(_) => STATUS_DIRECTORY_NOT_EMPTY,
                Err(e) => errno_to_ntstatus(e),
            },
            Ok(_) => STATUS_SUCCESS,
            Err(e) => errno_to_ntstatus(e),
        }
    })
}

/// Inode number of `path`, if it exists.
fn ino_of_path(c: &Context, path: &[u8]) -> Option<(u64, FileType)> {
    let (h, st) = c.vfs.open(path).ok()?;
    c.vfs.close(h);
    Some((st.ino, st.ftype))
}

unsafe extern "system" fn rename(
    fs: *mut FSP_FILE_SYSTEM,
    _file_context: *mut c_void,
    old_name: PWSTR,
    new_name: PWSTR,
    replace_if_exists: BOOLEAN,
) -> NTSTATUS {
    guard("Rename", || {
        // SAFETY: WinFsp passes our file system and NUL-terminated names.
        let (c, old_w, new_w) = unsafe { (ctx(fs), wstr(old_name), wstr(new_name)) };
        if c.read_only {
            return STATUS_MEDIA_WRITE_PROTECTED;
        }
        let (Ok(old), Ok(new)) = (vfs_path(old_w), vfs_path(new_w)) else {
            return STATUS_OBJECT_NAME_INVALID;
        };
        let replace = replace_if_exists != 0;
        // Windows never replaces a directory, and a directory never replaces
        // anything, even with ReplaceIfExists (unless the target is the
        // source itself: a case-only rename). NTFS answers ACCESS_DENIED.
        if replace {
            if let Some((tino, tty)) = ino_of_path(c, &new) {
                let src = ino_of_path(c, &old);
                let same = src.map(|(i, _)| i) == Some(tino);
                let src_is_dir = matches!(src, Some((_, FileType::Dir)));
                if !same && (tty == FileType::Dir || src_is_dir) {
                    return STATUS_ACCESS_DENIED;
                }
            }
        }
        match c.vfs.rename(&old, &new, replace) {
            Ok(()) => STATUS_SUCCESS,
            Err(e @ (Errno::ENOENT | Errno::ENOTDIR)) => {
                // A missing source is reported by its name; a missing target
                // directory as a path error.
                if ino_of_path(c, &old).is_some() {
                    lookup_status(c, &new, e)
                } else {
                    lookup_status(c, &old, e)
                }
            }
            Err(e) => errno_to_ntstatus(e),
        }
    })
}

unsafe extern "system" fn close(fs: *mut FSP_FILE_SYSTEM, file_context: *mut c_void) {
    guard_void("Close", || {
        // SAFETY: WinFsp passes our file system.
        let c = unsafe { ctx(fs) };
        let h = handle(file_context);
        if lock(&c.open).remove(&h.0) {
            c.vfs.close(h);
        }
    })
}

unsafe extern "system" fn read(
    fs: *mut FSP_FILE_SYSTEM,
    file_context: *mut c_void,
    buffer: *mut c_void,
    offset: u64,
    length: u32,
    transferred: *mut u32,
) -> NTSTATUS {
    guard("Read", || {
        // SAFETY: WinFsp passes our file system and a valid out pointer.
        let c = unsafe { ctx(fs) };
        unsafe { *transferred = 0 };
        let h = handle(file_context);
        let size = match c.vfs.stat(h) {
            Ok(st) => st.size,
            Err(e) => return errno_to_ntstatus(e),
        };
        if offset >= size {
            return STATUS_END_OF_FILE;
        }
        let want = (u64::from(length)).min(size - offset) as usize;
        // SAFETY: WinFsp's buffer has `length` >= want writable bytes.
        let buf = unsafe { std::slice::from_raw_parts_mut(buffer.cast::<u8>(), want) };
        let mut done = 0;
        while done < want {
            match c.vfs.read(h, offset + done as u64, &mut buf[done..]) {
                Ok(0) => break,
                Ok(n) => done += n.min(want - done),
                Err(e) if done == 0 => return errno_to_ntstatus(e),
                Err(_) => break,
            }
        }
        if done == 0 {
            return STATUS_END_OF_FILE;
        }
        // SAFETY: valid out pointer.
        unsafe { *transferred = done as u32 };
        STATUS_SUCCESS
    })
}

unsafe extern "system" fn get_file_info(
    fs: *mut FSP_FILE_SYSTEM,
    file_context: *mut c_void,
    info: *mut FSP_FSCTL_FILE_INFO,
) -> NTSTATUS {
    guard("GetFileInfo", || {
        // SAFETY: WinFsp passes our file system.
        let c = unsafe { ctx(fs) };
        match c.vfs.stat(handle(file_context)) {
            Ok(st) => {
                // SAFETY: valid out pointer.
                unsafe { *info = file_info(&st, c.read_only) };
                STATUS_SUCCESS
            }
            Err(e) => errno_to_ntstatus(e),
        }
    })
}

unsafe extern "system" fn get_security(
    fs: *mut FSP_FILE_SYSTEM,
    _file_context: *mut c_void,
    sd: PSECURITY_DESCRIPTOR,
    sd_size: *mut usize,
) -> NTSTATUS {
    guard("GetSecurity", || {
        // SAFETY: WinFsp passes our file system; buffer contract as copy_sd.
        unsafe { copy_sd(ctx(fs), sd, sd_size) }
    })
}

/// Fixed part of `FSP_FSCTL_DIR_INFO` in bytes.
const DIR_INFO_FIXED: usize = std::mem::size_of::<FSP_FSCTL_DIR_INFO>();

/// Builds one directory entry in `buf` (u64 storage keeps it aligned) and
/// returns a pointer to it, or `None` when the name cannot be represented.
fn dir_entry(
    buf: &mut Vec<u64>,
    info: FSP_FSCTL_FILE_INFO,
    cookie: u64,
    name: &[u16],
) -> Option<*mut FSP_FSCTL_DIR_INFO> {
    let size = DIR_INFO_FIXED + name.len() * 2;
    let size16 = u16::try_from(size).ok()?;
    buf.clear();
    buf.resize(size.div_ceil(8), 0);
    let p = buf.as_mut_ptr() as *mut FSP_FSCTL_DIR_INFO;
    // SAFETY: buf holds `size` zeroed, 8-aligned bytes: the fixed part and
    // then the name.
    unsafe {
        (*p).size = size16;
        (*p).file_info = info;
        (*p).u.next_offset = cookie;
        let dst = (p as *mut u8).add(DIR_INFO_FIXED) as *mut u16;
        std::ptr::copy_nonoverlapping(name.as_ptr(), dst, name.len());
    }
    Some(p)
}

/// Attributes for a listed entry: a fresh lookup, or (if that fails for a
/// reason other than the entry having vanished) what the entry says.
fn entry_info(c: &Context, dir: Handle, name: &[u8], ino: u64, ftype: FileType) -> Option<Stat> {
    match c.vfs.stat_at(dir, name) {
        Ok(st) => Some(st),
        Err(Errno::ENOENT) => None,
        Err(_) => Some(Stat {
            ino,
            ftype,
            mode: 0o644,
            uid: 0,
            gid: 0,
            nlink: 1,
            size: 0,
            blocks: 0,
            atime: Timespec::default(),
            mtime: Timespec::default(),
            ctime: Timespec::default(),
            btime: Timespec::default(),
        }),
    }
}

unsafe extern "system" fn read_directory(
    fs: *mut FSP_FILE_SYSTEM,
    file_context: *mut c_void,
    _pattern: PWSTR,
    marker: PWSTR,
    buffer: *mut c_void,
    length: u32,
    transferred: *mut u32,
) -> NTSTATUS {
    guard("ReadDirectory", || {
        // SAFETY: WinFsp passes our file system and a valid out pointer.
        let c = unsafe { ctx(fs) };
        unsafe { *transferred = 0 };
        let dir = handle(file_context);
        // With DirectoryMarkerAsNextOffset the marker is the 8-byte
        // NextOffset (our cookie) of the last entry WinFsp consumed; it sits
        // in a request buffer and may be unaligned.
        let marker = if marker.is_null() {
            None
        } else {
            // SAFETY: WinFsp guarantees 8 marker bytes (plus a NUL unit).
            Some(unsafe { std::ptr::read_unaligned(marker as *const u64) })
        };
        let dir_st = match c.vfs.stat(dir) {
            Ok(st) => st,
            Err(e) => return errno_to_ntstatus(e),
        };
        let mut entry = Vec::new();
        // "." and ".." carry the directory's own information.
        let dots: &[(&str, u64)] = match dir_start(marker, dir_st.ino == ROOT_INO) {
            DirStart::Dot => &[(".", COOKIE_DOT), ("..", COOKIE_DOTDOT)],
            DirStart::DotDot => &[("..", COOKIE_DOTDOT)],
            DirStart::Entries(_) => &[],
        };
        for &(name, next) in dots {
            let wname: Vec<u16> = name.encode_utf16().collect();
            let info = file_info(&dir_st, c.read_only);
            if let Some(p) = dir_entry(&mut entry, info, next, &wname) {
                // SAFETY: p is a complete entry; buffer/transferred as above.
                if !unsafe { c.fsp.add_dir_info(p, buffer, length, transferred) } {
                    return STATUS_SUCCESS;
                }
            }
        }
        let mut cookie = match dir_start(marker, dir_st.ino == ROOT_INO) {
            DirStart::Entries(c) => c,
            _ => 0,
        };
        loop {
            let page = match c.vfs.read_dir(dir, cookie, DIR_PAGE) {
                Ok(p) => p,
                Err(e) => return errno_to_ntstatus(e),
            };
            if page.is_empty() {
                // End of directory. If the marker does not fit either, the
                // buffer is simply returned full and WinFsp asks again.
                // SAFETY: WinFsp's buffer and counter.
                unsafe {
                    c.fsp
                        .add_dir_info(std::ptr::null_mut(), buffer, length, transferred)
                };
                return STATUS_SUCCESS;
            }
            for e in page {
                let next = e.cookie;
                let Some(st) = entry_info(c, dir, &e.name, e.ino, e.ftype) else {
                    cookie = next;
                    continue;
                };
                let wname = namemap::to_windows(&e.name);
                let Some(p) = dir_entry(&mut entry, file_info(&st, c.read_only), next, &wname)
                else {
                    cookie = next;
                    continue;
                };
                // SAFETY: p is a complete entry; buffer/transferred as above.
                if !unsafe { c.fsp.add_dir_info(p, buffer, length, transferred) } {
                    return STATUS_SUCCESS;
                }
                cookie = next;
            }
        }
    })
}
