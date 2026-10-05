# Phase 16 — Reject case-insensitive and encrypted directories

**Decision (russ, 2026-10-05).** No support. Reject if detection costs
nothing; otherwise assume it does not happen (no scans).
- Both checks are free at the point dcfs already holds a directory fd and
  its statx (populating a directory): encrypted from `STATX_ATTR_ENCRYPTED`
  in the statx dcfs already does; case-insensitive from `FS_CASEFOLD_FL`
  via one `FS_IOC_GETFLAGS` on the directory fd already open. Such a
  directory is presented like a boundary stub (ENOTSUP inside, one-time
  ERROR log naming it). If the instance root itself is one, the mount
  fails with a clear error.
- Tests first (QEMU): ext4 made with `-O casefold,encrypt`; a `chattr +F`
  directory and an fscrypt directory (policy set by a `testutil` command
  via `FS_IOC_SET_ENCRYPTION_POLICY` and `FS_IOC_ADD_ENCRYPTION_KEY`) are
  stubs; a root that is one fails the mount. Kernel fragment gains
  `UNICODE` and `FS_ENCRYPTION`.
Owner: Sonnet. Order: after the stubs exist (mount wrapper phase).
