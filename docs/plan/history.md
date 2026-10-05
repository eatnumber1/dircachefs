# History: the original plan (Phases 0-7) and amendments 1-20

Completed on 2026-10-04 (main at e5df2a7). Phase numbers in this file are
historical and unrelated to the numbering in `phases/`. Some decisions here
are superseded by later phases (noted there).


## Context

**Goal.** Turn `~/Sources/dircachefs` into a production-quality FUSE daemon
("dcfs") that mirrors a backing directory tree — including any filesystems
mounted and btrfs subvolumes below it — and serves every read operation
except reading file *contents* from a persistent SQLite cache on an SSD:
lookup, getattr, readdir, readlink, xattr names and values, access, statfs.
Backing disks spin up only for file-content I/O and for writes. It must be
safely NFS-exportable: handles stay valid across daemon restarts, and
handles that cannot be honored fail with ESTALE, never by serving the wrong
file.

**dcfs is a write-through cache and nothing more.** The backing filesystems
are the only authority. Every mutation reaches the backing filesystem before
the cache reflects it. Deleting the cache database at any time — including
mid-operation or after a crash — must never lose or corrupt data; the only
consequences are a cold cache and stale NFS handles (an accepted
limitation, see Decisions).

**Why now.** The kernel patch at HEAD of `~/Sources/linux`
(`320a1ed162d7`, protocol 7.47, `FUSE_ATTR_GENERATION`, on LKML as
`20260927141437.1432584-1-russ@har.mn`) and libfuse commit `0aa1acc6` in
`~/Sources/libfuse` make the kernel honor generation numbers on
LOOKUP/GETATTR/SETATTR and mark recycled inodes bad, so a daemon can safely
give the kernel a generation that changes when identity changes.
`~/Sources/fuse-generation-qemu` already boots that kernel under QEMU and
passes a 10-test suite exercising exactly this; **that repo stays as it
is** (it tests the LKML patch and may be shared with reviewers) — dircachefs
copies what it needs from it. Verified while planning: HEAD's reconnect
path (`fs/fuse/inode.c:1079`) sends only the nodeid to userspace
(`LOOKUP(nodeid, ".")`) and checks the handle's generation after the reply.

**State of the code (surveyed 2026-09-27).**
- Naming: the project and its code are **`dcfs`** (namespace `dcfs`,
  package `dcfs/`, binary `dcfs`; repo `dircachefs`). **`dfs`** in this
  plan always means the *old* 2023 repo `~/Sources/dfs` ("Diskyphus"),
  never the current code.
- `dircachefs` is not a git repo (ignored by the home dotfiles repo). Its
  ancestor `~/Sources/dfs` (8 commits, May 2023, a ublk block device
  design) is a strict subset: `dircachefs/dcfs/old/` == the dfs tree modulo
  the `dfs`→`dcfs` rename; every shared base file in `dircachefs/dcfs/` is
  a later evolution. Only the old repo's git history is worth carrying
  over (its libfuse Bazel glue is superseded by the BCR module, see 0.3).
- It does not compile; it is mid-refactor: `dir_cache_fs.cc` has a
  `Directory::OpenDir` signature pasted into `Opendir()`; `fs_db.h` has a
  stray declaration; `inode.*`/`dentry.*`/`directory.*` are drafts not in
  `BUILD.bazel`; `sqlite.cc` (`Prepare(const Statement&)`, `WithSavepoint`)
  and `file_handle.cc` (`MountOpener`, missing `mount_fd_`) have syntax
  errors; `syscalls.cc` `dirfd()` lacks a return; `Connection::Transaction`
  is used but never declared.
- Worth keeping: `absl::Status`/`absl::StatusOr` everywhere; the errno
  payload (`dcfs/status.cc`, `dcfs/errno.cc`); `FileDescriptor` RAII
  (`dcfs/fd.*`); the `syscalls::` wrapper layer (`dcfs/syscalls.*`); the
  `sqlite3::` wrapper; the file-handle idea (`name_to_handle_at` blob
  reopened via `open_by_handle_at`); the `FuseRequest` RAII reply wrapper.
  There is no homegrown StatusOr; the tree already uses `absl::StatusOr`.
- **Custom Bazel glue replaced by upstream modules (audit 2026-09-27):**
  the old dfs repo's hand-written libfuse `BUILD.bazel`/config headers/ldscript -> BCR
  `libfuse` 3.18.2 overlay + one patch (0.3); the `/usr` libmount
  `new_local_repository` -> deleted with libmount (0.4); `liburing` is in
  BCR (2.15) for Phase 6; `rules_foreign_cc` (0.16.0, has meson) is not
  needed. Nothing in BCR covers cpio initramfs assembly, QEMU test
  execution, or a kernel image, so those stay as a small genrule, an
  `sh_test`-based macro, and a repository rule (Phase 5).
  `hedron_compile_commands` is not in BCR; clangd support is optional dev
  tooling via `git_override` if wanted.
- **Homegrown code now provided by Abseil (delete in 1.1):**
  `RETURN_IF_ERROR`/`ASSIGN_OR_RETURN` in `dcfs/status.h` ->
  `ABSL_RETURN_IF_ERROR`/`ABSL_ASSIGN_OR_RETURN`
  (`absl/status/status_macros.h`, target `@absl//absl/status:status_macros`;
  supports declarations, references, structured bindings, and a third
  argument `_ << "context"`); `Prepend`/`Append` in `dcfs/status.{h,cc}` ->
  `absl::StatusBuilder` (`status_builder.h`; `SetPrepend()`/`SetAppend()`/
  default annotate join, `SetPayload`, `LogError()`); the `absl_nonnull`/
  `absl_nullable` stubs in `dcfs/attributes.h` -> `absl/base/nullability.h`;
  ad-hoc test assertions -> `absl_testing::IsOk`, `IsOkAndHolds`,
  `StatusIs`, `ABSL_EXPECT_OK`/`ABSL_ASSERT_OK` (`status_matchers.h`,
  testonly target `@absl//absl/status:status_matchers`). Not in Abseil:
  `RET_CHECK` (keep ours, reimplemented on `StatusBuilder`), the errno
  name table (`strerrorname_np`-based; keep), `RawMemoryDeleter`,
  `StrongNumber`, `LOG_IF_ERROR`.
- Build: bzlmod; `MODULE.bazel` pins Abseil 20250127.1, sqlite3 3.49.1,
  libfuse 3.16.2 (BCR; lacks the new capability), rules_cc 0.1.1, and an
  unhermetic `new_local_repository` on `/usr` for libmount. Host: Bazel
  9.2.0 via bazelisk. `.bazelrc`: C++20, `-Werror`.

## Decisions (made with russ, 2026-09-27)

1. **Code now: simple, synchronous, single-threaded, reviewable in small
   chunks.** Plain functions and small classes; no template trampolines, no
   threads, no async. Each step below is one reviewable diff.
2. **Eventual architecture: C++ coroutines over io_uring** (FUSE-over-
   io_uring for requests — `CONFIG_FUSE_IO_URING` and libfuse
   `lib/fuse_uring.c` exist in our trees — and io_uring for backing I/O,
   and potentially SQLite itself via its pluggable VFS). No interim
   compromises toward it. Where io_uring or FUSE lack what is needed, the
   answer is kernel work. Today's code only has to *not preclude* that
   future ("Future-proofing rules" below).
3. **Storage: SQLite, WAL.** One connection today because there is one
   thread. In the coroutine/io_uring future: **one connection per worker
   thread (per ring), shared by all coroutines on that thread**, with the
   invariant that a transaction never spans a suspension point (backing
   I/O awaited *before* `BEGIN`; transaction bodies synchronous and short).
   Connection-per-coroutine/pooling is only needed if transactions must
   span awaits, which write-through never requires. SQLite's I/O can later
   move onto the ring through a custom `sqlite3_vfs` (pluggable; xOpen/
   xRead/xWrite/xSync map onto io_uring), so nothing about the wrapper may
   assume the default VFS.
4. **Coherence: exclusive access + write-through.** All access to the
   backing trees goes through dcfs. No fanotify, no TTL revalidation.
5. **Fds and serializable file handles only, never paths.** After startup
   the daemon holds no path strings. This is what makes mounting dcfs *over*
   the directory it caches legal and a supported configuration.
6. **Cache xattr values too**; only file contents bypass the cache.
7. **Multiple backing filesystems per mount are supported:** mount points
   and btrfs subvolumes below the source are crossed and cached. The
   filesystem id is therefore part of every backing identity.
8. **Filesystem identity = the filesystem UUID from `ioctl(fd,
   FS_IOC_GETFSUUID)`, always** (reboot-stable on ext4, xfs, btrfs; ZFS
   returns ENOTTY until OpenZFS ships the ioctl, which is imminent — a unit
   test documents the ENOTTY and will flip). No `f_fsid` fallback. **At
   startup the daemon purges cached state of every filesystem that is not
   currently mounted below the source.** Everything is derived from fds;
   no `/proc/self/mountinfo`, no libmount. Option kept open: a small
   mountinfo parser or libmount can be added later purely for diagnostics
   (device/mount-point names in logs); identity never depends on it.
9. **Identity model:** nodeid is decoupled from the backing filesystem.
   The DB maps our 64-bit nodeid -> `(device_id, backing_ino, backing_gen)`;
   users see the backing inode number; the generation is ours. **Accepted
   limitation: NFS handles do not survive deleting the cache DB** (they
   get ESTALE, never a wrong file). Handles do survive daemon restarts.
   We deliberately do not pursue the kernel work (handle encode/decode
   passthrough) that would lift this; the feature is not needed.
10. **Dependencies at their latest versions** — verified on
    registry.bazel.build 2026-09-27: `abseil-cpp` **20260817.0**,
    `sqlite3` **3.53.4**, `googletest` **1.18.0.bcr.1**, `rules_cc`
    **0.2.25**, `rules_shell` **0.8.0**. Re-check at execution time.
    Abseil 20260817.0 ships `absl/status/status_macros.h`,
    `status_builder.h` and `status_matchers.h`, which replace several
    homegrown pieces (see 1.1).
11. **Filesystem coverage: ext4, btrfs (incl. subvolumes), xfs tested in
    QEMU; ZFS later** (needs OpenZFS with `FS_IOC_GETFSUUID`). A host unit
    test gated on `DCFS_TEST_ZFS_PATH` asserts today's ENOTTY and is
    flipped to a success assertion when ZFS ships the ioctl.
12. **All build and test infrastructure is Bazel**, including the QEMU
    end-to-end tests (`bazel test --config=qemu //test/qemu/...`). The
    kernel under test is `~/Sources/linux` for now (repository rule reading
    a `--repo_env`), pinned to a released kernel once the patch is upstream.
13. **Repo:** seed from the old `~/Sources/dfs` git history, archive it, delete
    `deleteme/`, `dcfs/old/`, `dcfs/example.cc`, stale `.gitmodules`.

### Identity model

Backing identity of an object = `(device_id, backing_ino, backing_gen)`
where `device_id` identifies the filesystem (see Device identity) and
`backing_gen` is `FS_IOC_GETVERSION`. Rows are unique on that triple, so
hard links share a row.

- `inodes.id INTEGER PRIMARY KEY AUTOINCREMENT` (64-bit, never reused) is
  **our** identity and the **FUSE nodeid**. Backing identifiers never serve
  as our keys (with several filesystems, backing inode numbers are not even
  unique).
- `st_ino` reported to users = `backing_ino` (hardlink-aware tools behave
  as on the backing fs; the kernel does not require `st_ino == nodeid`).
  Different backing filesystems can present colliding `st_ino`s to users,
  as any multi-mount view does; a later option may offset them.
- FUSE generation (LOOKUP/GETATTR/SETATTR replies) = `inodes.fuse_gen`, a
  32-bit value we mint from a counter in `meta` that is **randomly seeded
  at DB creation**, so a rebuilt cache cannot reissue an old `(id, gen)`
  pair: a handle from before a wipe gets ESTALE, never a different file.
  Root (nodeid 1) reports 0. When the backing fs recycles an inode number
  (new `backing_gen`), a new row (new `id`, new `fuse_gen`) is created and
  the old row invalidated, so the patched kernel marks the old inode bad.
- **Why handles cannot survive a cache wipe (and why we accept that):**
  the FUSE handle is the kernel's fixed `(nodeid, generation)` and
  reconnection sends only the nodeid; with multiple backing filesystems no
  encoding of `(device_id, ino, gen)` fits, and even for one filesystem xfs needs
  the exact generation (`fs/xfs/xfs_export.c:20-23,163`). Reconnecting
  after a wipe would need the daemon to own handle encoding in the kernel;
  we do not need the feature, so we do not add it. Handles survive daemon
  restarts (rows persist) and a wipe yields ESTALE, which is safe.

### Fd/handle-only rules (enables mounting over the source)

1. At startup, before mounting, `source_fd = open(source, O_PATH |
   O_DIRECTORY)` and `db = open(cache_db_path)`; those are the last times a
   path is used. No `/proc/self/mountinfo` parsing, no libmount.
2. `open_by_handle_at(mount_fd, handle, flags)` accepts any fd within the
   target filesystem as `mount_fd`. `MountFds` keeps one `O_PATH` fd per
   device id: `source_fd` for the source filesystem; for a filesystem mounted
   below, the fd obtained by `openat(parent_dir_fd, name, O_PATH)` while
   populating the parent (a boundary is detected by `statx`
   `STATX_MNT_ID_UNIQUE` changing; btrfs subvolumes appear the same way).
   Never open a mount point by path.
3. All backing operations use `*at` syscalls relative to fds obtained from
   handles (`openat`, `fstatat(fd, "", AT_EMPTY_PATH)`, `readlinkat`,
   `fgetxattr`/`flistxattr` on a real fd obtained by reopening an `O_PATH`
   fd through `/proc/self/fd/N` where required, `linkat(..., AT_EMPTY_PATH)`,
   `renameat2`, ...). Children are reached with `openat(dir_fd, name)`,
   never a joined path.
4. Handles are stored serialized (`handle_type`, bytes) with their device
   id and are the only durable reference to a backing object.

### Device identity and the startup purge

- `DeviceId GetDeviceId(int fd)`: `ioctl(fd, FS_IOC_GETFSUUID)` (generic
  VFS ioctl since Linux 6.5, returns the superblock UUID: ext4, xfs, btrfs;
  OpenZFS support is at head, released versions return ENOTTY, which maps
  to `absl::UnimplementedError` naming the fstype from `fstatfs().f_type`).
  Btrfs wrinkle: all subvolumes of one filesystem share the UUID but have
  separate inode-number spaces, so for `f_type == BTRFS_SUPER_MAGIC` the id
  also carries the subvolume id from `BTRFS_IOC_GET_SUBVOL_INFO` (stable,
  on-disk, unprivileged). `DeviceId = {uuid[16], subvol_id}` with
  `subvol_id = 0` elsewhere. Verified in the btrfs QEMU test (5.2).
- **No mountinfo, no libmount.** libmount's only roles were turning
  `name_to_handle_at`'s `mount_id` into a mount-point *path* (to open a
  mount fd) and finding a device UUID; both come from fds now (mount fds:
  `source_fd` and `openat(parent_fd, name, O_PATH)` walks; UUID: the
  ioctl). Deleted in 0.4: `dcfs/mount_table.*`, `dcfs/mount_fd_cache.*`,
  `dcfs/device_uuid.h`, `third_party/BUILD.util_linux`, the `/usr`
  `new_local_repository`. If device or mount-point *names* are wanted in
  logs later, add a small `/proc/self/mountinfo` parser or the libmount
  dependency then; identity must never depend on it.
- `filesystems(device_id BLOB PRIMARY KEY, fstype INTEGER, parent_inode
  INTEGER NULL, boundary_name BLOB NULL)` records each filesystem below the
  source and the dentry through which it was entered; its mount fd is
  re-derived at startup (below), not stored.
- **Startup purge:** compare `GetDeviceId(source_fd)` with
  `meta.source_device_id`; on mismatch refuse to start (the cache belongs
  to a different filesystem — the operator chooses to delete it). Then,
  walking `filesystems` from the source down, open each boundary directory
  via `open_by_handle_at` on its *parent* filesystem's mount fd +
  `openat(boundary_name, O_PATH)` and compare device ids; any filesystem
  that is not mounted there (or is a different one) has all of its rows
  deleted (dentries, inodes, xattrs, symlinks, its `filesystems` row, and
  its descendants' rows). Mount boundaries are re-discovered on the next
  population of their parent directory. Unmounting a sub-filesystem is
  therefore equivalent to a cold cache for that subtree — never wrong data.

### Crash robustness of write-through (no data loss, bounded staleness)

Mutations are two-phase so a crash between the backing change and the
cache update can only cost a repopulation, never wrong data:
1. transaction: mark the affected state **unknown** (delete the dentry
   rows involved and clear `children_complete` on the parent; clear
   `attrs_valid` on attribute targets);
2. backing syscall;
3. transaction: write the new state.
Unknown state is repopulated from the backing filesystem on next access.

### Future-proofing rules (so the coroutine/io_uring rewrite is mechanical)

- Every cache and backing operation takes an explicit `Context&`
  (`{sqlite3::Connection& db; MountFds& mounts;}`); no globals, no
  singletons, no `thread_local`.
- A transaction never spans a point that could later suspend: **backing
  I/O first, then one short synchronous transaction.** No `Statement`
  cursor is held across a backing syscall.
- Abseil's `ABSL_RETURN_IF_ERROR`/`ABSL_ASSIGN_OR_RETURN` embed `return`;
  coroutines will need `CO_*` variants written on the same
  `StatusBuilder` adaptors — keep any local macros in one header.
- libfuse permits deferred replies from any thread; `FuseRequest` stays
  movable and owns exactly one reply.
- `dcfs/syscalls.{h,cc}` remains the single thin wrapper over the kernel
  API. Layering rule: only `dcfs/backing.{h,cc}` touches the backing
  filesystems; the cache and FUSE op layers never call `syscalls::`
  themselves, so the io_uring rewrite replaces one module.
- The SQLite wrapper opens connections through one factory that accepts a
  VFS name, so an io_uring `sqlite3_vfs` can be dropped in later.
- Kernel work to raise when the time comes: io_uring ops for `getdents64`,
  `open_by_handle_at`, `name_to_handle_at`, `ioctl`.

## Phase 0 — Repository and toolchain

### 0.1 Git repo seeded from the old `~/Sources/dfs` history
- In `~/Sources/dfs` commit the 3 uncommitted ublk edits ("ublk: drop
  GetControlDevice"). `cp -a ~/Sources/dfs/.git ~/Sources/dircachefs/.git`;
  in dircachefs `git rm -r --cached . && git add -A`; commit "Rename dfs to
  dcfs; pivot from ublk block device to FUSE metadata cache (snapshot)".
  (`dfs` was the 2023 name; everything from here on is `dcfs`.)
- Delete `deleteme/`, `dcfs/old/`, `dcfs/example.cc`, both `.gitmodules`,
  `third_party/BUILD.util_linux`, all `*.sw[po]`; new `.gitignore`
  (`/bazel-*`, `*.swp`); commit. (`third_party/` will hold only the libfuse
  patch after 0.3.)
- `mkdir -p ~/Sources/archive && mv ~/Sources/dfs ~/Sources/archive/dfs`.
- New `README.md`: purpose, "write-through cache only", the identity,
  fd-only, device-identity/purge and future-proofing sections, build/test
  commands, links to the patch and blog post.
- **Done when:** history shows 2023 commits + snapshot + cleanup; tree is
  `dcfs/`, `third_party/`, Bazel files, README.

### 0.2 Bazel refresh, latest dependencies
- `.bazelversion` = installed version. `MODULE.bazel`: `abseil-cpp`
  20260817.0, `rules_cc` 0.2.25, `sqlite3` 3.53.4, add `googletest`
  1.18.0.bcr.1 and `rules_shell` 0.8.0 (re-check BCR for newer).
- `.bazelrc`: keep C++20 `-Werror`; `test --test_output=errors`; keep
  `asan`, add `ubsan`; `test --test_tag_filters=-qemu` by default and
  `--config=qemu` enabling it; `build --disk_cache=~/.cache/bazel-disk-cache`
  (shared across the parallel worktrees). `.clang-format` (Google,
  2-space), `tools/format.sh`.
- `dcfs/smoke_test.cc` (one trivial gtest).
- **Done when:** `bazel test //dcfs:smoke_test` passes.

### 0.3 Patched libfuse via the BCR module + a patch (no custom Bazel glue)
- BCR has `libfuse` **3.18.2** with an overlay `BUILD.bazel` that already
  builds everything including `lib/fuse_uring.c` (deps `liburing` 2.14 and
  `numactl` from BCR, `HAVE_URING` on, static library, no linker script).
  Use it instead of the old dfs repo's hand-maintained
  `third_party/libfuse/*` and instead of a submodule: `bazel_dep(name = "libfuse", version = "3.18.2")`
  plus `single_version_override(module_name = "libfuse", patches =
  ["//third_party/libfuse:0001-attr-generation.patch"], patch_strip = 1)`.
- Produce the patch from `~/Sources/libfuse` commit `0aa1acc6`, limited to
  `include/` and `lib/` (the `passthrough_ll.c` example is not needed):
  `git format-patch -1 0aa1acc6 --stdout -- include lib`. A dry run against
  the 3.18.2 tarball shows 4 trivial context conflicts (the 7.46/7.47
  changelog comment in `fuse_kernel.h`, two INIT-flag hunks in
  `fuse_lowlevel.c` whose neighbouring lines are newer flags, and the
  `FUSE_3.19` versionscript node — put the symbol under `FUSE_3.18` there).
  Rebase by hand in a scratch copy (`git archive fuse-3.18.2`), verify with
  `patch -p1 --dry-run`, commit the patch file. Never modify
  `~/Sources/libfuse` itself for this.
- `FUSE_USE_VERSION` -> `FUSE_MAKE_VERSION(3, 18)` in dcfs. Drop the patch
  when a libfuse release includes the feature.
- **Done when:** `dcfs/libfuse_version_test.cc` asserts
  `FUSE_CAP_ATTR_GENERATION` is defined and links
  `fuse_reply_attr_with_generation`; `bazel mod graph` shows no
  `local_path_override`.

### 0.4 Filesystem identity without libmount
- New `dcfs/device_id.{h,cc}`: `struct DeviceId {std::array<uint8_t, 16>
  uuid; uint64_t subvol_id;}`, `GetDeviceId(int fd)` (`FS_IOC_GETFSUUID`;
  on `BTRFS_SUPER_MAGIC` also `BTRFS_IOC_GET_SUBVOL_INFO`; `ENOTTY` ->
  `absl::UnimplementedError("<fstype> does not support FS_IOC_GETFSUUID")`),
  `Serialize`/`Parse`, hashing, `FstypeName(f_type)` for messages.
  New `dcfs/mount_fds.{h,cc}`: `MountFds` = `DeviceId -> FileDescriptor
  (O_PATH)`; `Insert`, `Get`, `Erase`.
- Delete `dcfs/mount_table.*`, `dcfs/mount_fd_cache.*`, `dcfs/device_uuid.h`,
  `third_party/BUILD.util_linux`, the `/usr` repo rule.
- Tests: `device_id_test` on `/` and `$TEST_TMPDIR` (skips with a message
  if the test filesystem itself lacks the ioctl, e.g. tmpfs); a test gated
  on `DCFS_TEST_ZFS_PATH` that **asserts the current ENOTTY ->
  Unimplemented mapping on ZFS** and is marked in a comment to be flipped
  when OpenZFS ships `FS_IOC_GETFSUUID`.
- **Done when:** tests pass; no `libmount.h` include remains.

### 0.5 Make the tree compile (mechanical only)
- Fix the syntax/typo errors listed in Context so `bazel build //...` is
  green without redesign. Leave `inode.*`, `dentry.*`, `directory.*`,
  `fs_db.*` out of the build (Phase 2 replaces them; delete in 2.2).
- Replace the concept/template trampoline in `dcfs/fuse.h` with a plain
  `fuse_lowlevel_ops` table of static functions; keep `FuseRequest`.
- **Done when:** `//dcfs:main` mounts an empty tmpfs-backed dir, answers
  `ls`, unmounts cleanly.

## Phase 1 — Foundation libraries, hardened and tested

### 1.1 Replace homegrown status utilities with Abseil's
- Delete `RETURN_IF_ERROR`/`ASSIGN_OR_RETURN`/`LOG_IF_ERROR` from
  `dcfs/status.h`; mechanically rewrite all call sites to
  `ABSL_RETURN_IF_ERROR`/`ABSL_ASSIGN_OR_RETURN` (`absl/status/
  status_macros.h`), using the third argument (`_ << "..."`) where a
  call site currently wraps with `Prepend`/`Append`. Delete `Prepend`/
  `Append`; use `absl::StatusBuilder(st).SetPrepend() << "ctx: "` or the
  default annotate join. Replace `LOG_IF_ERROR(level, expr)` uses with
  `LOG_IF(level, !s.ok())` or `StatusBuilder(...).LogError()`.
- Delete `dcfs/attributes.h`; include `absl/base/nullability.h`
  (`absl_nonnull`/`absl_nullable` are real annotations there).
- Keep `dcfs/ret_check.h` but implement `RET_CHECK*` on
  `absl::InternalErrorBuilder()`-style `StatusBuilder` so callers can add
  context (`RET_CHECK_EQ(a, b) << "while ..."`); messages include operand
  values; add `RET_CHECK_OK`.
- Keep the errno helpers (`ErrnoToStatus` with payload,
  `GetErrnoFromStatus`, `ErrnoToErrorName`/`ErrorNameToErrno`); add
  `StatusToErrno(const absl::Status&)` (payload first, else the code table
  currently in `fuse.cc`).
- Tests use `absl_testing::IsOk`/`StatusIs`/`IsOkAndHolds` and
  `ABSL_EXPECT_OK` (`@absl//absl/status:status_matchers`): errno-name round
  trip for every entry in `errno.cc`; payload survives `StatusBuilder`
  annotation; `RET_CHECK` produces `kInternal` with both operands.

### 1.2 `FileDescriptor` and `syscalls`
- Add fd-based wrappers: `statx`, `fstatfs`, `readlinkat`,
  `fgetxattr`/`flistxattr`/`fsetxattr`/`fremovexattr` plus
  `ReopenPathFd(fd, flags)` (via `/proc/self/fd`), `linkat`, `unlinkat`,
  `renameat2`, `mkdirat`, `mknodat`, `symlinkat`, `fchmod`,
  `fchownat(AT_EMPTY_PATH)`, `futimens`, `ftruncate`, `fsync`, `fallocate`,
  `pread`/`pwrite`; keep `getdents64`, `GetInodeGeneration`,
  `name_to_handle_at`, `open_by_handle_at`. Remove unused signal/pthread/
  mount wrappers and `mount.*`.
- Tests under `$TEST_TMPDIR`.

### 1.3 SQLite wrapper
- Finish `Connection`/`Statement`/`WithSavepoint`; add
  `Connection::Transaction(absl::FunctionRef<absl::Status()>)`. A
  `ConnectionFactory{path, flags, vfs_name}` opens connections (VFS
  pluggable later). Open with `SQLITE_OPEN_NOMUTEX|SQLITE_OPEN_EXRESCODE`;
  pragmas `journal_mode=WAL`, `synchronous=NORMAL`, `foreign_keys=ON`,
  `busy_timeout=5000`, `temp_store=MEMORY`. Add `Column<std::optional<T>>`,
  `Column<bool>`, `Bind(std::optional<T>)`, `Bind(bool)`,
  `Bind(std::span<const uint8_t>)`, `ForEachRow`; remove the per-step
  `LOG(INFO)`.
- `StatementCache` owned by the `Connection` (`Get(sql)` returns a reset
  prepared statement) so no other class holds `Statement` members.
- Tests: nested savepoints, rollback on error, WAL active, cache reuse,
  `:memory:` and file-backed.

### 1.4 File handles
- `dcfs/file_handle.{h,cc}` as a value type: `struct FileHandle {DeviceId
  device; int handle_type; std::vector<uint8_t> bytes;}` with
  `Serialize()`/`Parse()`, `FileHandle::FromFd(int fd)` (`name_to_handle_at(
  fd, "", AT_EMPTY_PATH)` + `GetDeviceId`), `Open(const MountFds&, flags)
  -> FileDescriptor` (mount fd looked up by device id).
- Tests: round trip on a temp dir (unprivileged); `Open` tests tagged
  `requires-root` and run in the QEMU suite.

## Phase 2 — Metadata cache core (no FUSE)

### 2.1 Schema v1 and migrations
- `dcfs/schema.sql` embedded via genrule; `Migrate(Connection&)`;
  `meta(key TEXT PRIMARY KEY, value BLOB)` holds `schema_version`,
  `gen_counter` (random 32-bit seed at creation), `source_device_id`.
- Tables: `filesystems(device_id BLOB PRIMARY KEY, fstype INTEGER,
  parent_inode INTEGER NULL, boundary_name BLOB NULL)`; `inodes(id INTEGER
  PRIMARY KEY AUTOINCREMENT, device_id BLOB NOT NULL REFERENCES
  filesystems(device_id) ON DELETE CASCADE, backing_ino INTEGER NOT NULL,
  backing_gen INTEGER NOT NULL,
  fuse_gen INTEGER NOT NULL, handle_type INTEGER, handle BLOB, attrs_valid
  BOOL, mode, nlink, uid, gid, rdev, size, blocks, blksize, atime_s,
  atime_ns, mtime_s, mtime_ns, ctime_s, ctime_ns, btime_s, btime_ns,
  xattrs_complete BOOL, UNIQUE(device_id, backing_ino, backing_gen))`;
  `dentries(parent INTEGER REFERENCES inodes(id) ON DELETE CASCADE, name
  BLOB, inode INTEGER NULL REFERENCES inodes(id) ON DELETE SET NULL,
  PRIMARY KEY(parent, name))` (`inode IS NULL` = negative entry);
  `directories(inode PRIMARY KEY, children_complete BOOL)`;
  `symlinks(inode PRIMARY KEY, target BLOB)`; `xattrs(inode, name BLOB,
  value BLOB, PRIMARY KEY(inode, name))`. Index `dentries(inode)`.
- Root row inserted at creation with `id = 1` (= `FUSE_ROOT_ID`).
- Tests: fresh DB migrates; reopen is a no-op; cascade from `filesystems`.

### 2.2 `MetadataCache` (all methods take `Context&`)
- Reads: `Lookup(parent_id, name) -> Found{id}|Negative|Unknown`,
  `GetAttr(id)` (`st_ino = backing_ino`), `GetGeneration(id)`,
  `ListDir(id, cursor, cb)` (cursor = dentry rowid), `IsDirComplete`,
  `Readlink`, `ListXattrs`, `GetXattr`, `GetHandle`, `ParentOf(dir id)`,
  `ListFilesystems`.
- Writes (each one transaction): `UpsertInode(FileHandle, statx, gen)`
  (mints `fuse_gen` on insert; a changed `backing_gen` creates a new row
  and invalidates the old), `LinkDentry`, `UnlinkDentry`, `RenameDentry`,
  `SetNegative`, `MarkDirComplete`, `MarkUnknown`, `UpdateAttr`,
  `SetSymlink`, `ReplaceXattrs`, `SetXattr`, `RemoveXattr`,
  `InvalidateInode`, `AddFilesystem`, `PurgeFilesystem(device_id)`
  (cascades).
- Delete `fs_db.*`, `inode.*`, `dentry.*`, `directory.*`.
- Tests: every method; hardlinks share a row; negatives; rename across
  parents; unlink of last link removes row, xattrs, symlink; purge removes a
  whole subtree; two-phase `MarkUnknown` leaves a repopulatable state.

### 2.3 `Backing` and population policy
- `dcfs/backing.{h,cc}` — the only module touching the backing fs (via
  `syscalls.h`): `OpenNode(Context&, id, flags)` (open via handle on that
  device id's mount fd, verify `(st_ino, generation)` against the row, else
  `InvalidateInode` + EIO), `StatNode`, `ReadSymlink`, `ReadXattrs`,
  `PopulateDirectory(Context&, dir id)`: `getdents64` loop; per child
  `openat(O_PATH)`, `statx` (`STATX_MNT_ID_UNIQUE`), on a new mount id
  `GetDeviceId` + `MountFds.Insert` + `AddFilesystem(parent, name)`, then
  `FS_IOC_GETVERSION`, `name_to_handle_at`, symlink target, xattrs —
  gathered first, then written in **one** transaction, then
  `MarkDirComplete`. The single "spin the disk once per directory" point.
- `Resolver::LookupOrPopulate(Context&, parent_id, name)`: `Lookup`; on
  `Unknown` with parent complete -> `Negative`; else `PopulateDirectory`
  then `Lookup` again.
- `StartupPurge(Context&)` per the Device identity section.
- Tests on a temp tree (incl. a bind-mounted or tmpfs subdirectory as a
  second filesystem when the test runs with privileges; otherwise a
  single fs): populate; then make the backing tree unreadable and assert
  every read still answers from cache; negative path answers ENOENT without
  a backing syscall; purge test: unmount the sub-fs, run `StartupPurge`,
  assert its rows are gone and the boundary dir is re-discoverable.

## Phase 3 — FUSE daemon, read-only

### 3.1 Plain op table and reply helpers
- `dcfs/fuse_ops.{h,cc}`: hand-written `fuse_lowlevel_ops` of static
  functions, each building a `FuseRequest`, calling `DirCacheFS::Xxx`, and
  replying the error on failure. `FuseRequest` (`dcfs/fuse_request.*`) gains
  `ReplyEntry(nodeid, gen, stat, timeouts)`, `ReplyAttr(stat, timeout,
  gen)` (via `fuse_reply_attr_with_generation`), `ReplyReadlink`,
  `ReplyData`, `ReplyXattr`, `ReplyStatfs`, `ReplyDirsPlus`; `ReplyErrno`
  uses `StatusToErrno`.

### 3.2 Read-only ops from cache
- `Init`: request `FUSE_CAP_EXPORT_SUPPORT`, `FUSE_CAP_ATTR_GENERATION`,
  `FUSE_CAP_READDIRPLUS`, `FUSE_CAP_CACHE_SYMLINKS`, `FUSE_CAP_PASSTHROUGH`.
  `Lookup` (incl. `"."`/`".."` for export reconnection), `Getattr`,
  `Readdir`/`Readdirplus`, `Readlink`, `Listxattr`, `Getxattr` (from cache),
  `Access`, `Statfs` (backing `fstatfs` of the node's filesystem; cheap),
  `Forget` (no-op; rows persist).
- Kernel timeouts default long (1h) because access is exclusive; Phase 4
  invalidates after our own mutations.
- **Done when:** `strace -f -e trace=%file,getdents64` on the daemon shows
  **zero** backing syscalls during a second `find /mnt -ls` and
  `getfattr -d -R /mnt`, including across a submount.

### 3.3 File contents via FUSE passthrough (the only disk touch)
- `FUSE_PASSTHROUGH` (Linux ≥ 6.9, libfuse ≥ 3.17): `Open` does `OpenNode`
  then `fuse_passthrough_open(req, fd)` and sets `fi->backing_id`; the
  kernel performs read/write/mmap directly on the backing file. Keep the fd
  in `fi->fh` for the fallback `Read`/`Write` when the kernel refuses
  passthrough; `Release` closes both. (Incompatible with writeback caching,
  which we do not enable.)
- **Done when:** `cat` works; a large `dd` runs at disk speed with ~0
  daemon CPU.

### 3.4 Export support with handles
- Copy `~/Sources/fuse-generation-qemu/guest/fhtest.c` to `tools/fhtest.c`
  (a full copy; `cc_binary`); `tools/handle_tests.sh` (root): `name_to_handle_at`
  returns `(id, fuse_gen)`; `open_by_handle_at` works after a daemon
  restart with the same DB, also for a file on a submount; a doctored
  generation -> ESTALE; a recycled backing inode -> ESTALE and `fstat` on
  an old fd -> EIO; after a DB wipe -> ESTALE.

### 3.5 Daemon lifecycle and CLI
- `main.cc`: `--source`, `--cache_db`, mountpoint positional, timeouts,
  `--foreground`, `-o` passthrough. Startup: open `source_fd` before
  mounting, migrate DB, check `meta.source_device_id` (refuse on
  mismatch), `StartupPurge`, ensure root row, mount. Mounting over the source
  directory is supported and tested. SIGTERM: unmount, WAL checkpoint.
  `packaging/dcfs.service`.

## Phase 4 — Write-through mutations

Rule per op: (1) transaction marking affected state unknown, (2) backing
syscall, (3) transaction writing the new state, then reply. Each step's
tests check the backing change happened and that a following read is
served with zero backing syscalls. Cross-filesystem `Rename`/`Link` return
EXDEV exactly as the backing filesystems do.

- 4.1 `Setattr` (chmod/chown/truncate/utimens) -> `statx` -> `UpdateAttr`.
- 4.2 `Create`, `Mknod`, `Mkdir`, `Symlink`, `Link` -> `UpsertInode` +
  `LinkDentry` (+ `SetSymlink`).
- 4.3 `Unlink`, `Rmdir`, `Rename` (incl. `RENAME_EXCHANGE`/`NOREPLACE`) ->
  dentry ops; drop rows at nlink 0 with no open handles;
  `fuse_lowlevel_notify_inval_entry` for names the kernel may still cache.
- 4.4 `Write` (passthrough moves data; on `Release`/`Flush`/`Fsync` one
  `statx` -> `UpdateAttr`), `Fallocate`, `Fsync`, `Setxattr`/`Removexattr`.
- 4.5 POSIX conformance with **pjdfstest** (Pawel Jakub Dawidek's
  filesystem test suite from FreeBSD, github.com/pjd/pjdfstest: ~8000
  checks of chmod/chown/link/mkdir/open/rename/unlink/... semantics and
  errnos; used by ZFS, gVisor and FUSE filesystems). Run against a dcfs
  mount over ext4 (root); fix deviations; record known failures in
  `docs/conformance.md`.

## Phase 5 — QEMU end-to-end tests in Bazel (`test/qemu/`)

- 5.1 Create `test/qemu/` by **copying** (plain file copies, no git
  subtree) only what dircachefs needs from `~/Sources/fuse-generation-qemu`:
  `scripts/build-kernel.sh`, `scripts/mkinitramfs.sh`, `scripts/run-qemu.sh`
  and `guest/init` as starting points, adapted here. That repo itself is
  left untouched (it tests the LKML kernel patch and may be shared with
  reviewers). Then Bazel-ify:
  - `test/qemu/kernel.bzl`: repository rule `kernel_image` exposing
    `bzImage` from `--repo_env=DCFS_KERNEL_BUILD=<out-of-tree build dir>`.
    `test/qemu/scripts/build-kernel.sh` builds `~/Sources/linux` into
    dircachefs's own build dir (never into fuse-generation-qemu's) with the
    config extended: `BTRFS_FS`, `XFS_FS`, `NFSD`, `NFS_FS`, `NFSD_V4`,
    `NFS_V4`, `SUNRPC`, `FUSE_IO_URING`, `IO_URING`. Later an `http_file`
    of a released kernel.
  - `//test/qemu:initramfs` genrule: host busybox (exposed by the same
    repo rule), `//dcfs:main` built `--features=fully_static_link`,
    `//tools:fhtest`, guest scripts -> `initramfs.cpio.gz` (cpio/gzip as
    declared host tools).
  - `qemu_test` macro (`sh_test` around `scripts/run-qemu.sh`), `tags =
    ["qemu", "exclusive"]`, `size = "large"`; disk images created per test
    by the runner (`mkfs.ext4`/`mkfs.btrfs`/`mkfs.xfs` on the host;
    documented prerequisites).
- 5.2 `//test/qemu:dcfs_ext4_test`, `dcfs_btrfs_test`, `dcfs_xfs_test`:
  backing disk `/dev/vdb` formatted per fs, a second disk `/dev/vdc` (ext4)
  mounted *below* the source as a submount (and for btrfs a subvolume),
  cache DB on tmpfs. Tests: populate across the boundary; **sectors-read of
  `/dev/vdb` and `/dev/vdc` unchanged across a metadata-only workload**
  (find, stat, getfattr -d, readlink); `cat` changes it; mount dcfs *over*
  the source dir and repeat; write-through cases incl. EXDEV across the
  boundary; handle tests incl. daemon restart; recycled-inode ESTALE/EIO;
  **startup purge**: unmount `/dev/vdc`, restart dcfs, the submount subtree
  is gone from the cache and the boundary dir is an empty directory; remount
  and it repopulates.
- 5.3 Real NFS: `//test/qemu:rootfs_debian` genrule (mmdebstrap in unshare
  mode; nfs-kernel-server, nfs-common, attr, strace) as an alternate root
  image (tag `manual` if it cannot run sandboxed); `dcfs_nfs_test`: export
  the dcfs mount (`crossmnt`), mount over NFSv4 loopback, repeat the
  sectors-read test over NFS including the submount, hold a file open on
  the client, **restart dcfs**, keep reading/writing without ESTALE; wipe
  the DB, restart, expect ESTALE.
- 5.4 `.github/workflows/ci.yml`: `bazel test //...` now; the `qemu` config
  once the kernel is pinnable.

## Phase 6 — Future: coroutines over io_uring (not scheduled)

Preconditions accumulated by the rules above. Work items when the time
comes: coroutine `Task<>` and `CO_*` status macros; FUSE-over-io_uring via
libfuse (`FUSE_CAP_OVER_IO_URING`; the BCR libfuse module already builds
`fuse_uring.c`; `liburing` 2.15 is in BCR); io_uring awaitables for `statx`, `openat2`,
`read`/`write`, `fsync`, `fallocate`, `*xattr`, `linkat`/`unlinkat`/
`renameat`/`mkdirat`/`symlinkat`; an io_uring `sqlite3_vfs` if SQLite I/O
shows up; kernel patches for the missing ops (`getdents64`,
`open_by_handle_at`, `name_to_handle_at`, `ioctl`); one ring + one SQLite
connection per worker thread.

## Verification (definition of done for the whole plan)

1. `bazel test //...` and `--config=asan` green on the host.
2. `make -C ~/Sources/fuse-generation-qemu test` still passes (the kernel
   patch's own suite; unchanged by this work).
3. `bazel test --config=qemu //test/qemu:dcfs_{ext4,btrfs,xfs}_test` pass:
   warm metadata workload reads zero backing sectors on both disks; file
   read touches backing; over-mounting the source works; submount and
   subvolume caching, EXDEV, and startup purge behave; handles survive
   daemon restart; recycled inodes -> ESTALE/EIO; write-through cases pass.
4. `//test/qemu:dcfs_nfs_test` passes including daemon restart under an
   open NFS client.
5. pjdfstest results documented; no regressions across steps.

## Critical files

- `dcfs/BUILD.bazel`, `MODULE.bazel`, `.bazelrc`,
  `third_party/libfuse/0001-attr-generation.patch`
- `dcfs/status.{h,cc}`, `dcfs/errno.cc`, `dcfs/ret_check.h` (Abseil's
  `status_macros.h`/`status_builder.h` replace the rest)
- `dcfs/syscalls.{h,cc}`, `dcfs/fd.{h,cc}`
- `dcfs/sqlite.{h,cc}` (+ statement cache, connection factory)
- `dcfs/device_id.{h,cc}`, `dcfs/mount_fds.{h,cc}`, `dcfs/file_handle.{h,cc}`
- `dcfs/schema.sql`, `dcfs/metadata_cache.{h,cc}`, `dcfs/backing.{h,cc}`
- `dcfs/fuse_ops.{h,cc}`, `dcfs/fuse_request.{h,cc}`,
  `dcfs/dir_cache_fs.{h,cc}`, `dcfs/main.cc`
- `test/qemu/{BUILD.bazel,kernel.bzl,scripts/*,guest/*}`, `tools/fhtest.c`,
  `tools/handle_tests.sh`

---

## Amendments (2026-09-27, russ, during execution)

1. **`Backing::OpenNode` returns ESTALE** (not EIO) when the object behind a
   handle was replaced; the row is invalidated.
2. **Root is a requirement.** No unprivileged fallback paths anywhere (no
   name-walk when `open_by_handle_at` is EPERM, no `f_fsid` identity
   fallback). The daemon refuses to run usefully without
   `CAP_DAC_READ_SEARCH`.
3. **All tests run inside QEMU, unit tests included.** No host-side test
   execution is supported, so no dependency-injection seams exist for host
   kernels lacking `FS_IOC_GETFSUUID` (delete `Context::device_id_fn`).
   `bazel test //...` boots each test binary in a guest. If test-only
   helpers are ever needed, they are fakes (never mocks), live in a
   `testonly/` directory with `testonly = 1` BUILD rules, or in `*_test.cc`.
4. **QEMU must be fast:** direct kernel boot without firmware where possible
   (`-M microvm` / PVH or `-kernel` with no BIOS/iPXE), a minimal kernel
   config (kvm_guest + virtio + the filesystems under test, nothing else),
   KVM (`sg kvm -c` in the current session), small `-m`, no graphics; the
   goal is sub-second boots so per-test VMs are cheap. Unit tests need not
   be `exclusive`.
5. Subagents may run the QEMU suites (including
   `make -C ~/Sources/fuse-generation-qemu test`) when the orchestrator asks;
   that repo's tracked files stay untouched.
6. `~/Sources/libfuse` was updated (08e3eec1) to bit 44 / protocol 7.47 on
   top of upstream; dircachefs's `third_party/libfuse` patch already matches.
7. **Mount fds are `O_RDONLY | O_DIRECTORY`, never `O_PATH`.** Found by the
   QEMU `readonly_test`: `open_by_handle_at(2)` resolves `mount_fd` with the
   non-raw fd class (`fs/fhandle.c`, `get_path_from_fd`), so an `O_PATH`
   descriptor fails with EBADF. The "Fd/handle-only rules" section's
   "`source_fd = open(source, O_PATH | O_DIRECTORY)`" and "`MountFds` keeps
   one `O_PATH` fd per device id" are corrected accordingly; per-child
   probing still uses `O_PATH`.
8. (russ, 2026-09-28) **Protocol minor becomes 47** in the kernel patch (v2
   to LKML), libfuse and the carried dircachefs patch.
9. (russ, 2026-09-28) **Detect and log out-of-band backing changes wherever
   it is free**, e.g. compare the `statx` taken anyway when opening a node
   with the cached attributes; on a mismatch log a WARNING and refresh the
   attributes; for a directory whose mtime changed mark its children
   incomplete; for a ctime change mark xattrs unknown. No extra syscalls
   and no design compromise; unit tests (in QEMU) cover the trivially
   checkable cases. The exclusive-access assumption stands.
10. (russ, 2026-09-28) **Docs:** README.md comprehensive (purpose, usage,
    build, test, brief design, explicit limitations incl. no out-of-band
    access); new `docs/design.md` with the detailed design and assumptions.
11. (russ, 2026-09-28) **No kernel-cache invalidation on detected out-of-band
    changes.** dcfs updates its own cache and logs, but does not call
    `fuse_lowlevel_notify_inval_*` (doing so safely needs a notifier thread
    to avoid /dev/fuse deadlocks). Staleness until the kernel's attr/entry
    timeouts expire is accepted, since out-of-band changes are unsupported.
    Document in README limitations and docs/design.md.
12. (russ, 2026-09-28) **Submounts are refused for now.** dcfs fails to start
    if any mount lies below --source (checked at startup via mountinfo or
    listmount/statmount: a policy check, identity never depends on it), and
    a boundary discovered at runtime (a later mount, or a btrfs subvolume)
    is logged at ERROR and its name fails with EXDEV instead of being
    cached. Rationale: one superblock means one st_dev, so backing inode
    numbers from several filesystems would collide (wrong hard-link
    detection in tar/rsync/cp -a). Keep device ids in identity, the
    filesystems table, MountFds and boundary detection, so that
    kernel-supported FUSE submounts (FUSE_ATTR_SUBMOUNT, today virtiofs-only;
    would need an INIT opt-in for /dev/fuse) can be added later. st_ino stays
    the backing inode number.
13. (russ, 2026-09-28) **Power-loss safety via a durable dirty set** (step
    4.10): phase 1 records "unknown" and adds the affected inodes/dirs to a
    `dirty` table in a transaction committed with synchronous=FULL (WAL
    fsync) before the backing syscall; phase 3 commits normally and leaves
    the dirty entries; a periodic (a few seconds, when dirty), on-fsync/
    syncfs-request and at-clean-shutdown step `syncfs`es the backing
    filesystem and then clears the dirty set; at startup after an unclean
    shutdown (boot id / clean flag), everything in the dirty set is marked
    unknown and its dentries/xattrs dropped. No per-op backing flush.
14. (russ, 2026-09-28) **Random 32-bit FUSE generation per row** (never 0;
    root 0), replacing meta.gen_counter: a rollback that reuses a nodeid
    yields a fresh generation, so old handles get ESTALE.
15. (orchestrator, pending russ) mmap writes after the last close are not
    observable (no FUSE mmap/munmap request; passthrough mmap drops the FUSE
    file reference). Proposed: document as unsupported + attr_timeout 0
    while open for writing; kernel fix later.
16. (russ, 2026-09-28) Amendment 15 confirmed: mmap writes after the last
    close are documented as unsupported for now (kernel fix later).
17. (russ, 2026-09-28) Replace the untyped `meta` key/value table with a
    typed, documented single-row table (done inside unreleased schema v2).
18. (russ, 2026-09-28) Every cached record representing something on the
    backing filesystem has explicit states present / absent / unknown, set at
    the right times (phase 1 unknown, phase 3 present/absent), correct for a
    concurrent reader at every point (coroutine future). Audit
    (audit-tristate.md) then conversion, starting with xattrs.
19. (russ, 2026-09-28) **Test-first for every bug.** Every problem found
    (audits, reviews, test failures) gets a regression test. Write the test
    first, run it to show it FAILS on the unfixed code (record the failing
    output in the commit message or report), then fix and show it passes.
    When a bug cannot be reproduced deterministically (e.g. coroutine-only
    interleavings, power loss), write the closest honest test and say what
    it does and does not prove. Applies to all remaining steps.
20. (found in step 5.2, 2026-10-02) **btrfs does not implement
    FS_IOC_GETFSUUID** (no super_set_uuid in fs/btrfs; decision 8 was wrong
    about btrfs). Filesystem identity on btrfs comes from BTRFS_IOC_FS_INFO's
    fsid (the on-disk filesystem UUID, reboot-stable; not f_fsid), plus the
    subvolume id as before. ext4 and xfs use FS_IOC_GETFSUUID.

## Original execution waves (process for Phases 0-7)

Steps are grouped into **waves**. Everything inside a wave can run in
parallel, one subagent per step, each in its own git worktree on a branch
named after the step; the orchestrator merges a wave (rebase, run
`bazel test //...`) before starting the next. A **gate** is a wave whose
single step must land before anything else proceeds. Within a wave, steps
**own disjoint files** (listed per step) so merges are conflict-free; a
step that needs a small addition to a file it does not own (e.g. one more
`syscalls::` wrapper) adds it in a separate, minimal commit that the
orchestrator lands first.

Shared Bazel cache across worktrees: `.bazelrc` sets
`build --disk_cache=~/.cache/bazel-disk-cache` so parallel worktrees do not
each rebuild Abseil/SQLite/libfuse.

```
GATE  W0a  0.1 repo seed ─────────────────────────────────────────────┐
GATE  W0b  0.2 Bazel refresh                                          │
      W0c  0.3 libfuse BCR+patch   ║ 0.4 device_id/mount_fds (drop libmount)
GATE  W0d  0.5 tree compiles                                          │
GATE  W1a  1.1 Abseil status macros (touches every file)              │
      W1b  1.2 syscalls/fd         ║ 1.3 sqlite wrapper   ║ 5.1 QEMU infra
      W1c  1.4 file_handle         ║ 2.1 schema/migrate   ║ 3.1 fuse_ops/request ║ 3.4a tools/fhtest
GATE  W2a  2.2 MetadataCache
GATE  W2b  2.3 Backing/Resolver/StartupPurge
GATE  W3a  3.2 read-only FUSE ops
      W3b  3.3 passthrough open/read ║ 3.5 CLI/lifecycle ║ 3.4b handle_tests.sh ║ 5.3a Debian rootfs genrule
      W4a  4.1 setattr             ║ 4.2 create/mkdir/symlink/link
      W4b  4.3 unlink/rmdir/rename ║ 4.4 write/fsync/fallocate/xattr
      W5   5.2 ext4 test ║ 5.2 btrfs test ║ 5.2 xfs test ║ 5.3b NFS test ║ 5.4 CI ║ 4.5 pjdfstest
```

| Step | Depends on | Owns (files/dirs) |
|---|---|---|
| 0.1 | — | whole tree (git init, deletions, README) |
| 0.2 | 0.1 | `MODULE.bazel`, `.bazelrc`, `.bazelversion`, `.clang-format`, `tools/format.sh`, `dcfs/smoke_test.cc` |
| 0.3 | 0.2 | `third_party/libfuse/*.patch`, libfuse lines of `MODULE.bazel`, `dcfs/libfuse_version_test.cc` |
| 0.4 | 0.2 | `dcfs/device_id.*`, `dcfs/mount_fds.*`, deletions of libmount files |
| 0.5 | 0.3, 0.4 | mechanical fixes anywhere; `dcfs/fuse.h` trampoline removal |
| 1.1 | 0.5 | `dcfs/status.*`, `dcfs/ret_check.h`, `dcfs/errno.cc`, call sites everywhere |
| 1.2 | 1.1 | `dcfs/syscalls.*`, `dcfs/fd.*` (+ removal of `mount.*`) |
| 1.3 | 1.1 | `dcfs/sqlite.*` |
| 1.4 | 1.2, 0.4 | `dcfs/file_handle.*` |
| 2.1 | 1.3 | `dcfs/schema.sql`, `dcfs/migrate.*` |
| 2.2 | 2.1, 1.4 | `dcfs/metadata_cache.*`; deletes `fs_db.*`, `inode.*`, `dentry.*`, `directory.*` |
| 2.3 | 2.2 | `dcfs/backing.*`, `dcfs/resolver.*` |
| 3.1 | 1.1, 0.3 | `dcfs/fuse_ops.*`, `dcfs/fuse_request.*` (from `fuse.*`) |
| 3.2 | 2.3, 3.1 | `dcfs/dir_cache_fs.*` |
| 3.3 | 3.2 | open/read/release in `dir_cache_fs.*` |
| 3.4a | 0.2 | `tools/fhtest.c`, `tools/BUILD.bazel` |
| 3.4b | 3.2, 3.4a | `tools/handle_tests.sh` |
| 3.5 | 3.2 | `dcfs/main.cc`, `packaging/` |
| 4.1, 4.2 | 3.3, 3.5 | disjoint method groups in `dir_cache_fs.*` (setattr vs. create family) |
| 4.3, 4.4 | 4.1, 4.2 | disjoint method groups (unlink/rmdir/rename vs. write/fsync/fallocate/xattr) |
| 4.5 | 4.3, 4.4 | `docs/conformance.md` |
| 5.1 | 0.2 | `test/qemu/{BUILD.bazel,kernel.bzl,scripts/*,guest/init}` |
| 5.2 (×3) | 5.1, 3.5 (+4.x for write cases) | `test/qemu/guest/dcfs_{ext4,btrfs,xfs}.sh` |
| 5.3a | 5.1 | `test/qemu/scripts/mkrootfs-debian.sh`, rootfs genrule |
| 5.3b | 5.3a, 3.5 | `test/qemu/guest/dcfs_nfs.sh` |
| 5.4 | 5.1 | `.github/workflows/ci.yml` |

Critical path: 0.1 → 0.2 → 0.3/0.4 → 0.5 → 1.1 → 1.3 → 2.1 → 2.2 →
2.3 → 3.2 → 3.3/3.5 → 4.x → 5.2. Everything else (5.1, 3.1, 3.4a, 1.2,
1.4, 5.3a, 5.4) hangs off it and can be done by parallel agents while the
chain progresses.
