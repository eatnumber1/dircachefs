# Intentional Crash Audit in Production Code (Step 25.9)

**Date:** 2026-10-11  
**Scope:** `dcfs/`, `bench/`, and shipped tools (`tools/*.c`, `tools/*.cc`)  
**Rules & Precedence:** `AGENTS.md`, `docs/style.md` section 1.6b, Abseil Tips of the Week (TotW #42)  
**Governing Principle from Russ:**
> "no intentional crashes. E.g. no use of `LOG(FATAL)` or `CHECK` (`RET_CHECK` is ok because it doesn't crash). I understand the necessity sometimes, but try hard to avoid it (for instance, another style rule that's in an Abseil TOTW somewhere: don't call methods that can fail inside constructors, instead add a static Create method). If you *really* feel you need a crash, bring it to me for review and I'll consider on a case-by-case basis."

---

## 1. Executive Summary & Census

A complete census and audit of production code (`dcfs/`, `bench/`, and shipped tools `tools/*.c`) was conducted across all intentional termination mechanisms.

### Summary Table

| Category | Production Count | Test & Testonly Count | Status / Remedy Summary |
|---|:---:|:---:|---|
| `CHECK*` / `QCHECK*` / `DCHECK*` | **25** | 59 lines (44 testonly, 15 tests) | 1 clean conversion to `RET_CHECK` (`ReplyNone`); 17 FUSE pointer checks convertible to `RET_CHECK` inside `Serve`; 1 candidate for approval (`RestoreRoot`); 6 FUSE lifecycle/session checks audited |
| `LOG(FATAL)` / `LOG(QFATAL)` | **0** | 4 (invariant checking / death tests) | **Zero in production.** |
| `abort()` / `std::abort()` | **1** | 0 | `fork_split.h:41` (guard if parent returns; case presented for approval vs `_exit`) |
| `assert()` / `std::assert()` | **0** | 0 | **Zero in production** (and zero across test files). |
| `std::terminate` | **0** | 0 | **Zero in production.** |
| `exit()` / `_exit()` | **10** | 10 in unit tests/fault tests | 8 standard fork/exec child exits; 2 CLI wrapper exits (`AwaitReportAndExit`); 1 detachment exit (`BecomeDaemon`, convertible to Status) |

---

## 2. Full Census of Production `CHECK*` and `abort()` Sites

### 2.1 Core Daemon Logic

#### Site 1: `dcfs/backing.cc:281` — Credential Restoration Failure
- **Location:** [`dcfs/backing.cc:277-285`](file:///home/russ/Sources/dircachefs-lanes/lane-3/dcfs/backing.cc#L277-L285)
- **Surrounding Logic:**
  ```cpp
  void RestoreRoot(const SavedGroups &groups) {
    absl::Status uid = syscalls::setfsuid(0);
    absl::Status status = syscalls::setgroups(groups);
    absl::Status gid = syscalls::setfsgid(0);
    CHECK(uid.ok() && status.ok() && gid.ok())
        << "cannot restore root filesystem credentials: setfsuid " << uid
        << ", setgroups " << status << ", setfsgid " << gid << " (fsuid "
        << syscalls::fsuid() << ", fsgid " << syscalls::fsgid() << ")";
  }
  ```
  `RestoreRoot` is invoked via `absl::Cleanup` upon leaving `SwitchTo` or `AsCaller` after executing a backing filesystem syscall as the unprivileged caller. It resets `fsuid`, `fsgid`, and supplementary groups back to root (0).
- **Remedy Options:**
  - **Option A (Conversion to Status):**
    If `RestoreRoot` returns `absl::Status`, `AsCaller`'s `absl::Cleanup` cannot directly return that status without capturing it in an outer status variable. More critically, if credentials fail to restore, the thread cannot continue serving requests safely.
  - **Option B (Formal Case for Intentional Crash Requiring Russ's Approval):**
    If `RestoreRoot()` fails to restore root credentials, the executing daemon thread remains trapped in the unprivileged caller's UID, GID, or supplementary group set. Allowing execution to continue would cause all subsequent operations processed on that thread—including requests from unrelated users, database cache synchronization, and internal root-only operations—to execute under incorrect credentials or fail with spurious `EPERM`/`EACCES`, breaking Unix privilege boundaries and multi-tenant isolation. Because the thread is left in an irrecoverably corrupted security state, immediate termination is required.

---

#### Site 2: `dcfs/fuse_request.cc:283` — Single-Reply Invariant in `ReplyNone()`
- **Location:** [`dcfs/fuse_request.cc:278-286`](file:///home/russ/Sources/dircachefs-lanes/lane-3/dcfs/fuse_request.cc#L278-L286)
- **Surrounding Logic:**
  ```cpp
  void FuseRequest::ReplyNone() {
    // ReplyNone() returns void (there is nothing to report to the caller, and
    // no reply function call that could itself fail), so unlike the other
    // Reply* methods this uses CHECK rather than RET_CHECK to enforce the
    // single-reply invariant.
    CHECK(req_.has_value()) << "FuseRequest already replied";
    fuse_reply_none(*req_);
    req_ = std::nullopt;
  }
  ```
- **Remedy (Clean Conversion to `Status`):**
  Convert `ReplyNone()` to return `absl::Status`:
  ```cpp
  absl::Status FuseRequest::ReplyNone() {
    RET_CHECK(req_.has_value()) << "FuseRequest already replied";
    fuse_reply_none(*req_);
    req_ = std::nullopt;
    return absl::OkStatus();
  }
  ```
  The only callers in production code are `dir_cache_fs.cc:998` (`Forget`) and `dir_cache_fs.cc:1018` (`ForgetMulti`). Both callers can either return or ignore the status via `req.ReplyNone().IgnoreError()` or update their return signatures. This completely eliminates the `CHECK` with standard `RET_CHECK`.

---

#### Site 3: `dcfs/fork_split.h:41` — Parent Callback Return Trap
- **Location:** [`dcfs/fork_split.h:33-43`](file:///home/russ/Sources/dircachefs-lanes/lane-3/dcfs/fork_split.h#L33-L43)
- **Surrounding Logic:**
  ```cpp
  template <typename Child, typename Parent>
  __attribute__((
      no_profile_instrument_function)) absl::StatusOr<std::invoke_result_t<Child>>
  ForkSplit(Child child, Parent parent) {
    absl::StatusOr<pid_t> pid = syscalls::fork();
    if (!pid.ok()) return pid.status();
    if (*pid == 0) return child();
    parent(*pid);
    std::abort();  // `parent` returned: a bug, and it must not run on.
  }
  ```
  `ForkSplit` provides `fork(2)` semantics without a frame that returns twice under LLVM continuous code coverage. The child executes `child()` and returns its result; the parent executes `parent(*pid)` which is required to terminate (e.g., via `std::exit` or `_exit`).
- **Remedy Options:**
  - **Option A (Replacement with `_exit`):**
    Replace `std::abort()` with `syscalls::_exit(127)`. While technically still a process exit, it replaces an intentional abort crash with an exit code indicating an unreachable path failure.
  - **Option B (Formal Case for Intentional Crash Requiring Russ's Approval):**
    The contract of `ForkSplit` requires that `parent(*pid)` never returns. If the parent callback were to return, allowing execution to proceed would cause both the parent process and child daemon process to return from `ForkSplit` into the caller (`MountHelperMain`), resulting in two active daemon instances concurrently accessing the same cache database, mountpoint, and lockfiles. Because continuing execution causes catastrophic concurrent process duplication, an immediate abort is warranted if `parent` returns.

---

### 2.2 FUSE Low-Level Callbacks (`dcfs/fuse_ops.cc`)

`dcfs/fuse_ops.cc` contains 23 instances of `CHECK_NE`. These fall into three distinct functional groups:

#### Group 1: Null Pointer Validation of FUSE Arguments (17 Sites)
- **Locations:**
  - [`fuse_ops.cc:234`](file:///home/russ/Sources/dircachefs-lanes/lane-3/dcfs/fuse_ops.cc#L234): `ForgetMulti` (`CHECK_NE(forgets, nullptr)`)
  - [`fuse_ops.cc:340`](file:///home/russ/Sources/dircachefs-lanes/lane-3/dcfs/fuse_ops.cc#L340): `Open` (`CHECK_NE(fi, nullptr)`)
  - [`fuse_ops.cc:347`](file:///home/russ/Sources/dircachefs-lanes/lane-3/dcfs/fuse_ops.cc#L347): `Read` (`CHECK_NE(fi, nullptr)`)
  - [`fuse_ops.cc:356`](file:///home/russ/Sources/dircachefs-lanes/lane-3/dcfs/fuse_ops.cc#L356): `Write` (`CHECK_NE(fi, nullptr)`)
  - [`fuse_ops.cc:364`](file:///home/russ/Sources/dircachefs-lanes/lane-3/dcfs/fuse_ops.cc#L364): `Flush` (`CHECK_NE(fi, nullptr)`)
  - [`fuse_ops.cc:371`](file:///home/russ/Sources/dircachefs-lanes/lane-3/dcfs/fuse_ops.cc#L371): `Release` (`CHECK_NE(fi, nullptr)`)
  - [`fuse_ops.cc:379`](file:///home/russ/Sources/dircachefs-lanes/lane-3/dcfs/fuse_ops.cc#L379): `Fsync` (`CHECK_NE(fi, nullptr)`)
  - [`fuse_ops.cc:387`](file:///home/russ/Sources/dircachefs-lanes/lane-3/dcfs/fuse_ops.cc#L387): `Opendir` (`CHECK_NE(fi, nullptr)`)
  - [`fuse_ops.cc:396`](file:///home/russ/Sources/dircachefs-lanes/lane-3/dcfs/fuse_ops.cc#L396): `Readdir` (`CHECK_NE(fi, nullptr)`)
  - [`fuse_ops.cc:405`](file:///home/russ/Sources/dircachefs-lanes/lane-3/dcfs/fuse_ops.cc#L405): `Readdirplus` (`CHECK_NE(fi, nullptr)`)
  - [`fuse_ops.cc:413`](file:///home/russ/Sources/dircachefs-lanes/lane-3/dcfs/fuse_ops.cc#L413): `Releasedir` (`CHECK_NE(fi, nullptr)`)
  - [`fuse_ops.cc:422`](file:///home/russ/Sources/dircachefs-lanes/lane-3/dcfs/fuse_ops.cc#L422): `Fsyncdir` (`CHECK_NE(fi, nullptr)`)
  - [`fuse_ops.cc:473`](file:///home/russ/Sources/dircachefs-lanes/lane-3/dcfs/fuse_ops.cc#L473): `Create` (`CHECK_NE(fi, nullptr)`)
  - [`fuse_ops.cc:482`](file:///home/russ/Sources/dircachefs-lanes/lane-3/dcfs/fuse_ops.cc#L482): `Fallocate` (`CHECK_NE(fi, nullptr)`)
  - [`fuse_ops.cc:492`](file:///home/russ/Sources/dircachefs-lanes/lane-3/dcfs/fuse_ops.cc#L492): `CopyFileRange` (`CHECK_NE(fi_in, nullptr)`)
  - [`fuse_ops.cc:493`](file:///home/russ/Sources/dircachefs-lanes/lane-3/dcfs/fuse_ops.cc#L493): `CopyFileRange` (`CHECK_NE(fi_out, nullptr)`)
  - [`fuse_ops.cc:523`](file:///home/russ/Sources/dircachefs-lanes/lane-3/dcfs/fuse_ops.cc#L523): `Tmpfile` (`CHECK_NE(fi, nullptr)`)
- **Surrounding Logic:**
  These entry points are the C callbacks called by libfuse with pointers to `fuse_file_info` or `fuse_forget_data`. If the pointers were null, dereferencing them would cause a segmentation fault.
- **Remedy (Clean Conversion to `RET_CHECK` & Status Reply):**
  Instead of crashing the entire daemon process with `CHECK_NE`, each check can be moved inside the `Serve` lambda using `RET_CHECK_NE`:
  ```cpp
  void Open(fuse_req_t req, fuse_ino_t ino, fuse_file_info *fi) {
    Serve(req, {.op = events::Op::kOpen, .ino = Ino(ino)},
          [&](DirCacheFS &fs, FuseRequest &fr) -> absl::Status {
            RET_CHECK_NE(fi, nullptr) << "null fuse_file_info in Open";
            return fs.Open(fr, ino, *fi);
          });
  }
  ```
  When `fi == nullptr`:
  1. `RET_CHECK_NE` returns an `absl::InternalError`.
  2. `Serve` receives the failed status and executes `fr.ReplyFailureAndLogIfNotOk(status)`.
  3. The error is logged and replied to the kernel (translated by `StatusToErrno` to `ELIBBAD`/`EIO`).
  4. The daemon continues operating safely without crash.
  For `ForgetMulti` (which sends no reply), if `forgets == nullptr && count > 0`, log an ERROR and return immediately:
  ```cpp
  if (forgets == nullptr && count > 0) {
    LOG(ERROR) << "ForgetMulti received null forgets pointer with non-zero count";
    return;
  }
  ```

---

#### Group 2: Session Userdata Validation (2 Sites)
- **Locations:**
  - [`fuse_ops.cc:131`](file:///home/russ/Sources/dircachefs-lanes/lane-3/dcfs/fuse_ops.cc#L131): `GetFS` (`CHECK_NE(fs, nullptr)`)
  - [`fuse_ops.cc:327`](file:///home/russ/Sources/dircachefs-lanes/lane-3/dcfs/fuse_ops.cc#L327): `Link` (`CHECK_NE(fs, nullptr)`)
- **Surrounding Logic:**
  `fuse_req_userdata(req)` retrieves the `DirCacheFS` pointer passed during `fuse_session_new`.
- **Remedy:**
  - In `Link`: the check at line 327 is performed only to evaluate `fs->IsUnnamedTmpfile(ino)` before calling `Serve`. If `IsUnnamedTmpfile` is checked inside the `Serve` handler (where `GetFS(req)` has already been called), line 327's `CHECK_NE` is completely redundant and can be deleted.
  - In `GetFS`:
    - **Conversion Option:** Return `absl::StatusOr<DirCacheFS&>` from `GetFS(req)`. In `Serve()`, if `GetFS` returns an error, `fr.ReplyFailureAndLogIfNotOk(status)` replies `EFAULT` and logs the error.
    - **Formal Case for Crash Option:** If `fuse_req_userdata(req)` is null, libfuse has dispatched a request on a session whose primary filesystem context was never bound or has been corrupted. Because no filesystem state exists to process requests, crashing prevents undefined pointer dereferences.

---

#### Group 3: FUSE Lifecycle (`Init`, `Destroy`) Validation (4 Sites)
- **Locations:**
  - [`fuse_ops.cc:187`](file:///home/russ/Sources/dircachefs-lanes/lane-3/dcfs/fuse_ops.cc#L187): `Init` (`CHECK_NE(userdata, nullptr)`)
  - [`fuse_ops.cc:188`](file:///home/russ/Sources/dircachefs-lanes/lane-3/dcfs/fuse_ops.cc#L188): `Init` (`CHECK_NE(conn, nullptr)`)
  - [`fuse_ops.cc:201`](file:///home/russ/Sources/dircachefs-lanes/lane-3/dcfs/fuse_ops.cc#L201): `Init` (`CHECK_NE(unoffered, 0u) << "cannot refuse the INIT: every flag is offered"`)
  - [`fuse_ops.cc:206`](file:///home/russ/Sources/dircachefs-lanes/lane-3/dcfs/fuse_ops.cc#L206): `Destroy` (`CHECK_NE(userdata, nullptr)`)
- **Surrounding Logic & Remedies:**
  - **`Init` lines 187, 188:** Libfuse's `init()` callback returns `void`. If `userdata == nullptr` or `conn == nullptr`, dcfs cannot initialize. Instead of `CHECK_NE`, log `LOG(ERROR)` and trigger the unoffered capability flag refusal mechanism (lines 200–202) so the mount is cleanly refused with `EPROTO` rather than crashing the process.
  - **`Init` line 201:** Line 201 checks that at least one capability bit is unoffered by the kernel so dcfs can request it in `want_ext` to force libfuse to fail the mount with `EPROTO`. Because libfuse defines capability bits up to bit 31, bit 63 is never offered.
    - *Remedy:* If `unoffered == 0u`, log `LOG(ERROR) << "cannot refuse INIT: all capability flags offered"` and return. Alternatively, make the case for crash: if every flag is offered, dcfs cannot signal initialization failure to libfuse, and mounting in an invalid configuration risks data corruption.
  - **`Destroy` line 206:** `Destroy` runs during unmount shutdown. If `userdata == nullptr`, crashing aborts the process instead of letting the session close cleanly.
    - *Remedy:* Replace with `if (userdata == nullptr) { LOG(ERROR) << "Destroy called with null userdata"; return; }`.

---

## 3. Audit of Process Exit (`exit` / `_exit`) Sites

`docs/style.md` 1.6b prohibits "an `exit()` that stands in for an error return (distinguishing legitimate CLI lifecycle exits)." A complete review of the 10 production sites demonstrates that 9 are legitimate process lifecycle boundaries, and 1 is a candidate for conversion to a `Status` return:

| Site | Location | Role / Context | Classification |
|---|---|---|---|
| 1 | [`bench/process.cc:73`](file:///home/russ/Sources/dircachefs-lanes/lane-3/bench/process.cc#L73) | `dcfs::syscalls::_exit(127)` if `execv` fails in child benchmark process | **Legitimate child lifecycle exit.** Standard POSIX fork/exec child termination. |
| 2 | [`dcfs/backing_capture.cc:86`](file:///home/russ/Sources/dircachefs-lanes/lane-3/dcfs/backing_capture.cc#L86) | `syscalls::_exit(126)` if `dup2` fails in child `/bin/mount` process | **Legitimate child lifecycle exit.** Child setup termination before exec. |
| 3 | [`dcfs/backing_capture.cc:95`](file:///home/russ/Sources/dircachefs-lanes/lane-3/dcfs/backing_capture.cc#L95) | `syscalls::_exit(127)` if `execv` fails in child `/bin/mount` process | **Legitimate child lifecycle exit.** Standard POSIX child termination. |
| 4 | [`dcfs/backing_capture.cc:191`](file:///home/russ/Sources/dircachefs-lanes/lane-3/dcfs/backing_capture.cc#L191) | `syscalls::_exit(0)` in private mount namespace helper process | **Legitimate child lifecycle exit.** Helper subprocess clean completion exit after sending descriptor over socketpair. |
| 5 | [`dcfs/fsck.cc:92`](file:///home/russ/Sources/dircachefs-lanes/lane-3/dcfs/fsck.cc#L92) | `syscalls::_exit(126)` if `dup2` fails in child fsck helper | **Legitimate child lifecycle exit.** Child setup termination before exec. |
| 6 | [`dcfs/fsck.cc:101`](file:///home/russ/Sources/dircachefs-lanes/lane-3/dcfs/fsck.cc#L101) | `syscalls::_exit(127)` if `execv` fails in child fsck helper | **Legitimate child lifecycle exit.** Standard POSIX child termination. |
| 7 | [`dcfs/startup_channel.cc:99`](file:///home/russ/Sources/dircachefs-lanes/lane-3/dcfs/startup_channel.cc#L99) | `std::exit(0)` in parent CLI wrapper upon receiving daemon ready report | **Legitimate CLI lifecycle exit.** Parent CLI returns success to invoking terminal after daemonization. |
| 8 | [`dcfs/startup_channel.cc:109`](file:///home/russ/Sources/dircachefs-lanes/lane-3/dcfs/startup_channel.cc#L109) | `std::exit(report.exit_status)` in parent CLI wrapper upon daemon startup error | **Legitimate CLI lifecycle exit.** Parent CLI returns error status to invoking terminal after daemonization failure. |
| 9 | [`dcfs/startup_channel.cc:121`](file:///home/russ/Sources/dircachefs-lanes/lane-3/dcfs/startup_channel.cc#L121) | `syscalls::_exit(ExitStatusFor(detached))` in child daemon if `DetachDaemon()` fails | **Candidate for conversion to `Status` return.** |
| 10 | [`dcfs/umount_helper.cc:182`](file:///home/russ/Sources/dircachefs-lanes/lane-3/dcfs/umount_helper.cc#L182) | `syscalls::_exit(127)` if `execv` fails in child `/bin/umount` process | **Legitimate child lifecycle exit.** Standard POSIX child termination. |

### Detailed Evaluation of Site 9 (`startup_channel.cc:121`):
In `BecomeDaemon()`:
```cpp
if (absl::Status detached = DetachDaemon(); !detached.ok()) {
  reporter.Fail(detached);
  syscalls::_exit(ExitStatusFor(detached));
}
```
Currently, `BecomeDaemon` returns `StartupReporter` and calls `syscalls::_exit` on failure.
**Remedy:** Modify `BecomeDaemon` to return `absl::StatusOr<StartupReporter>`. On failure, call `reporter.Fail(detached)` (so the parent wrapper receives the diagnostic report), and `return detached;`. `ForkDaemon()` then returns `detached`, and `MountHelperMain()` in `main.cc:690` handles the returned error through its existing return path (`return ExitStatusFor(status);`). This converts an explicit `_exit()` in production library code into a standard `StatusOr` propagation.

---

## 4. Audit of Fallible Work in Constructors (Abseil TotW #42)

The codebase was audited against Abseil Tip of the Week #42 ("Prefer Factory Functions to Initializer Methods") and the Google C++ Style Guide ("Doing Work in Constructors"):

- **`Connection` ([`sqlite.h`](file:///home/russ/Sources/dircachefs-lanes/lane-3/dcfs/sqlite.h)):** Constructor is private; all database openings use static factory `Connection::Open(...)` returning `absl::StatusOr<Connection>`.
- **`Statement` ([`sqlite.h`](file:///home/russ/Sources/dircachefs-lanes/lane-3/dcfs/sqlite.h)):** Constructor is private; all statement preparations use static factory `Statement::Prepare(...)` returning `absl::StatusOr<Statement>`.
- **`FileHandle` ([`file_handle.h`](file:///home/russ/Sources/dircachefs-lanes/lane-3/dcfs/file_handle.h)):** Constructors perform trivial field assignment; fallible parsing and statx translations use `FileHandle::Parse(...)` and `FileHandle::FromStatx(...)` returning `absl::StatusOr<FileHandle>`.
- **`DeviceId` ([`device_id.h`](file:///home/russ/Sources/dircachefs-lanes/lane-3/dcfs/device_id.h)):** Constructors perform trivial assignment; parsing uses `DeviceId::Parse(...)` returning `absl::StatusOr<DeviceId>`.
- **`DirCacheFS` ([`dir_cache_fs.h`](file:///home/russ/Sources/dircachefs-lanes/lane-3/dcfs/dir_cache_fs.h)):** Constructor only performs pointer binding to `Context` and calculates descriptor reserve limits from soft `RLIMIT_NOFILE`. All fallible initialization work is deferred to `Init(fuse_conn_info &conn)` returning `absl::Status`.
- **Conclusion:** Production classes already fully comply with the rule; zero constructors perform fallible operations or invoke throwing/crashing methods.

---

## 5. Permitted Crashes in Test Code & Harnesses

Under `docs/style.md` 1.6b:
> "crashes are permitted in tests when invariants fail that aren't under test. Like if we fail to write to disk while setting up the database for a test that's checking whether open() succeeds through dcfs, that's obviously not related to the test itself, so in test code it's ok to crash (but still prefer to return errors as appropriate)."

The census verified:
- **`LOG(FATAL)` in tests:** Exactly 4 instances, all in runtime invariant validation harnesses (`dcfs/dir_cache_fs_test.cc:219,226`, `dcfs/testonly/invariant_checker.cc:318`, `dcfs/testonly/invariant_checker.h:78`), used specifically to abort on invariant violations in death tests (`DirCacheFSDeathTest.*`).
- **`CHECK` / `CHECK_OK` in test setup & harnesses:** 59 lines across `*_test.cc` and `testonly/` (fixtures, trace recorder, and invariant checking). These correctly use `CHECK_OK` for test infrastructure preambles rather than gtest `ASSERT` macros, preserving the distinction between infrastructure failure and property-under-test failure.

---

## 6. Consolidated Recommendations for Russ

1. **Approve Conversion to `Status` / `RET_CHECK` (No Crash Needed):**
   - **`FuseRequest::ReplyNone` (`dcfs/fuse_request.cc:283`):** Convert to return `absl::Status` with `RET_CHECK`.
   - **`fuse_ops.cc` 17 Pointer Checks (`lines 340-523`):** Convert to `RET_CHECK_NE` inside `Serve` lambdas, returning `Status` and sending error replies (`EINVAL`) to the kernel.
   - **`fuse_ops.cc:327` (`Link`):** Delete redundant `CHECK_NE(fs, nullptr)` and resolve `fs` inside `Serve`.
   - **`fuse_ops.cc:206` (`Destroy`):** Replace `CHECK_NE(userdata, nullptr)` with null check and early return.
   - **`startup_channel.cc:121` (`BecomeDaemon`):** Convert to return `absl::StatusOr<StartupReporter>` and propagate error to `main()`.

2. **Requests for Russ's Explicit Approval for Intentional Crash:**
   - **`dcfs/backing.cc:281` (`RestoreRoot`):** Approve `CHECK` when root credentials cannot be restored, because continuing execution in an unprivileged state compromises the daemon's security domain and violates user isolation.
   - **`dcfs/fork_split.h:41` (`ForkSplit`):** Approve `std::abort()` (or allow `syscalls::_exit(127)`) when a parent callback returns, because returning would duplicate daemon execution across parent and child processes.
   - **`dcfs/fuse_ops.cc:131` (`GetFS`) & `201` (`Init`):** Determine whether uninitialized session userdata or inability to refuse FUSE init should crash the process or log `ERROR` and return.
