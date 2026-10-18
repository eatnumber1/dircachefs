Draft by Gemini 4 Argon (high), 2026-10-18; orchestrator review notes in the plan step (25.41 in `phases/25-style-convergence.md`).

### Draft Plan Step (for `docs/plan/phases/25-style-convergence.md`)

```markdown
### 25.34b Replace link-time `-Wl,--wrap` fakes with dependency injection

**Relation to prior decision ([`docs/design.md:3292-3312`]).** On 2026-10-16 `docs/design.md` recorded keeping `-Wl,--wrap` after reverting `attic/virtual-syscalls` because that branch (1) collapsed [`//dcfs:syscalls`], [`//dcfs:syscalls_backing`], and [`//dcfs:syscalls_process`] into one library, weakening [`//tools:syscalls_backing_users_test`] ([`dcfs/syscalls_backing_users.txt:1-64`]); (2) left two ways to call a syscall (`RealSyscalls::Default()` and free functions); and (3) used virtual-inheritance mixins (`public virtual Syscalls`). This step executes the deliberate design sketched at [`docs/design.md:3307-3312`] ahead of steps 25.35–25.38 ([`docs/plan/phases/25-style-convergence.md:1363-1437`]) so strong types and span buffers land once on the final interfaces.

**Why.** Link-time wrapping relies on glibc symbol names (`openat64`, `fstatfs64` at [`dcfs/BUILD.bazel:1126-1134`]), fragile IA-64 C++ mangled names ([`dcfs/BUILD.bazel:1100`], [`dcfs/BUILD.bazel:1341-1348`], [`dcfs/BUILD.bazel:1429-1432`]), inlining avoidance across translation units ([`dcfs/BUILD.bazel:1439-1440`]), global mutable state in tests ([`dcfs/dir_cache_fs_test.cc:134-203`]), and reserved-identifier exceptions ([`.clang-tidy:35-37`], [`tools/raw_syscalls_test.py:15-17`]).

**Design options evaluated.**
1. **Three narrow virtual interfaces mirroring the three Bazel libraries, injected by reference (Recommended).**
   - `BackingSyscalls` in [`//dcfs:syscalls_backing`], `LocalSyscalls` in [`//dcfs:syscalls`], and `ProcessSyscalls` in [`//dcfs:syscalls_process`]. Each library provides its `Real*Syscalls` implementation; `testonly/` provides `Delegating*Syscalls` wrappers holding `inner_` by reference (single inheritance, no virtual inheritance).
   - *Rules check:* No test-only knobs in production; the injected object is the real operating-system boundary of [`Context`] and standalone tools. No `CHECK`/abort in production. Preserves [`dcfs/syscalls_backing_users.txt:1-64`] and [`tools/banned_symbols.txt:1-147`].
   - *Hot-path cost:* Cache hits do zero backing syscalls (`SlopeTest`, [`dcfs/dir_cache_fs_test.cc:8460-8503`]). On cache misses and mutations, one indirect call (~1–2 ns) precedes a kernel transition and filesystem operation (~300–5,000+ ns) — <0.5% overhead, verified by [`//bench:dcfs_bench`].
   - *Nth-call faults:* A `CountdownFault` helper in `testonly/` decrements a counter on matching calls and returns `ErrnoToStatus(err, op)` when `++calls == fail_at` (for [`dcfs/dir_cache_fs_test.cc:288-306`], [`dcfs/dir_cache_fs_test.cc:486-495`]) or while `times-- > 0` (for [`dcfs/fsck_test.cc:52-67`]).
2. **Template parameter (`template <typename Syscalls = RealSyscalls>`) on callers.**
   - *Rejected:* Zero dispatch cost, but [`dcfs/backing.cc`] (2,555 lines), [`dcfs/dir_cache_fs.cc`] (4,072 lines), [`dcfs/file_handle.cc`], [`dcfs/device_id.cc`], and [`dcfs/fsck.cc`] would either move into headers or require explicit instantiations of production `.cc` files for every test fake, breaking `.o`-level symbol enforcement in [`tools/banned_symbols.txt:117`] and bloating compile times. And [`FaultSitesTest`] still needs runtime dispatch across hooks. (Compile-time selection *is* used in step 25.38 for typed `ioctl`/`fcntl` tags that dispatch to non-template virtual methods on `BackingSyscalls`.)
3. **Per-thread or per-instance fault table inside `syscalls::`.**
   - *Rejected:* Violates [`docs/style.md:505-511`] (step 25.7: no test-only knobs in production code) and adds a TLS/table branch to every production syscall solely for tests.

**Architecture of the seams.**
- **Syscall interfaces:** [`Context`] carries `BackingSyscalls &backing_syscalls` and `LocalSyscalls &syscalls` (constructed in `main` at [`dcfs/main.cc:322-375`] and in test harnesses; no global `Default()` accessor). Low-level helpers in [`dcfs/backing.cc`], [`dcfs/file_handle.h:35-66`], and [`dcfs/device_id.h:33`] take `BackingSyscalls &` (or `Context &`). [`dcfs/fsck.h:37-49`] and [`dcfs/startup_channel.h:58`] take a small `FsckDeps` / `DaemonDeps` struct holding references to `BackingSyscalls`, `LocalSyscalls`, and `ProcessSyscalls`. [`FileDescriptor`] keeps calling [`syscalls::close`] directly (never wrapped; avoids doubling `sizeof(FileDescriptor)`). The template [`syscalls::ioctl`] becomes a typed front-end (anticipating 25.38) dispatching to concrete virtual methods on `BackingSyscalls` (`IoctlGetVersion`, `IoctlGetFsUuid`, `IoctlBtrfsFsInfo`, `IoctlBtrfsSubvolInfo`, `IoctlFlags`, `IoctlRaw`).
- **Call sites (russ, 2026-10-18):** "for the change to use a dependency-injected syscalls wrapper, keep the global free functions (so call sites look like syscalls::write(...)), but pass in the Context or Syscalls object. Then the wrapper can call syscalls.Write(...) (and note I'm removing the function naming exception for the member methods of Syscalls, since those functions will now only ever be called by the free functions in syscalls::." Then (russ, 2026-10-18): "Actually, let's have the syscalls:: free functions take Context. It can pull Syscalls from inside it, and it provides a uniform surface for the API (syscalls functions look and feel mostly like libc, with an additional Context argument)." Every `syscalls::` free function takes the `Context` as its first argument (`syscalls::write(ctx, fd, buf, n)`), pulls the right Syscalls interface out of it, and calls its CamelCase member (`Write`). The members are called only by the free functions. How that is enforced (members private with the free functions as friends, or public with a `repo_shape`/clang-query rule refusing member calls outside `syscalls*.cc`) is an open question for the 25.41 implementer.
- **Libfuse seam:** Extract a `FuseLowlevelOps` interface on [`FuseSession`] / [`FuseRequest`] for `fuse_passthrough_open`, `fuse_passthrough_close` ([`dcfs/dir_cache_fs.cc:158-221`]), and `fuse_reply_none` ([`dcfs/fuse_request.cc:136`]), replacing those 3 wraps in [`dcfs/fuse_request_test.cc:159`] and [`dcfs/dir_cache_fs_test.cc:320-335`].
- **Trace-validation & recovery fault tests:** The 6 negative tests in [`DIR_CACHE_FS_FAULTS`] (`skip_mark_unknown`, `syscall_before_phase1`, `phase3_before_syscall`, `snapshot_after_syncfs`, `create_syscall_before_phase1`, `swallow_syscall_error`) test [`//formal:trace_validate`], not `dcfs`, by corrupting `dcfs` at link time. Replace `syscall_before_phase1`, `snapshot_after_syncfs`, `create_syscall_before_phase1`, and `swallow_syscall_error` with injected `BackingSyscalls` fakes that emit/alter trace events or run the backing syscall inside the fake before phase 1 commits, and replace `skip_mark_unknown` and `phase3_before_syscall` by either a `sqlite3_authorizer` on the test's [`sqlite::Database`] (`SQLITE_IGNORE` on the `unknown` write) or direct negative trace tests on `trace_validate`. In [`dir_cache_fs_crash_during_recovery_test`], replace wrapping mangled `cache::RecoverDirty` and `backing::BackingNlink` ([`dcfs/testonly/recover_dirty_fails_once.cc:25`], [`dcfs/testonly/probe_fails_once.cc:26`]) by injecting a `BackingSyscalls` fake that fails `statx` / `open_by_handle_at` on the first recovery pass and on the root-nlink probe.
- **`tools/waitexit.c`:** Split `waitexit_run(pid_t pid, const struct waitexit_sys *sys)` in [`tools/waitexit.c:18-62`] with function pointers `.pidfd_open` and `.poll` so [`test/qemu/waitexit_test.cc:56-96`] injects fakes without `-Wl,--wrap`.

**Migration order (small mergeable commits, each tested).**
1. **Define `LocalSyscalls`, `ProcessSyscalls`, and `BackingSyscalls` + `Delegating*Syscalls` fakes.** Keep [`tools/style_matchers/backing_door_guard.query:1-9`] enforcing `BackingDoor()` in `RealBackingSyscalls`. *Test:* unit tests of `Real*Syscalls` and `Delegating*Syscalls` + [`//tools:syscalls_backing_users_test`].
2. **Migrate [`//test/qemu:waitexit_test`] and [`//dcfs:startup_channel_fault_test`].** Inject `waitexit_sys` in `waitexit.c` and `LocalSyscalls &` in [`ForkDaemon`]; drop `--wrap=pidfd_open`, `--wrap=poll`, `--wrap=setsid`. *Test:* `bazel test //test/qemu:waitexit_test //dcfs:startup_channel_fault_test`.
3. **Migrate [`//dcfs:device_id_fault_test`] and [`//dcfs:backing_fault_test`].** Thread `BackingSyscalls &` through [`GetDeviceId`], [`Context`], [`file_handle.cc`], and [`backing.cc`]; drop `--wrap=ioctl`, `--wrap=fstatfs64`, `--wrap=getxattr`, `--wrap=listxattr`, `--wrap=readlinkat`, and the [`tools/style_checks_allow.txt:66`] exception. *Test:* `bazel test //dcfs:device_id_fault_test //dcfs:backing_fault_test`.
4. **Migrate [`//dcfs:fsck_test`].** Thread `FsckDeps` through [`dcfs/fsck.cc:49-393`]; drop `--wrap={fork,waitpid,dup2,flock,unlinkat}`. *Test:* `bazel test //dcfs:fsck_test`.
5. **Migrate [`//dcfs:fuse_request_test`] and libfuse passthrough in `dir_cache_fs`.** Inject `FuseLowlevelOps`; drop `--wrap=fuse_reply_none` and `--wrap=fuse_passthrough_{open,close}`. *Test:* `bazel test //dcfs:fuse_request_test //dcfs:dir_cache_fs_test`.
6. **Migrate [`DIR_CACHE_FS_TEST_WRAPS`].** Move `NoTransactionAt`, `DCFS_INJECT`, `WrappedCalls`, `FakeInodeNumbers`, hooks, and `SetgroupsFault` ([`dcfs/dir_cache_fs_test.cc:134-516`]) into a `HarnessSyscalls` delegating wrapper on the test harness's `Context`; delete `DIR_CACHE_FS_TEST_WRAPS`. *Test:* `bazel test //dcfs:dir_cache_fs_test //dcfs:dir_cache_fs_poisoned_test //dcfs:dir_cache_fs_slope_test //dcfs:dir_cache_fs_fault_sites_test`.
7. **Migrate [`DIR_CACHE_FS_FAULTS`] and [`dir_cache_fs_crash_during_recovery_test`].** Replace the 8 `dcfs/testonly/` `--wrap` libraries ([`dcfs/testonly/BUILD.bazel:184-237`], [`dcfs/testonly/BUILD.bazel:310-334`]) with injected fakes/authorizer. *Test:* `bazel test //dcfs:dir_cache_fs_fault_... //dcfs:dir_cache_fs_crash_during_recovery_test`.
8. **Delete `--wrap` allow-lists, ban `-Wl,--wrap`, and update docs.** Ban calls to the interface members outside `syscalls*.cc` (the `syscalls::` free functions, which take `Context`, are their only callers; the mechanism is the open question above); remove `__wrap_`/`__real_` exceptions from [`.clang-tidy:35-37,68-70`] and [`tools/raw_syscalls_test.py:15-17,141`]; add a check in [`tools/style_checks_test.py`] banning `--wrap` in `BUILD.bazel`/`*.bzl`; update [`AGENTS.md:22-23`], [`docs/style.md:474-475,516,532,1491-1493,1559,1757-1758`], and [`docs/design.md:3292-3312`]. *Test:* `bazel test --config=presubmit //...`.

**Risks.**
- *Bypassing the injected interface:* A caller invoking an interface member directly would silently skip `NoTransactionAt` and `FaultSitesTest`. Mitigation: the members are reachable only through the `syscalls::` free functions, enforced as the 25.41 design decides in sub-step 8 (`FileDescriptor::Close`, which calls `syscalls::close` without an interface argument, needs the same treatment; open question).
- *Variadic `openat`/`ioctl`/`fcntl`:* Virtual methods cannot be variadic or templates. Mitigation: split `openat(dirfd, path, flags)` and `openat(dirfd, path, flags, mode)` and use the `ioctl<OP>`/`fcntl<OP>` free function templates over per-op members, aligning directly with step 25.38 ([`docs/plan/phases/25-style-convergence.md:1405-1437`]).
- *Coverage Baseline:* Every `Real*Syscalls` method and error branch must stay covered so total coverage never drops ([`AGENTS.md:24-29`]).
```

---

## Appendix A: Complete Inventory of `-Wl,--wrap` Uses

A search across all `BUILD.bazel` and `*.bzl` files finds **48 `-Wl,--wrap` flags** across **9 target groups** in [`dcfs/BUILD.bazel`] and [`test/qemu/BUILD.bazel`] (zero in `*.bzl`; [`third_party/google_styleguide/BUILD.bazel:34`] has `pandoc --wrap=none`, which is markdown line-wrapping).

### A.1 [`//dcfs:backing_fault_test`] ([`dcfs/backing_fault_test.cc`])

| Wrapped Symbol | `BUILD.bazel` Line | `__wrap_*` Definition | Test(s) & Injected Fault (`file:line`) |
| :--- | :--- | :--- | :--- |
| `getxattr` | [`dcfs/BUILD.bazel:386`] | [`dcfs/backing_fault_test.cc:84-104`] | `XattrValueRetriesOnErangeFromReadNotQuery` ([`dcfs/backing_fault_test.cc:182-196`]): calls `__real_getxattr` ([`line 87`]); on the first size-query call (`value == nullptr && size == 0`) matching `FaultState::getxattr_path` and `getxattr_name` ([`lines 46-53, 91-92`]), calls `syscalls::setxattr` ([`lines 94-100`]) to grow the xattr value so the subsequent read fails with real kernel `ERANGE`. |
| `listxattr` | [`dcfs/BUILD.bazel:387`] | [`dcfs/backing_fault_test.cc:106-123`] | `XattrListRetriesOnErangeFromReadNotQuery` ([`dcfs/backing_fault_test.cc:200-213`]): calls `__real_listxattr` ([`line 108`]); on the first size-query call (`list == nullptr && size == 0`) matching `FaultState::listxattr_path` ([`lines 55-59, 109-110`]), calls `syscalls::setxattr` ([`lines 114-118`]) to add an xattr so the subsequent read fails with `ERANGE`. |
| `readlinkat` | [`dcfs/BUILD.bazel:388`] | [`dcfs/backing_fault_test.cc:128-139`] | `SymlinkTargetIsEnametoolongAtPathMaxTimesFourCap` ([`dcfs/backing_fault_test.cc:221-231`]): when `(dirfd, pathname)` matches `(fake_readlinkat_dirfd, fake_readlinkat_path)` ([`lines 66-67, 131-132`]), fills `buf` with `'a'` and returns `bufsize` ([`lines 135-136`]), simulating a symlink target exceeding `PATH_MAX * 4` so `ReadSymlinkFd` returns `ENAMETOOLONG`. |

### A.2 [`//dcfs:fuse_request_test`] ([`dcfs/fuse_request_test.cc`])

| Wrapped Symbol | `BUILD.bazel` Line | `__wrap_*` Definition | Test(s) & Injected Fault (`file:line`) |
| :--- | :--- | :--- | :--- |
| `fuse_reply_none` | [`dcfs/BUILD.bazel:433`] | [`dcfs/fuse_request_test.cc:159`] | `ReplyNoneTwiceRepliesOnceAndLogsOnce` ([`dcfs/fuse_request_test.cc:143-154`]): increments `dcfs::RepliesNone()` ([`lines 123-126`]) without calling libfuse (test passes fake `reinterpret_cast<fuse_req_t>(0x1)` at [`line 147`]). |

### A.3 [`//dcfs:device_id_fault_test`] ([`dcfs/device_id_fault_test.cc`])

| Wrapped Symbol | `BUILD.bazel` Line | `__wrap_*` Definition | Test(s) & Injected Fault (`file:line`) |
| :--- | :--- | :--- | :--- |
| `ioctl` | [`dcfs/BUILD.bazel:565`] | [`dcfs/device_id_fault_test.cc:82-111`] | Driven by `Faults` ([`lines 46-56`]):<br>• `NonFallbackIoctlErrorFails` ([` lines 131-136`]): fails `FS_IOC_GETFSUUID` with `EIO` ([`lines 89-93`]).<br>• `FallbackToFstatfsFsidWhenIoctlUnsupported` ([`lines 140-149`]): fails `FS_IOC_GETFSUUID` with `ENOTTY`, `EOPNOTSUPP`, `ENOSYS`.<br>• `ZeroFsidOnAllowlistedFsFails` ([`lines 153-158`]): fails `FS_IOC_GETFSUUID` with `ENOTTY`.<br>• `UnexpectedUuidLengthFails` ([`lines 161-166`]) & `ValidGetfsuuidSucceeds` ([`lines 168-172`]): returns fake `fsuuid2` with `len = 17` or `16` ([`lines 94-99`]).<br>• `BtrfsSubvolIncludesTreeId` ([`lines 175-182`]): populates fake `BTRFS_IOC_FS_INFO` and `BTRFS_IOC_GET_SUBVOL_INFO` ([`lines 100-109`]).<br>• `BtrfsFsInfoErrorFails` ([`lines 184-191`]) & `BtrfsSubvolInfoErrorFails` ([`lines 194-202`]): fails btrfs ioctls with `EPERM` / `EACCES`. |
| `fstatfs64` | [`dcfs/BUILD.bazel:566`] | [`dcfs/device_id_fault_test.cc:75-80`] | Calls `__real_fstatfs64` and overwrites `buf->f_type` when `Faults::fstatfs_type` is set ([`lines 48, 77-78`]) to simulate `PROC_SUPER_MAGIC` ([`line 141`]), `NFS_SUPER_MAGIC` ([`line 154`]), or `BTRFS_SUPER_MAGIC` ([`lines 176, 185, 195`]). |

### A.4 [`//dcfs:fsck_test`] ([`dcfs/fsck_test.cc`])

All 5 wraps use `ShouldFail(Fault&)` ([`dcfs/fsck_test.cc:52-67`]), which decrements `fault.times` while `> 0` and sets `errno = fault.error`.

| Wrapped Symbol | `BUILD.bazel` Line | `__wrap_*` Definition | Test(s) & Injected Fault (`file:line`) |
| :--- | :--- | :--- | :--- |
| `fork` | [`dcfs/BUILD.bazel:791`] | [`dcfs/fsck_test.cc:72`] | `NoProcessForFindmntIsOperational` ([`dcfs/fsck_test.cc:618-621`]): fails `fork` once with `EAGAIN`. |
| `waitpid` | [`dcfs/BUILD.bazel:792`] | [`dcfs/fsck_test.cc:73-75`] | `AWaitThatFailsIsOperational` ([`dcfs/fsck_test.cc:630-634`]): fails `waitpid` once with `ECHILD`.<br>`AWaitThatASignalInterruptedIsRetried` ([`dcfs/fsck_test.cc:637-644`]): fails `waitpid` twice with `EINTR` before succeeding. |
| `dup2` | [`dcfs/BUILD.bazel:793`] | [`dcfs/fsck_test.cc:76-78`] | `AChildWhoseOutputCannotBeRedirectedDoesNotRun` ([`dcfs/fsck_test.cc:624-628`]): fails `dup2` in the forked child with `EBADF` so child exits with 127. |
| `flock` | [`dcfs/BUILD.bazel:794`] | [`dcfs/fsck_test.cc:79-81`] | `ALockThatCannotBeTakenIsReported` ([`dcfs/fsck_test.cc:589-602`]): fails `flock` once with `ENOLCK`. |
| `unlinkat` | [`dcfs/BUILD.bazel:795`] | [`dcfs/fsck_test.cc:82-84`] | `ACorruptCacheThatCannotBeDeletedIsReported` ([`dcfs/fsck_test.cc:605-616`]): fails `unlinkat` once with `EBUSY` when removing a corrupt DB. |

### A.5 [`//dcfs:startup_channel_fault_test`] ([`dcfs/startup_channel_fault_test.cc`])

| Wrapped Symbol | `BUILD.bazel` Line | `__wrap_*` Definition | Test(s) & Injected Fault (`file:line`) |
| :--- | :--- | :--- | :--- |
| `setsid` | [`dcfs/BUILD.bazel:885`] | [`dcfs/startup_channel_fault_test.cc:138-144`] | `ADaemonThatCannotDetachReportsWhy` ([`dcfs/startup_channel_fault_test.cc:126-133`]): when `g_setsid_fails` ([`line 44`]) is true, fails `setsid()` in the daemon child with `EPERM`. |

### A.6 `DIR_CACHE_FS_TEST_WRAPS` ([`dcfs/BUILD.bazel:1100-1143`], [`dcfs/dir_cache_fs_test.cc:318-516`])

Linked into [`//dcfs:dir_cache_fs_test`] (and via `:dir_cache_fs_test_bin` into [`dir_cache_fs_trace_{a,b,c}_test`]), [`//dcfs:dir_cache_fs_poisoned_test`], [`//dcfs:dir_cache_fs_slope_test`], [`//dcfs:dir_cache_fs_fault_sites_test`], the 6 [`dir_cache_fs_fault_*_test`] targets, and [`//dcfs:dir_cache_fs_crash_during_recovery_test`].

| Wrapped Symbol(s) | `BUILD.bazel` Line | `__wrap_*` Definition | Test(s) & Injected Fault (`file:line`) |
| :--- | :--- | :--- | :--- |
| `fuse_passthrough_open` | [`dcfs/BUILD.bazel:1108`] | [`dcfs/dir_cache_fs_test.cc:320-324`] | When `Passthrough().enabled` ([`lines 176-189`]), returns `fake.next_id++` instead of calling libfuse (used at [`lines 7606-7682`]). |
| `fuse_passthrough_close` | [`dcfs/BUILD.bazel:1109`] | [`dcfs/dir_cache_fs_test.cc:326-335`] | Records `backing_id` in `Passthrough().closed`; if `Passthrough().close_fails` ([`line 7671`]), fails with `EBADF`. |
| `open_by_handle_at` | [`dcfs/BUILD.bazel:1104`] | [`dcfs/dir_cache_fs_test.cc:338-353`] | Asserts `NoTransactionAt("open_by_handle_at")` ([`lines 221-238`]); runs `DCFS_INJECT("open_by_handle_at", -1)` ([`lines 288-316`]); runs one-shot `OpenByHandleHook()` ([`lines 140-143, 342-343`]); pops front of `OpenByHandleFailures()` ([`lines 134-137, 344-351`]) to fail with `ESTALE`/`EIO`/`ENOENT`. |
| `name_to_handle_at` | [`dcfs/BUILD.bazel:1103`] | [`dcfs/dir_cache_fs_test.cc:357-369`] | `NoTransactionAt`; `DCFS_INJECT`; runs one-shot `NameToHandleHook()` ([`lines 146-149, 362-363`]); checks one-shot `NameToHandleFailure()` ([`lines 158-161, 364-367`]). |
| `statx` | [`dcfs/BUILD.bazel:1105`] | [`dcfs/dir_cache_fs_test.cc:372-390`] | `NoTransactionAt`; runs one-shot `StatxHook()` ([`lines 171-174, 375-378`]); `DCFS_INJECT`; checks one-shot `StatxFailure()` ([`lines 200-203, 380-383`]); remaps `buf->stx_ino` via `FakeInodeNumbers()` ([`lines 194-197, 385-388`]). |
| `syncfs` | [`dcfs/BUILD.bazel:1106`] | [`dcfs/dir_cache_fs_test.cc:392-398`] | `NoTransactionAt`; `DCFS_INJECT`; runs one-shot `SyncfsHook()` ([`lines 152-155, 395-396`]). |
| `fstatvfs64` | [`dcfs/BUILD.bazel:1127`] | [`dcfs/dir_cache_fs_test.cc:448-454`] | `NoTransactionAt`; `DCFS_INJECT`; sets `b->f_flag |= ST_RDONLY` when `StatvfsReadOnly()` is true ([`lines 165-168, 452`]). |
| `openat64` | [`dcfs/BUILD.bazel:1134`] | [`dcfs/dir_cache_fs_test.cc:464-475`] | `NoTransactionAt("openat")`; `DCFS_INJECT("openat", -1)`; forwards variadic `mode` to `__real_openat64`. |
| 23 `DCFS_BACKSTOP` wraps: `copy_file_range`, `fallocate64`, `fchmodat`, `fchownat`, `fgetxattr`, `flistxattr`, `fremovexattr`, `fsetxattr`, `fstatat64`, `fstatfs64`, `futimens`, `getxattr`, `linkat`, `listxattr`, `mkdirat`, `mknodat`, `readlinkat`, `removexattr`, `renameat2`, `setxattr`, `symlinkat`, `unlinkat`, `utimensat` | [`dcfs/BUILD.bazel:1115-1142`] | [`dcfs/dir_cache_fs_test.cc:402-460`] | Each runs `NoTransactionAt(#name)` ([`lines 221-238`]: aborts if SQLite has an open transaction or active statement during a backing syscall) and `DCFS_INJECT(#name, -1)` ([`lines 282-316`]: increments `WrappedCalls()` for `SlopeTest` at [`lines 8460-8503`] and injects per-site `EIO`/custom errnos in `FaultSitesTest` at [`lines 8314-8371`]). |
| `_ZN4dcfs8syscalls9setgroupsENSt3__14spanIKjLm18446744073709551615EEE` (`dcfs::syscalls::setgroups`) | [`dcfs/BUILD.bazel:1100, 1111`] | [`dcfs/dir_cache_fs_test.cc:501-514`] | `SetgroupsFault` ([`lines 486-495`]): fails when `++fault.calls == fault.fail_at`. Used at [`line 7021`] (`fail_at = 1, err = EPERM` to fail `AsCaller::SwitchTo`) and in `PoisonedBackingTest` at [`line 7064`] (`fail_at = 2, err = ENOMEM` to fail `RestoreRoot` and poison the filesystem). |

### A.7 `DIR_CACHE_FS_FAULTS` ([`dcfs/BUILD.bazel:1341-1420`], [`dcfs/testonly/BUILD.bazel:186-237`])

Each builds a variant of `:dir_cache_fs_test_bin` and asserts in [`dcfs/testonly/expect_trace_failure.sh`] that `trace_validate` rejects the trace.

| Fault Target | Wrapped Symbol(s) & `BUILD.bazel` Line | `__wrap_*` Definition & Injected Fault (`file:line`) |
| :--- | :--- | :--- |
| `skip_mark_unknown` | `sqlite3_step` ([`dcfs/BUILD.bazel:1352`]) | [`dcfs/testonly/skip_mark_unknown.cc:16-26`]: inspects `sqlite3_expanded_sql(stmt)` and returns `SQLITE_DONE` without stepping when SQL starts with `INSERT INTO dentries` and contains `'unknown'`. |
| `syscall_before_phase1` | `_ZN4dcfs5cache11BeginRemoveERNS_7ContextElNSt3__117basic_string_viewIcNS3_11char_traitsIcEEEE` (`cache::BeginRemove`, [`dcfs/BUILD.bazel:1341, 1360`]), `_ZN4dcfs7backing8UnlinkAtERNS_7ContextEiiNSt3__117basic_string_viewIcNS3_11char_traitsIcEEEEi` (`backing::UnlinkAt`, [`dcfs/BUILD.bazel:1347, 1361`]) | [`dcfs/testonly/fault_syscall_before_phase1.cc:27-65`]: on the first `BeginRemove`, opens parent dirfd and calls real `backing::UnlinkAt` *before* calling real `BeginRemove`, then makes the subsequent `backing::UnlinkAt` return `absl::OkStatus()`. |
| `phase3_before_syscall` | `cache::BeginRemove` ([`dcfs/BUILD.bazel:1341, 1367`]) | [`dcfs/testonly/fault_phase3_before_syscall.cc:24-49`]: on the first `BeginRemove`, calls real `BeginRemove` and immediately calls `cache::CompleteRemove` before the backing syscall runs. |
| `snapshot_after_syncfs` | `_ZN4dcfs5cache9BeginSyncERNS_7ContextE` (`cache::BeginSync`, [`dcfs/BUILD.bazel:1343, 1373`]) | [`dcfs/testonly/snapshot_after_syncfs.cc:21-37`]: on the first `BeginSync`, calls `backing::SyncFs(ctx)` *before* `BeginSync(ctx)` snapshots the dirty set. |
| `create_syscall_before_phase1` | `_ZN4dcfs5cache11BeginCreateERNS_7ContextElNSt3__117basic_string_viewIcNS3_11char_traitsIcEEEE` (`cache::BeginCreate`, [`dcfs/BUILD.bazel:1345, 1380`]) | [`dcfs/testonly/create_syscall_before_phase1.cc:21-38`]: on the first `BeginCreate`, emits a `trace::BackingCreate` event before calling real `BeginCreate`. |
| `swallow_syscall_error` | `backing::UnlinkAt` ([`dcfs/BUILD.bazel:1347, 1387`]) | [`dcfs/testonly/swallow_syscall_error.cc:23-42`]: on the first `backing::UnlinkAt`, emits `trace::BackingUnlink(ctx, ENOENT)` and returns `absl::OkStatus()` instead of calling real `UnlinkAt`, so phase 3 marks the entry absent after a failed syscall. |

### A.8 [`//dcfs:dir_cache_fs_crash_during_recovery_test`] ([`dcfs/testonly/BUILD.bazel:310-334`])

| Wrapped Symbol | `BUILD.bazel` Line | `__wrap_*` Definition & Injected Fault (`file:line`) |
| :--- | :--- | :--- |
| `_ZN4dcfs5cache12RecoverDirtyERNS_7ContextE` (`cache::RecoverDirty`) | [`dcfs/BUILD.bazel:1429, 1444`] | [`dcfs/testonly/recover_dirty_fails_once.cc:20-35`]: fails the first `cache::RecoverDirty` call with `absl::AbortedError("injected crash during recovery")` so recovery runs a second time on remount. |
| `_ZN4dcfs7backing12BackingNlinkERNS_7ContextEl` (`backing::BackingNlink`) | [`dcfs/BUILD.bazel:1431, 1445`] | [`dcfs/testonly/probe_fails_once.cc:20-38`]: fails the first `backing::BackingNlink` call with `absl::UnavailableError("injected probe failure during recovery")` so the startup probe retries. |

### A.9 [`//test/qemu:waitexit_test`] ([`test/qemu/waitexit_test.cc`])

| Wrapped Symbol | `BUILD.bazel` Line | `__wrap_*` Definition | Test(s) & Injected Fault (`file:line`) |
| :--- | :--- | :--- | :--- |
| `pidfd_open` | [`test/qemu/BUILD.bazel:180`] | [`test/qemu/waitexit_test.cc:56-71`] | `EsrchExitsZeroImmediately` ([`lines 123-128`]): fails with `ESRCH`. Other fake-poll tests ([`lines 130-173`]): returns `open("/dev/null", O_RDONLY | O_CLOEXEC)` ([`line 69`]). |
| `poll` | [`test/qemu/BUILD.bazel:181`] | [`test/qemu/waitexit_test.cc:73-96`] | `TimeoutExits124` ([`lines 130-137`]), `EintrIsRetriedThenExitsZero` ([`lines 140-149`]), `PollErrorExitsOne` ([`lines 152-160`]): steps through `g_poll_steps` ([`lines 21-25, 78-95`]) to return `0` (timeout), `-1` (`EINTR`/`EFAULT`), or `1` (`POLLIN`). |

---

## Appendix B: Inventory of the `syscalls::` Layer and Production Callers

### B.1 The Three `syscalls::` Libraries

1. **[`//dcfs:syscalls`]** ([`dcfs/syscalls.h:1-125`], [`dcfs/syscalls.cc:1-254`], [`dcfs/fd.h:1-32`], [`dcfs/fd.cc:1-54`]) — **32 functions** in `namespace dcfs::syscalls` + `FileDescriptor`:
   - File descriptors: [`close`], [`dup`], [`fcntl`], [`flock`], [`poll`].
   - Process credentials & state: [`setfsuid`], [`setfsgid`], [`fsuid`], [`fsgid`], [`umask`], [`setgroups`], [`getgroups`], [`getpid`], [`getuid`], [`setsid`], [`chdir`], [`getrlimit`], [`setrlimit`].
   - Time, sync, IPC, syslog: [`mkdtemp`], [`mkstemp`], [`sync`], [`nanosleep`], [`realpath`], [`clock_gettime`], [`socketpair`], [`send`], [`openlog`], [`syslog`].
   - Host-local paths (daemon startup, `/proc`, lock files): [`open`], [`mkdir`], [`unlink`], [`stat`].
2. **[`//dcfs:syscalls_backing`]** ([`dcfs/syscalls_backing.h:1-160`], [`dcfs/syscalls_backing.cc:1-442`]) — **41 functions** in `namespace dcfs::syscalls`, every one guarded by [`RETURN_IF_ERROR(BackingDoor())`] (enforced by [`tools/style_matchers/backing_door_guard.query:1-9`]) and dependency-restricted by [`dcfs/syscalls_backing_users.txt:1-64`]:
   - Open & handles: [`openat`], [`name_to_handle_at`], [`open_by_handle_at`].
   - Stat & dir reading: [`fstatvfs`], [`fstat`], [`fstatat`], [`ioctl`] (template at [`lines 148-153`]), [`getdents64`], [`lseek`], [`statx`], [`fstatfs`], [`readlinkat`].
   - Extended attributes: [`fgetxattr`], [`flistxattr`], [`fsetxattr`], [`fremovexattr`], [`getxattr`], [`listxattr`], [`setxattr`], [`removexattr`].
   - Tree & attribute mutations: [`fchmodat`], [`utimensat`], [`linkat`], [`unlinkat`], [`renameat2`], [`mkdirat`], [`mknodat`], [`symlinkat`], [`fchmod`], [`fchownat`], [`futimens`], [`ftruncate`].
   - Data I/O, sync, mount: [`read`], [`fsync`], [`fdatasync`], [`syncfs`], [`fallocate`], [`pread`], [`copy_file_range`], [`pwrite`], [`write`], [`mount`], [`umount2`].
3. **[`//dcfs:syscalls_process`]** ([`dcfs/syscalls_process.h:1-29`], [`dcfs/syscalls_process.cc:1-57`]) — **7 functions** in `namespace dcfs::syscalls`:
   - [`fork`], [`execv`], [`waitpid`], [`kill`], [`dup2`], [`pause`], [`_exit`].

### B.2 Production Callers of `syscalls::`

| Production File | Library Deps | Call Count | `syscalls::` Functions Called (`file:line`) |
| :--- | :--- | :--- | :--- |
| [`dcfs/backing.cc`] | `syscalls`, `syscalls_backing` | 74 | `getgroups` ([`:77, 81`]), `setfsuid` ([`:103, 129`]), `setfsgid` ([`:104, 130`]), `setgroups` ([`:106, 131`]), `fsuid` ([`:112`]), `fsgid` ([`:115`]), `umask` ([`:120, 126`]), `readlinkat` ([`:147`]), `ioctl` ([`:168, 2248, 2260, 2265`]), `listxattr` ([`:177, 188`]), `getxattr` ([`:204, 219`]), `openat` ([`:286, 305, 577, 653, 814, 1081, 1413, 1791, 2111, 2243`]), `statx` ([`:322, 417, 1138, 1711, 2127, 2507`]), `getdents64` ([`:724`]), `mkdirat` ([`:1296`]), `mknodat` ([`:1355`]), `symlinkat` ([`:1467`]), `unlinkat` ([`:1519, 1559`]), `renameat2` ([`:1619`]), `linkat` ([`:1690`]), `fchmod` ([`:1821`]), `fchmodat` ([`:1827`]), `fchownat` ([`:1839`]), `ftruncate` ([`:1849, 1933`]), `futimens` ([`:1871`]), `utimensat` ([`:1875`]), `fsetxattr` ([`:1979`]), `setxattr` ([`:1983`]), `fremovexattr` ([`:2035`]), `removexattr` ([`:2039`]), `pread` ([`:2163`]), `pwrite` ([`:2176`]), `fallocate` ([`:2200`]), `copy_file_range` ([`:2215`]), `fdatasync` ([`:2282`]), `fsync` ([`:2282, 2324`]), `syncfs` ([`:2369`]), `fstatfs` ([`:2448, 2539`]), `fstatvfs` ([`:2450`]). |
| [`dcfs/file_handle.cc`] | `syscalls_backing` | 5 | `name_to_handle_at` ([`:51`]), `statx` ([`:142, 145`]), `openat` ([`:159`]), `open_by_handle_at` ([`:186`]). |
| [`dcfs/device_id.cc`] | `syscalls_backing` | 4 | `fstatfs` ([`:113`]), `ioctl` ([`:125, 133`]), `openat` ([`:132`]). |
| [`dcfs/fd.cc`] | `syscalls` | 2 | `close` ([`:19, 40`]). |
| [`dcfs/dir_cache_fs.cc`] | `syscalls` | 1 | `getrlimit` ([`:142`]; also calls libfuse `fuse_passthrough_open` at [`:162`] and `fuse_passthrough_close` at [`:172, 217`]). |
| [`dcfs/session_loop.cc`] | `syscalls` | 1 | `poll` ([`:105`]). |
| [`dcfs/fsck.cc`] | `syscalls`, `syscalls_backing`, `syscalls_process` | 11 | `read` ([`:56`]), `socketpair` ([`:73`]), `fork` ([`:75`]), `dup2` ([`:78`]), `_exit` ([`:79, 88`]), `execv` ([`:87`]), `waitpid` ([`:96`]), `openat` ([`:297`]), `flock` ([`:311`]), `unlinkat` ([`:354`]). |
| [`dcfs/startup_channel.cc`] & [`dcfs/fork_split.h`] | `syscalls`, `syscalls_backing`, `syscalls_process` | 11 | `read` ([`startup_channel.cc:42`]), `setsid` ([`:53`]), `chdir` ([`:54`]), `openat` ([`:56`]), `dup2` ([`:58`]), `send` ([`:72, 84`]), `waitpid` ([`:102`]), `socketpair` ([`:128`]); `fork` ([`fork_split.h:36`]), `_exit` ([`fork_split.h:40`]). |
| [`dcfs/umount_helper.cc`] | `syscalls`, `syscalls_process` | 17 | `mkdir` ([`:99`]), `flock` ([`:109, 134`]), `realpath` ([`:158`]), `open` ([`:212, 232`]), `stat` ([`:221, 240`]), `unlink` ([`:247`]), `fork` ([`:279, 289`]), `execv` ([`:281, 291`]), `_exit` ([`:282, 292`]), `waitpid` ([`:284, 294`]). |
| [`dcfs/main.cc`], [`mounts_below.cc`], [`remount.cc`], [`absolute_paths.cc`], [`syslog_sink.cc`] | `syscalls`, `syscalls_backing`, `syscalls_process` | 28 | Startup/CLI plumbing: `getuid`, `setrlimit`, `openat`, `mkdirat`, `flock`, `fstat`, `statx`, `mount`, `umount2`, `fork`, `execv`, `_exit`, `waitpid`, `read`, `realpath`, `openlog`, `syslog`. |
