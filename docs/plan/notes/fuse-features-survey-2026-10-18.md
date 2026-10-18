# Optional FUSE features against dcfs (Gemini survey, 2026-10-18)

Corrections by the orchestrator, 2026-10-18. Where this survey and the plan
differ, the plan governs:

- `FUSE_EXPIRE_ONLY` is adopted for 12.20 entry expiry. The survey's "dcfs
  never sends entry invalidations" describes today's state, not the plan.
- `FUSE_CAP_NO_OPENDIR_SUPPORT` is skipped: 22.7 stream handles need
  per-handle state.
- `FUSE_ATTR_GENERATION` (Linux 7.1+, protocol 7.47) is tracked as future
  work; it is related to russ's FUSE generation work.
- `READDIRPLUS_AUTO` is to be set explicitly in Init as a deliberate
  choice (12.24), not left to the default.

---


This document evaluates optional FUSE features from the Linux kernel (`include/uapi/linux/fuse.h`, Linux v7.3-rc4) and `libfuse` (`include/fuse_common.h` 3.18.2) for dcfs, a write-through metadata cache over local ext4/xfs/btrfs backing filesystems.

Evaluations are anchored in dcfs core architectural principles from `docs/design.md`:
1. **Behave like the backing, only faster**: POSIX semantics, error codes, attributes, and atomicity match the backing filesystem.
2. **Fetch the minimum needed to serve the request; cache what was fetched**: Avoid speculative I/O or unnecessary backing operations.
3. **Bounded working memory**: $O(1)$ working memory per request; caches are strictly bounded.
4. **Single-threaded request dispatch**: The daemon state has no locking; concurrency requires careful race modeling.
5. **Kernel floors**: Documented minimum floor is Linux 6.9 (FUSE passthrough and `FS_IOC_GETFSUUID`); deployment kernel is Linux 7.0.

> [!NOTE]
> In `include/uapi/linux/fuse.h`, the protocol changelog explicitly lists protocol minor versions (e.g., `7.21`, `7.39`), but **never records Linux kernel release versions**. Where a kernel version is cited below, it is derived from git commit history; entries strictly conforming to the changelog text are marked `"Not found in changelog"`.

---

## 1. Comprehensive Feature Matrix

| Feature | Kernel Version Added (from Changelog) | libfuse Capability (or mechanism) | dcfs Today | Relevance to dcfs Principles | Recommendation & Rationale | Min Kernel vs Floor (6.9 floor / 7.0 deploy) |
| :--- | :--- | :--- | :--- | :--- | :--- | :--- |
| **ATOMIC_O_TRUNC** | Not found in changelog (base 7.1; Linux 2.6.14) | `FUSE_CAP_ATOMIC_O_TRUNC` (1UL << 3) | **Off-deliberately** (comment: strips O_TRUNC to let Setattr handle truncate(0) rather than teaching Open a 2nd truncate path; `dir_cache_fs.cc:251-262`) | Turning off splits `open(O_TRUNC)` into OPEN then SETATTR(0), violating atomic backing semantics (Audit B6). | **Adopt (already decided)**: Turn on to guarantee atomic truncate matching native ext4/xfs/btrfs. | Linux 2.6.14 (Met: $\le 6.9$) |
| **HANDLE_KILLPRIV_V2** | Not found in changelog (protocol 7.33; Linux 5.11) | `FUSE_CAP_HANDLE_KILLPRIV_V2` (1UL << 21) | **Not mentioned in Init** (approved in plan 17.2e; noted in comment `dir_cache_fs.cc:83`) | Without it, kernel clears `SB_NOSEC` and issues per-write `GETXATTR` for `security.capability` (3,200 roundtrips on 3,200 writes, 20x slowdown). Backing kills privs natively. | **Adopt (already adopted)**: Set in `Init()` per plan 17.2e to eliminate per-write GETXATTR round-trips. | Linux 5.11 (Met: $\le 6.9$) |
| **STATX** | Not found in changelog (protocol 7.39; Linux 6.6) | None (`FUSE_STATX = 52` opcode; `fuse_lowlevel_ops::statx`, `fuse_reply_statx`) | **Not implemented** (`ops.statx = nullptr` in `fuse_ops.cc`) | dcfs already fetches `STATX_BASIC_STATS \| STATX_BTIME` from backing and caches `btime` in SQLite; lacking `statx` prevents serving `btime` to clients. | **Adopt**: Implement `ops.statx` to serve cached birth time (`btime`) directly to `statx(2)` callers. | Linux 6.6 (Met: $\le 6.9$) |
| **NO_OPENDIR_SUPPORT** | Not found in changelog (protocol 7.29; Linux 5.1) | `FUSE_CAP_NO_OPENDIR_SUPPORT` (1UL << 24) | **Not mentioned** (off by default in libfuse) | dcfs's `Opendir` only checks `S_ISDIR` and `Releasedir` is a pure no-op (`ReplyErrno(0)`). Zero-message opendir eliminates 2 roundtrips per opened directory. | **Consider**: Benchmarking zero-message opendir can shave two FUSE round-trips for every directory opened or used as `dirfd`. | Linux 5.1 (Met: $\le 6.9$) |
| **READDIRPLUS_AUTO** | Not found in changelog (protocol 7.21; Linux 3.14) | `FUSE_CAP_READDIRPLUS_AUTO` (1UL << 14) | **On by default** (set by libfuse default; not mentioned in `dcfs::Init`) | Makes `READDIRPLUS` adaptive: kernel uses plain `READDIR` if caller does not stat entries (e.g. `ls` vs `ls -l`), conserving entry cache leases. | **Consider**: Explicitly configure in `Init()` to make adaptive behavior an intentional policy decision. | Linux 3.14 (Met: $\le 6.9$) |
| **ATTR_GENERATION** | Not found in changelog (protocol 7.47; Linux 7.1+) | None (`init_out.flags2` bit 44; `fuse_attr_out.generation`) | **Not mentioned** | Returns backing inode generation on GETATTR/SETATTR replies so kernel dcache detects inode reuse. | **Consider (future)**: Excellent fit for dcfs generation-tracking model, but requires kernel > 7.0. | Linux 7.1+ (Exceeds 6.9 and 7.0) |
| **ALLOW_IDMAP** | Not found in changelog (protocol 7.41; Linux 6.11) | None (`init_out.flags2` bit 40) | **Not mentioned** | Permits idmapped mounts (`mount_setattr`) on top of dcfs for user-namespace containers. | **Consider (future)**: Useful if dcfs is exported into user namespaces, but requires Linux 6.11+ and raw flags2 manipulation. | Linux 6.11 (Exceeds 6.9 floor; met on 7.0) |
| **PARALLEL_DIROPS** | Not found in changelog (protocol 7.25; Linux 4.9) | `FUSE_CAP_PARALLEL_DIROPS` (1UL << 18) | **Not mentioned** (off by default; documented in `docs/design.md:1601-1603`) | Drops directory `i_rwsem` in kernel for concurrent lookups/readdirs; dcfs daemon is strictly single-threaded with un-synchronized state. | **Skip (today)**: Concurrency model today relies on kernel directory serialization; reconsider only when async/coroutines land. | Linux 4.9 (Met: $\le 6.9$) |
| **REQUEST_TIMEOUT** | Not found in changelog (protocol 7.43; Linux 6.14) | None (`init_out.flags2` bit 42; `conn.request_timeout` field) | **Not mentioned** (`conn.request_timeout = 0`) | Kernel aborts requests taking longer than timeout; backing disk operations or spin-ups may stall, and aborting risks tri-state protocol corruption. | **Skip**: Conflicts with backing filesystem semantics where operations wait, and exceeds Linux 6.9 floor. | Linux 6.14 (Exceeds 6.9 floor; met on 7.0) |
| **OVER_IO_URING** | Not found in changelog (protocol 7.42; Linux 6.13) | `FUSE_CAP_OVER_IO_URING` (1UL << 31) | **Off-deliberately** (comment: libfuse defaults on, but dcfs is single-threaded with un-synchronized SQLite and maps; `dir_cache_fs.cc:263-272`) | Spawns per-CPU ring worker threads invoking callbacks concurrently, causing fatal data races on dcfs state. | **Skip**: Strictly disabled to protect single-threaded daemon invariants. | Linux 6.13 (Exceeds 6.9 floor; met on 7.0) |
| **SPLICE_READ** | Not found in changelog (protocol 7.14; Linux 2.6.36) | `FUSE_CAP_SPLICE_READ` (1UL << 9) | **Off-deliberately** (comment: SessionLoop read-ahead drain queues messages; spliced messages live in per-thread pipe; `dir_cache_fs.cc:273-276`) | Interferes with SessionLoop interruption-point drain and pipe buffer reuse. Data reads use passthrough anyway. | **Skip**: Deliberately disabled to protect SessionLoop request drainage. | Linux 2.6.36 (Met: $\le 6.9$) |
| **NO_OPEN_SUPPORT** | Not found in changelog (protocol 7.23; Linux 3.16) | `FUSE_CAP_NO_OPEN_SUPPORT` (1UL << 17) | **Not mentioned** (off by default) | Regular file `open` is mandatory in dcfs to register FUSE passthrough (`FOPEN_PASSTHROUGH`), allocate `backing_id`, and track held fills. | **Skip**: Incompatible with FUSE passthrough registration and open-state tracking. | Linux 3.16 (Met: $\le 6.9$) |
| **WRITEBACK_CACHE** | Not found in changelog (protocol 7.23; Linux 3.15) | `FUSE_CAP_WRITEBACK_CACHE` (1UL << 16) | **Not mentioned** (off by default) | Buffers file writes in FUSE page cache; dcfs is a metadata cache delegating file data directly to the backing via passthrough. | **Skip**: Incompatible with FUSE passthrough and immediate write-through metadata guarantees. | Linux 3.15 (Met: $\le 6.9$) |
| **EXPLICIT_INVAL_DATA** | Not found in changelog (protocol 7.30; Linux 5.2) | `FUSE_CAP_EXPLICIT_INVAL_DATA` (1UL << 25) | **Not mentioned** (off by default) | Suppresses automatic page cache invalidation on size/mtime change; dcfs does not manage file page cache. | **Skip**: Adds page cache invalidation bookkeeping with no benefit under passthrough. | Linux 5.2 (Met: $\le 6.9$) |
| **AUTO_INVAL_DATA** | Not found in changelog (protocol 7.20; Linux 3.11) | `FUSE_CAP_AUTO_INVAL_DATA` (1UL << 12) | **On by default** (libfuse default; not mentioned in `Init`) | Ensures fallback non-passthrough reads invalidate page cache when backing mtime changes. Harmless under passthrough. | **Skip changing**: Leave enabled as default safety net for non-passthrough fallback. | Linux 3.11 (Met: $\le 6.9$) |
| **EXPIRE_ONLY** | Not found in changelog (protocol 7.38; Linux 6.2) | `FUSE_CAP_EXPIRE_ONLY` (1UL << 26) | **Not mentioned** (off by default) | Used only with `fuse_lowlevel_notify_inval_entry`; dcfs assumes exclusive access and never emits entry invalidation notifications (`dir_cache_fs.cc:1121-1127`). | **Skip**: dcfs never sends entry invalidations because backing filesystem is assumed exclusive. | Linux 6.2 (Met: $\le 6.9$) |
| **DIRECT_IO_ALLOW_MMAP** | Not found in changelog (protocol 7.39; Linux 6.6) | `FUSE_CAP_DIRECT_IO_ALLOW_MMAP` (1UL << 28) | **Not mentioned** (off by default) | Relaxes shared mmap for `FOPEN_DIRECT_IO`; dcfs uses `FOPEN_PASSTHROUGH` (or buffered fallback), never `FOPEN_DIRECT_IO`. | **Skip**: dcfs does not use direct I/O open mode. | Linux 6.6 (Met: $\le 6.9$) |
| **SETXATTR_EXT** | Not found in changelog (protocol 7.33; Linux 5.11) | `FUSE_CAP_SETXATTR_EXT` (1UL << 27) | **Not mentioned** (noted in test `dir_cache_fs_test.cc:1175`) | Passes `setxattr_flags` (e.g. `KILL_SGID`). libfuse 3.18 low-level API does not expose `setxattr_flags` to `ops.setxattr`, and backing syscall handles SGID natively. | **Skip**: Unsupported by libfuse callback signatures and unnecessary over native backing filesystem. | Linux 5.11 (Met: $\le 6.9$) |
| **FLOCK_LOCKS** | Not found in changelog (protocol 7.17; Linux 3.1) | `FUSE_CAP_FLOCK_LOCKS` (1UL << 10) | **Not mentioned** (off by default) | Offloading BSD locks to userspace requires daemon tracking; leaving off lets kernel VFS manage flock locally with zero round trips. | **Skip**: Local kernel VFS lock management is correct, efficient, and avoids cross-process daemon ownership issues. | Linux 3.1 (Met: $\le 6.9$) |
| **POSIX_LOCKS** | Not found in changelog (protocol 7.7; Linux 2.6.20) | `FUSE_CAP_POSIX_LOCKS` (1UL << 1) | **Not mentioned** (off by default) | Forwarding `fcntl` locks to backing fd makes daemon the owner, breaking mutual exclusion between dcfs clients. Local kernel locking is standard. | **Skip**: Local VFS locking avoids broken daemon-owner semantics and eliminates FUSE lock round trips. | Linux 2.6.20 (Met: $\le 6.9$) |
| **HAS_RESEND** | Not found in changelog (protocol 7.40; Linux 6.9) | None (`init_out.flags2` bit 39) | **Not mentioned** | Kernel resends uncompleted requests on connection reconnect. dcfs does not perform stateful reconnect across `/dev/fuse` sessions. | **Skip**: dcfs does not support live daemon reconnect without remount. | Linux 6.9 (Met: $\le 6.9$) |
| **NO_EXPORT_SUPPORT** | Not found in changelog (protocol 7.40; Linux 6.9) | `FUSE_CAP_NO_EXPORT_SUPPORT` (1UL << 30) | **Not mentioned** (dcfs explicitly enables `FUSE_CAP_EXPORT_SUPPORT`) | Explicitly fails NFS exports with `EOPNOTSUPP`. dcfs specifically supports knfsd export (tested in `nfs_test`). | **Skip**: Directly contradicts dcfs's supported NFS export capability. | Linux 6.9 (Met: $\le 6.9$) |
| **SECURITY_CTX** | Not found in changelog (protocol 7.36; Linux 5.18) | None (`init_out.flags2` bit 32) | **Not mentioned** | Passes LSM security contexts atomically on create. libfuse lacks capability/callback support, and backing syscalls inherit LSM contexts on host. | **Skip**: Unsupported by libfuse lowlevel API; host LSM handles backing inodes directly. | Linux 5.18 (Met: $\le 6.9$) |
| **READDIRPLUS** | Not found in changelog (protocol 7.21; Linux 3.14) | `FUSE_CAP_READDIRPLUS` (1UL << 13) | **On** (`fuse_set_feature_flag(&conn, FUSE_CAP_READDIRPLUS)` in `dir_cache_fs.cc:240`) | Combines readdir and lookup/getattr into a single streaming reply, populating entry leases. | **Already adopted**: Core to directory listing performance. | Linux 3.14 (Met: $\le 6.9$) |
| **CACHE_SYMLINKS** | Not found in changelog (protocol 7.28; Linux 4.20) | `FUSE_CAP_CACHE_SYMLINKS` (1UL << 23) | **On** (`fuse_set_feature_flag(&conn, FUSE_CAP_CACHE_SYMLINKS)` in `dir_cache_fs.cc:241`) | Caches readlink targets in VFS inode cache; symlink targets are immutable. | **Already adopted**: Eliminates repeated readlink round trips. | Linux 4.20 (Met: $\le 6.9$) |
| **PASSTHROUGH** | Not found in changelog (protocol 7.40; Linux 6.9) | `FUSE_CAP_PASSTHROUGH` (1UL << 29) | **On** (`fuse_set_feature_flag(&conn, FUSE_CAP_PASSTHROUGH)` in `dir_cache_fs.cc:247`) | Routes file data read/write directly to backing file descriptors, keeping memory $O(1)$. | **Already adopted**: Foundational architecture of dcfs. | Linux 6.9 (Met: $\le 6.9$) |
| **POSIX_ACL** | Not found in changelog (protocol 7.26; Linux 4.9) | `FUSE_CAP_POSIX_ACL` (1UL << 19) | **On (mandatory)** (`fuse_set_feature_flag(&conn, FUSE_CAP_POSIX_ACL)` in `dir_cache_fs.cc:317`) | Enforces POSIX ACL access checks in kernel rather than stripping permissions to mode bits. | **Already adopted**: Mandatory requirement for matching backing filesystem permission checks. | Linux 4.9 (Met: $\le 6.9$) |
| **DONT_MASK** | Not found in changelog (protocol 7.12; Linux 2.6.31) | `FUSE_CAP_DONT_MASK` (1UL << 6) | **On (mandatory)** (`fuse_set_feature_flag(&conn, FUSE_CAP_DONT_MASK)` in `dir_cache_fs.cc:318`) | Leaves file mode unmasked by umask when parent directory has a default ACL. | **Already adopted**: Mandatory companion to POSIX_ACL. | Linux 2.6.31 (Met: $\le 6.9$) |
| **IOCTL_DIR** | Not found in changelog (protocol 7.18; Linux 3.5) | `FUSE_CAP_IOCTL_DIR` (1UL << 11) | **On** (`fuse_set_feature_flag(&conn, FUSE_CAP_IOCTL_DIR)` in `dir_cache_fs.cc:250`) | Allows `FS_IOC_GETFLAGS` and `FS_IOC_SETFLAGS` (`lsattr`/`chattr`) on directories. | **Already adopted**: Matches backing filesystem directory ioctl support. | Linux 3.5 (Met: $\le 6.9$) |
| **EXPORT_SUPPORT** | Not found in changelog (base 7.1; Linux 2.6.14) | `FUSE_CAP_EXPORT_SUPPORT` (1UL << 4) | **On** (`fuse_set_feature_flag(&conn, FUSE_CAP_EXPORT_SUPPORT)` in `dir_cache_fs.cc:239`) | Enables lookups of `.` and `..` for knfsd file handle reconnection. | **Already adopted**: Required for NFS export. | Linux 2.6.14 (Met: $\le 6.9$) |
| **MAX_PAGES** | Not found in changelog (protocol 7.28; Linux 4.20) | None (handled automatically by libfuse via `conn.max_write`) | **Handled by libfuse** (`max_read` set in `dir_cache_fs.cc:234`) | Dictates maximum pages per I/O request in fallback mode; libfuse calculates `max_pages` automatically. | **Skip**: No capability bit exists; libfuse handles it internally from `max_write`. | Linux 4.20 (Met: $\le 6.9$) |
| **ASYNC_READ** | Not found in changelog (base 7.1; Linux 2.6.14) | `FUSE_CAP_ASYNC_READ` (1UL << 0) | **On by default** (libfuse default) | Allows concurrent read request submission. Under passthrough, reads bypass daemon. | **Skip changing**: Leave enabled as libfuse default. | Linux 2.6.14 (Met: $\le 6.9$) |
| **ASYNC_DIO** | Not found in changelog (protocol 7.22; Linux 3.15) | `FUSE_CAP_ASYNC_DIO` (1UL << 15) | **On by default** (libfuse default) | Asynchronous submission of direct I/O requests. Passthrough handles data I/O. | **Skip changing**: Leave enabled as libfuse default. | Linux 3.15 (Met: $\le 6.9$) |
| **MAP_ALIGNMENT** | Not found in changelog (protocol 7.31; Linux 5.3) | None (`init_out.map_alignment`) | **Not mentioned** | Specifies log2 byte alignment for DAX memory windows. dcfs does not use DAX. | **Skip**: Virtiofs DAX specific. | Linux 5.3 (Met: $\le 6.9$) |
| **SUBMOUNTS** | Not found in changelog (protocol 7.32; Linux 5.4) | None (`init_in/out` bit 27) | **Not mentioned** | Auto-mounting submounts across virtiofs boundaries. dcfs tracks submounts via `mounts_below.cc`. | **Skip**: Virtiofs container specific. | Linux 5.4 (Met: $\le 6.9$) |
| **HAS_INODE_DAX** | Not found in changelog (protocol 7.36; Linux 5.18) | None (`init_out.flags2` bit 33) | **Not mentioned** | Per-inode DAX direct access for persistent memory windows. | **Skip**: DAX specific. | Linux 5.18 (Met: $\le 6.9$) |
| **CREATE_SUPP_GROUP** | Not found in changelog (protocol 7.38; Linux 6.2) | None (`init_out.flags2` bit 34) | **Not mentioned** | Passes parent group in supplementary groups. Backing syscalls run as caller with native SGID handling. | **Skip**: Redundant with native host syscall execution. | Linux 6.2 (Met: $\le 6.9$) |
| **HAS_EXPIRE_ONLY** | Not found in changelog (protocol 7.38; Linux 6.2) | None (`init_out.flags2` bit 35) | **Not mentioned** | Capability probe for `EXPIRE_ONLY` entry invalidations. | **Skip**: Same as `EXPIRE_ONLY`. | Linux 6.2 (Met: $\le 6.9$) |
| **HAS_IO_URING_BUFPOOL**| Not found in changelog (protocol 7.46; Linux 6.17) | None (`init_out.flags2` bit 43) | **Not mentioned** | Kernel io_uring buffer pool management. io_uring is deliberately disabled in dcfs. | **Skip**: io_uring is disabled. | Linux 6.17 (Exceeds 6.9 floor; met on 7.0) |

---

## 2. Summary of Recommendations for New Adoption

### Priority 1: High Value / Immediate Adoption
1. **`STATX` (FUSE opcode 52, Linux 6.6 / Protocol 7.39)**:
   - **Recommendation**: **Adopt**.
   - **Why**: dcfs already issues `statx` to the backing store and caches `STATX_BTIME` (`btime_sec`, `btime_nsec`) in SQLite. Without implementing `fuse_lowlevel_ops::statx`, userspace callers of `statx(2)` asking for `STATX_BTIME` fail to receive birth times from dcfs. Implementing `ops.statx` satisfies "Behave like the backing" with zero additional backing calls.
2. **`ATOMIC_O_TRUNC` (`FUSE_CAP_ATOMIC_O_TRUNC`, Linux 2.6.14 / Protocol 7.1)**:
   - **Recommendation**: **Adopt (already decided)**.
   - **Why**: Eliminates the two-step `OPEN` + `SETATTR(size=0)` sequence that violates atomic truncation semantics on ext4/xfs/btrfs.
3. **`HANDLE_KILLPRIV_V2` (`FUSE_CAP_HANDLE_KILLPRIV_V2`, Linux 5.11 / Protocol 7.33)**:
   - **Recommendation**: **Adopt (already decided in Plan 17.2e)**.
   - **Why**: Prevents kernel from clearing `SB_NOSEC` and issuing 3,200 synchronous `GETXATTR` round-trips for 3,200 writes.

### Priority 2: Candidates to Benchmark & Consider
4. **`NO_OPENDIR_SUPPORT` (`FUSE_CAP_NO_OPENDIR_SUPPORT`, Linux 5.1 / Protocol 7.29)**:
   - **Recommendation**: **Consider**.
   - **Why**: dcfs's `Opendir` only checks `S_ISDIR` and `Releasedir` is a no-op returning 0. Enabling `NO_OPENDIR_SUPPORT` removes 2 FUSE roundtrips for every directory opened or used as a `dirfd`.
5. **`READDIRPLUS_AUTO` (`FUSE_CAP_READDIRPLUS_AUTO`, Linux 3.14 / Protocol 7.21)**:
   - **Recommendation**: **Consider**.
   - **Why**: Currently enabled implicitly via libfuse default. Making it explicit in `Init()` codifies dcfs's directory caching policy.

### Priority 3: Candidates for Future Architectural Phases
6. **`ATTR_GENERATION` (Flags2 bit 44, Linux 7.1+ / Protocol 7.47)**:
   - **Recommendation**: **Consider in future**.
   - **Why**: Carries inode generation numbers in GETATTR/SETATTR replies, matching dcfs's generation tracking to catch stale dcache dentries. Needs kernel > 7.0.
7. **`ALLOW_IDMAP` (Flags2 bit 40, Linux 6.11 / Protocol 7.41)**:
   - **Recommendation**: **Consider in future**.
   - **Why**: Needed only if dcfs mounts are used with container user namespaces and idmapped mounts. Requires kernel > 6.9 floor.
