> **Status (2026-10-16, the regular orchestrator):** sections 3 and 4 (the
> virtual-Syscalls refactor and its remaining work) are void: russ agreed to
> revert it (docs/design.md, "Syscall fault injection stays link-time"); the
> work is on the unpushed `attic/virtual-syscalls` branch in lane-1. The
> watchdog tool and the /tmp files it names were specific to the temporary
> orchestrator's harness.

# Handoff to Claude: dcfs Development State & Next Steps

**Date:** 2026-10-16  
**Repository:** `dircachefs`  
**Host Working Directory (`main`):** `/home/russ/Sources/dircachefs`  
**Active Worker Worktree (`step-virtual-syscalls`):** `/home/russ/Sources/dircachefs-lanes/lane-1`  

---

## 1. Executive Summary & Current State

1. **Watchdog Daemon**:
   - `tools/agent_watchdog.py` was developed, tested, reviewed, committed to `main` at `9efbcef`, and has been **completely shut down** per maintainer request (`task-5096` terminated).
2. **Subagents**:
   - All background subagents have been terminated (`manage_subagents(Action='kill_all')`). No background tasks or subagents are currently running.
3. **Host `main` Status**:
   - Clean working tree at commit `9efbcef`:
     - `9efbcef`: `agents: implement mechanical watchdog daemon and tests`
     - `0dabe61`: `plan: 5.2 act repository cache mount and agent operational policies`
     - `490b667`: `5.2: third_party/act: mount host repository cache into runner container`
     - `77a7c5d`: `plan: 25.28 periodic SQLite WAL checkpoints merged (ae8db94)`
     - `ae8db94`: `25.28: dcfs: periodic SQLite WAL checkpoints tied to sync_interval`
4. **Active Task in Progress (`lane-1`)**:
   - **Task**: Complete elimination of all link-time `-Wl,--wrap` flags across `dcfs/`, replacing them with a dependency-injected virtual inheritance `Syscalls` class hierarchy.
   - **Branch**: `step-virtual-syscalls` in `/home/russ/Sources/dircachefs-lanes/lane-1`.
   - **Status**: ~85% complete. Core hierarchy, production call sites, and several fault tests are wired. Implementation was paused in `dcfs/dir_cache_fs_test.cc` when the previous implementer encountered resource quota exhaustion.

---

## 2. Invariants & Maintainer Directives

Before touching code, note these critical project rules:
1. **Lowercase libc method names**:
   - Method names in `class Syscalls` and `class RealSyscalls` MUST retain lowercase libc names (e.g. `getpid()`, `openat()`, `unlinkat()`, `statfs()`, `poll()`, etc.).
   - This is an **explicit exception** to Google C++ PascalCase granted by the maintainer (russ, 2026-10-12). Do NOT rename them to PascalCase.
2. **Virtual inheritance**:
   - `class RealSyscalls : public virtual Syscalls` and test decorators/mixins (`CheckingSyscalls`, `FaultInjectingSyscalls`) MUST inherit virtually from `Syscalls` so mixins compose cleanly without diamond inheritance conflicts.
3. **No default arguments on virtual methods**:
   - Google C++ Style Guide prohibits default arguments on virtual function declarations (and overrides). If a 3-argument convenience overload is needed (e.g., `openat(dirfd, path, flags)` without mode), provide a non-virtual inline overload or pass `0`.
4. **Pointer nullability annotations**:
   - Clang enforces `-Wnullability-completeness`. All pointer parameters in `Syscalls` must explicitly have `absl_nonnull` or `absl_nullable`.
5. **Banned symbol restrictions**:
   - `tools/banned_symbols.txt` strictly allows `execv` only from `dcfs/_objs/syscalls_process/*`. Therefore, `RealSyscalls::execv` must be defined in `dcfs/syscalls_process.cc`.
6. **Backing layering check**:
   - `//tools:syscalls_backing_users_test` strictly verifies direct dependents of `//dcfs:syscalls_backing` against `dcfs/syscalls_backing_users.txt`. Keep forwarding free functions in `dcfs/syscalls_backing.h` calling `RealSyscalls::Default()` to avoid adding forbidden direct dependencies across the build graph.
7. **No `sg kvm`**:
   - The user session has direct KVM group access. Never invoke `sg kvm` or `sg qemu`. Run Bazel commands directly.
8. **Test first & tiers**:
   - Run tests with `bazel test --config=fast <target>`. Note that `--config=fast` is a test configuration (`test:fast --test_size_filters=small`), so it applies to `bazel test`, not `bazel build`.

---

## 3. Work in Progress in `lane-1`

### What is Already Completed in `lane-1`:
- **`dcfs/syscalls.h`**: Declares `class Syscalls` (pure virtual) and `class RealSyscalls : public virtual Syscalls` with default instance `RealSyscalls::Default()`, `PoisonBacking()`, and `IsBackingPoisoned()`.
- **`dcfs/syscalls.cc`**: Implements process-local and non-backing `RealSyscalls` methods.
- **`dcfs/syscalls_backing.cc`**: Implements backing filesystem calls on `RealSyscalls`.
- **`dcfs/syscalls_process.cc`**: Implements `fork`, `execv`, `waitpid`, `kill`, `dup2`, `pause`, `_exit` (satisfying banned-symbols boundaries).
- **`dcfs/context.h`**: Added `Syscalls *syscalls = &RealSyscalls::Default();` to `struct Context`.
- **Production call sites converted**:
  - `dcfs/backing.cc`: converted to route backing calls via `ctx.syscalls->...`.
  - `dcfs/device_id.cc`, `dcfs/file_handle.cc`, `dcfs/fsck.cc`, `dcfs/fuse_ops.cc`, `dcfs/fuse_request.cc`, `dcfs/metadata_cache.cc`, `dcfs/startup_channel.cc`, `dcfs/testonly/...`.
- **Tests passing**:
  - `bazel test --config=fast //dcfs:syscalls_test` (PASSED).
  - `bazel test --config=fast //dcfs:syscalls_process_test` (PASSED).

### Immediate Compiler Fix Needed:
In `dcfs/backing.cc:1815`:
```cpp
// Current:
ctx.syscalls->openat(parent_fd, name, O_PATH | O_NOFOLLOW);

// Fix:
ctx.syscalls->openat(parent_fd, name, O_PATH | O_NOFOLLOW, 0);
```
(When default arguments on virtual methods were removed, this call site was missing the 4th `mode` argument).

---

## 4. Remaining Work to Land the Refactor

1. **Fix `dcfs/backing.cc:1815`**:
   - Add `0` mode argument. Verify `//dcfs:backing_fault_test` builds.
2. **Refactor `dcfs/dir_cache_fs_test.cc` (Eliminate 30 `--wrap` flags)**:
   - In `dcfs/BUILD.bazel`: remove `DIR_CACHE_FS_TEST_WRAPS` and drop `--wrap` linkopts from `:dir_cache_fs_test`.
   - In `dcfs/dir_cache_fs_test.cc` (around lines 315–473):
     - Replace the `extern "C" __wrap_*` functions with:
       ```cpp
       class TestSyscalls : public virtual RealSyscalls {
        public:
         // Override methods to incorporate:
         // 1. NoTransactionAt(name) assertions
         // 2. DCFS_INJECT(name, failed) fault sweeps
         // 3. FakePassthrough tracking
         // 4. StatvfsReadOnly overrides
       };
       ```
     - In test harness initialization, set `ctx_.syscalls = &test_syscalls_;`.
3. **Eliminate Remaining Test `--wrap` Flags**:
   - `dcfs/backing_fault_test.cc`: inject custom `Syscalls` to simulate retry faults. Remove `--wrap` from `dcfs/BUILD.bazel`.
   - `dcfs/device_id_fault_test.cc`: inject `Syscalls` into `ProbeDeviceId` / `GetDeviceId`. Remove `--wrap` from `dcfs/BUILD.bazel`.
   - `dcfs/fsck_test.cc`: inject `Syscalls` into `Fsck`. Remove `--wrap` from `dcfs/BUILD.bazel`.
   - `dcfs/startup_channel_fault_test.cc`: inject `Syscalls` into `ForkDaemon`. Remove `--wrap` from `dcfs/BUILD.bazel`.
   - `dcfs/fuse_request_test.cc`: inject `Syscalls` for `fuse_reply_none`. Remove `--wrap` from `dcfs/BUILD.bazel`.
   - Trace fault tests (`DIR_CACHE_FS_FAULTS`): eliminate mangled symbol wrapping via clean test hooks or injected `Syscalls`.
4. **Verification**:
   - Verify `git grep -- "-Wl,--wrap" dcfs/` returns 0 matches!
   - Run:
     ```bash
     cd /home/russ/Sources/dircachefs-lanes/lane-1
     bazel test --config=fast //dcfs/...
     bazel test --config=fast //tools/...
     ```
   - Ensure formatting passes: `bazel test --config=fast //tools:format_test`.
   - Ensure style checks pass: `bazel test --config=fast //tools:style_checks_test`.
5. **Pre-Commit Review & Merge**:
   - Commit message requirements:
     - Subject must start with plan step (or approved prefix `agents:`, `plan:`, etc.).
     - Body wrapped at ≤ 72 columns.
     - Trailer: `Co-Authored-By: Gemini <noreply@google.com>` (or appropriate co-author trailer).
   - Merge `step-virtual-syscalls` into `main`.

---

## 5. Reference Files & Artifacts

- **Watchdog Tool:** [`tools/agent_watchdog.py`](file:///home/russ/Sources/dircachefs/tools/agent_watchdog.py) (can be run with `--once` or daemon mode `--interval 10`).
- **Watchdog Unit Tests:** [`tools/agent_watchdog_test.py`](file:///home/russ/Sources/dircachefs/tools/agent_watchdog_test.py).
- **Postmortem on Subagent Monitoring:** `/tmp/dcfs_agent_liveness_postmortem.md`.
- **Maintainer Status File:** `/tmp/dcfs_status.md`.
- **Plan Log:** [`docs/plan/log.md`](file:///home/russ/Sources/dircachefs/docs/plan/log.md).
