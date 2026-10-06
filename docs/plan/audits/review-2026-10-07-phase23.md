# Review of Phase 23 (semantics gaps), 2026-10-07, dcfs-reviewer, read-only

Findings fixed in 23.7.

I found no high-severity problems. Every new dcfs mutation path writes its unknown state before the backing syscall, and every stub row is written in the same transaction as its dentry. There are two medium findings and several low ones.

## Medium

**M1. FS_IOC_SETFLAGS is forwarded without filtering flags, so `chattr +F` can make a case-insensitive directory under dcfs's case-sensitive cache.** (`dcfs/dir_cache_fs.cc:2152-2153`, `2209`)
- Scenario: ext4 made with `-O casefold`. A user runs `chattr +F emptydir` through dcfs. The kernel's fileattr path sends SETFLAGS; dcfs forwards it and ext4 accepts it on an empty directory.
- Result: the directory's cached complete listing and negative entries are now wrong. Create `Foo`, look up `foo`, and dcfs answers ENOENT while the backing would find it.
- Phase 16 plans to reject casefold directories; this step opens a way to make one through dcfs itself.
- Fix: refuse any SETFLAGS or FSSETXATTR whose flags change FS_CASEFOLD_FL (compare with a GETFLAGS first) with EOPNOTSUPP, and add a test.

**M2. Reconciliation at DESTROY: a mapping can still be live, and the cost is unbounded.** (`dcfs/dir_cache_fs.cc:199-204`)
- Live mapping (verified in the libfuse source): as root, `fuse_kern_unmount` closes /dev/fuse, which aborts the connection, and then unmounts lazily (`libfuse/lib/mount.c:325,335`). Passthrough mappings keep storing to the backing file after that.
- On a SIGTERM shutdown, DESTROY records the attributes as current, FinishRun marks a clean shutdown, and the next run serves stale attributes with no recovery. README and design.md list the munmap-to-FORGET window and a crash, but not this case.
- Cost: `written_` holds every file written this run whose FUSE inode is still cached, which can be hundreds of thousands after a large build. DESTROY then does one open_by_handle_at plus two statx per file, serially, before the final sync point. On a cold backing inode cache this could exceed a service stop timeout, ending in SIGKILL and a recovery on the next start.
- DESTROY cannot block umount(2): the kernel sends DESTROY only for fuseblk (`fs/fuse/inode.c:1943-1945`). It only delays the daemon's exit.
- Fix: at least document the live-mapping case. Then measure the cost with N written files, or consider marking `written_` attributes unknown at DESTROY (one transaction, no I/O), which trades it against warm reads after a restart.

## Low

**L1. The writability check allows the open, but the shared fd stays read-only.** (`dir_cache_fs.cc:1277`, `2105`, `2054`)
- Scenario: a file is opened read-only while immutable, so the shared fd is O_RDONLY. Then `chattr -i`, then a writable open, which passes the check.
- copy_file_range into it and fallocate use the shared fd and get EBADF; the backing filesystem allows both. The same happens for fallocate on an append-only file opened `O_WRONLY|O_APPEND`.
- Passthrough writes themselves work, because `backing_file_open` reopens with the caller's flags.
- Side effects of the check itself: each shared writable open adds an O_WRONLY open and close on the backing file. That emits IN_CLOSE_WRITE, can break leases, and can trigger ext4's auto_da_alloc flush.
- Fix: when the check succeeds and the shared fd is not writable, keep the reopened fd, or an O_RDWR reopen, as the shared fd.

**L2. Errno and documentation mismatches for stubs.**
- RMDIR/UNLINK of a stub gets EXDEV (`dir_cache_fs.cc:844`), but `docs/design.md:384` says ENOTSUP, and the backing filesystem gives EBUSY for a mount point. EBUSY is suggested.
- LINK of a stub gets EPERM (`:1142`); design.md says EXDEV.
- RefuseStub logs "ENOTSUP" for EPERM and ENOTTY refusals (`:301`).
- None of these branches is tested.

**L3. RefuseStub answers ENOTSUP even when the stub row is gone.** (`:1559`, `:598`, and the rest)
- After a stub is dropped by a relisting or recovery, a cached dentry still gets ENOTSUP until `entry_timeout`.
- ESTALE would make the kernel's path walk retry with LOOKUP_REVAL and find the real directory.
- Fix: return ESTALE when GetStub returns NotFound.

**L4. Stub attributes are never refreshed.** (`schema.sql:165`) "Nothing dcfs does changes them" is true, but they copy another filesystem's root, which changes through that mount. They persist across restarts until a relisting. README should say "as of the last probe".

**L5. Orphan tmpfile rows.** A crash with open tmpfiles leaves rows that no dentry or GC ever reaches, and glibc's tmpfile(3) uses O_TMPFILE. The undo at `:2258/2264` also leaves the RecordTmpfile row behind. The rows are harmless, but they accumulate.

**L6. Gaps in the atime prediction (documentation).**
- A per-file FS_NOATIME_FL (`chattr +A`) is ignored.
- The private OPEN that lsattr and chattr cause moves dcfs's atime with no read.
- After a power loss the predicted atime can survive while the backing filesystem loses its own update; recovery does not cover it because the row is not dirty.
- Allowed by design ("can differ"), but design.md should say atime is exempt from the mirror rule.

**L7. Efficiency.**
- StatWritten does open_by_handle_at, VerifyBackingIdentity's statx, and then a second statx (`backing.cc:698`); it could return the first statx.
- When something changed, RefreshAttrs reopens by handle again.
- TouchAtime opens a write transaction on every read OPEN; it could read first and write only when an update is needed.

**L8. 32-bit programs.** Stub inode numbers of 2^63 and above make the compat getdents return EOVERFLOW for the whole directory a stub sits in, for 32-bit programs built without large-file support. Worth one line in README.

## Tests

- Some comments claim coverage that is not there: BoundaryStubIsRecordedWithItsDentry ("a FORGET of it is counted") and StubsGoWithTheirRefusals ("and with its parent").
- Untested stub refusals: rmdir/unlink of the stub, readlink, mknod, symlink, readdir, readdirplus, fsyncdir, removexattr.
- `handle-boundary-stub-decodes` passes even if drop_caches does not evict the stub's dentry. The harness test covers `LOOKUP(stub, ".")`.
- `rename-stub-exdev` has no `check_cold`.
- The trace recorder maps any IOCTL on D, including lsattr's read-only GETFLAGS, to a `dir-attrs` cut (`trace_recorder.cc:186`). That is conservative but cuts traces short.
- Process: by the agent's own note, write_test was not run against bc60347.
- The lib.sh change (`drop_caches_quiesced` waiting for the daemon) is not a weakening: the reconciliation statx is caused by the FORGET, and measured windows contain no FORGETs.
- None of the updated tests (readonly, create, handles, rename, nfs, write) lost its teeth: each now asserts the stub is listed and that nothing inside it is reachable. The failing-first runs are quoted in the commits and the tests exercise the real paths.
- The new tests follow AGENTS.md: the statx fake is a `--wrap` in the test target only, and no test-only code went into production files.

## Verified (no finding)

- **Stubs and crashes:** SetRefused writes the dentry and its stub in one transaction. Dentries are upserts, not REPLACE, so the triggers fire on updates, deletes, RecoverDirty's `DELETE FROM dentries` and parent cascades.
- **Migration v3 to v4:** refused dentries become unknown. IsDirComplete is false while any dentry is unknown, so the next listing probes them again; they are never reported absent.
- **Ioctl set, copy_file_range destination, FORGET phase 1:** each marks the attributes unknown and durably dirty before recording, then refreshes as a fill. Phase 1 before the refresh also keeps ReconcileAttrs from reporting a false out-of-band change.
- **Removed objects (23.2):** the record caches nothing that changes. WithStatx overwrites every stat field, and xattrs and readlink go to the fd. Reopening as root matches OpenNode, and the gates are the kernel's default_permissions plus the ptrace check on `/proc/<pid>/fd`.
- **23.1's claim:** the mapping's backing file takes `path_get` on the FUSE path (`fs/backing-file.c:46,71`), so the last FORGET comes after munmap. Identity is checked, and a recycled or removed inode gives ESTALE and the row is forgotten.
- **chattr +i fix:**
  - The check is necessary, because passthrough's `backing_file_open` reopens with the caller's flags and skips the open-time checks (may_open).
  - The exception for an fd just opened O_RDWR is sound: that open succeeding rules out immutable, append-only and read-only filesystems.
  - The daemon is single-threaded, so nothing can interleave between the check and the reply.
- **copy_file_range:** the kernel enforces the same superblock (`fs/fuse/file.c:2963`), sends flags as 0, and clamps the length or uses the 64-bit op. Short copies are replied as they are.
- **Stub nodeids:** reusing one is safe. The kernel's `fuse_iget` marks the old inode bad on a generation mismatch, and the lookup counts add up correctly.
  - Readdirplus and lookup count stub lookups, and FORGET drops them.
  - `..` inside a stub works.
  - NFS handles for stubs decode after a restart and give ESTALE after a cache wipe, as the plan accepts.
- **ENOTSUP for a link or rename into a stub:** the best achievable, since a FUSE LOOKUP carries no intent. It is documented in README, design.md and the phase file.
- **Model:** `linkcreate` matches the code. Its phase 3 has no probe but is guarded by Owns and the kernel's directory lock. The state-count growth (about 12 to 18%) is plausible for one more request kind, and no new call site is unmapped.

Files: dcfs/dir_cache_fs.cc, dcfs/backing.cc, dcfs/metadata_cache.cc, dcfs/schema.sql, dcfs/migrate.cc, dcfs/testonly/trace_recorder.cc, docs/design.md, README.md, test/qemu/guest/{boundary,copy,atime,lib}.sh.
