# Backend ABI v1

Headers: `include/winuxfs/backend.h` (the contract) and `include/winuxfs/errno.h` (error values). Rust bindings: `crates/winuxfs-abi`.

## Conventions

All operations return `wfs_err_t`. Success is 0; failure is `-(Linux errno value)` (for example `-2` is ENOENT). Names are byte arrays that need not be UTF-8, are not NUL-terminated, and are passed together with a length. Operations may be called concurrently from multiple threads; if a backend declares `WFS_CAP_SINGLE_THREADED`, the core serializes calls with a global lock. An inode returned by `lookup`/`create`/`symlink` carries one reference, released with `forget`. The required operations are `mount`, `unmount`, `getattr`, `lookup`, `forget`, `readdir` and `statfs`; any other operation that is NULL is handled by the core as `-ENOSYS` (-38).

## Error values: `winuxfs/errno.h`

Backends must take their error numbers from `include/winuxfs/errno.h`, which defines every Linux errno (1..133) as `WFS_E*` (the prefix keeps the names from clashing with the C runtime's `<errno.h>`). Do not use `<errno.h>`: on Windows (mingw) its values differ (ENOSYS 40, ENOTEMPTY 41, ENAMETOOLONG 38, ELOOP 114). The host never translates: it reads the number as a Linux errno, so a mingw `-ENOTEMPTY` (-41) arrives as EMULTIHOP and surfaces as a generic I/O error. A value outside 1..=133 (or a positive value) is treated as EIO. The bundled test backends (memfs, testplug, bad) use this header.

## Rules the host enforces

- **Root inode.** The root directory is inode 1 (`WFS_ROOT_INO`). A backend whose root has another number (XFS uses 64 or 128) must remap it, and keep reporting 1 in `getattr`/`readdir`. Right after `mount` the host calls `getattr(1)`; if that fails or the inode is not a directory the mount is refused with EIO and the backend is unmounted again. `mount` returning 0 must have stored a non-NULL `*out`; a NULL handle is refused with EIO.
- **Names.** A name is 1 to 255 bytes (`WFS_NAME_MAX`) and contains no NUL and no `/`. The host never passes `lookup`/`create`/`unlink`/`rename`/`link`/`symlink` a name that breaks this (it fails with ENAMETOOLONG for more than 255 bytes, EINVAL otherwise, without calling the backend). A backend must not return such a name from `readdir` either: the whole call fails with EIO. `.` and `..` may appear in a listing; `Vfs::read_dir` filters them out.
- **Inode numbers** returned by `lookup`/`create`/`symlink`/`readdir` are never 0 (EIO).
- **File types** must be one of the `WFS_FT_*` values (EIO otherwise).

## Mount lifecycle

`unmount` writes back everything the backend still holds (as if `sync` had been called), releases all its resources and returns; it is called exactly once, after the last `forget`. The host then flushes the writable block devices. Because of this the host's unmount sequence is: forget all references, `sync` (writable mounts), backend `unmount`, `blk_flush` of each writable device. An `unmount` error is reported to the caller of `Vfs::unmount`/`Mount::unmount` (the CLI exits with code 2 and a one-line message on stderr, also after Ctrl-C); it does not skip the flush. After `unmount` the `Mount` refuses every call with EIO without reaching the backend, and the unmount runs only once (a later drop does not repeat it). The controller must stop the WinFsp dispatcher and close the open handles before `Vfs::unmount`.

## Read-only mounts

A backend is told it must not write only through `blk_read_only()` of its devices (the host opens the devices read-only for a read-only mount; writes to a read-only device fail with `-EROFS`). A backend that sees a read-only data device must not replay or modify its log or superblock (if it cannot mount without that, it fails the mount), and must fail every mutating operation with `-WFS_EROFS`. The host additionally rejects mutations itself in a read-only `Vfs` and does not call `sync`/`fsync` in that mode.

## readdir cookies

`readdir(dir, cookie, ...)` starts at cookie 0 and resumes after the entry whose `next_cookie` was passed. Cookies must be unique within a directory, nonzero (0 means "start"), and below 2^63 (like Linux directory offsets, which are positive `loff_t` values). The Windows adapter reserves the values from 2^63 up for its synthetic `.` and `..` entries.

Within one `readdir` call every `next_cookie` must differ from the cookie passed in and from every earlier `next_cookie` of that call; the host fails the call with EIO otherwise. (Cookies need not be increasing.) The `Vfs` also remembers the last cookie of every page of a listing and fails with EIO if a chain returns to one, so a misbehaving backend cannot make a scan loop forever.

Note: this rule rejects a backend whose cookies are hashes (ext4-style htree) as soon as two entries of a directory collide, because the repeated cookie fails the call with EIO. That is fine for XFS (M1 to M2); revisit it before adding such a backend (M3+).

If an entry is renamed while a directory is being paged, a backend may give it a new cookie, so it can be returned twice (memfs does this); callers must not assume exactly-once delivery across concurrent renames.

## Entry point

A plugin DLL/shared object exports `const wfs_backend_desc *wfs_backend_entry(void)`. The descriptor carries the ABI version (`WFS_ABI_VERSION`, currently 1), the backend name, `caps` (`WFS_CAP_*`) and the operation table. The core refuses a plugin whose ABI version differs or whose required operations are missing.
