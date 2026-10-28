# Backend ABI v1

Header: `include/winuxfs/backend.h` (the contract). Rust bindings: `crates/winuxfs-abi`.

## Conventions

All operations return `wfs_err_t`. Success is 0; failure is `-(Linux errno value)` (for example `-2` is ENOENT). Names are byte arrays that need not be UTF-8, are not NUL-terminated, and are passed together with a length. Operations may be called concurrently from multiple threads; if a backend declares `WFS_CAP_SINGLE_THREADED`, the core serializes calls with a global lock. An inode returned by `lookup`/`create`/`symlink` carries one reference, released with `forget`. The required operations are `mount`, `unmount`, `getattr`, `lookup`, `forget`, `readdir` and `statfs`; any other operation that is NULL is handled by the core as `-ENOSYS` (-38).

## readdir cookies

`readdir(dir, cookie, ...)` starts at cookie 0 and resumes after the entry whose `next_cookie` was passed. Cookies must be unique within a directory and below 2^63 (like Linux directory offsets, which are positive `loff_t` values). The Windows adapter reserves the values from 2^63 up for its synthetic `.` and `..` entries.

If an entry is renamed while a directory is being paged, a backend may give it a new cookie, so it can be returned twice (memfs does this); callers must not assume exactly-once delivery across concurrent renames.

## Entry point

A plugin DLL/shared object exports `const wfs_backend_desc *wfs_backend_entry(void)`. The descriptor carries the ABI version (`WFS_ABI_VERSION`, currently 1), the backend name, `caps` (`WFS_CAP_*`) and the operation table. The core refuses a plugin whose ABI version differs or whose required operations are missing.
