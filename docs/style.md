# dcfs style guide

This guide **extends the [Google C++ Style
Guide](https://google.github.io/styleguide/cppguide.html)**, and our C++
must conform to it; Python follows the [Google Python Style
Guide](https://google.github.io/styleguide/pyguide.html) the same way (80
columns). A section here says only what adds to or narrows Google's rules,
or covers what Google does not (tests, Bazel, shell, docs, commits). Where
the tree breaks Google style or a rule below, Appendix A counts the sites
and gives a command that finds them, for a later mechanical step. Nothing
here is open: russ settled the questions on 2026-10-07.

Derived from the tree at c632ca4 (2026-10-07). Counts are of production
code (`dcfs/`, `bench/`, no tests) unless stated. File:line references
drift. Each rule is what most of the code does, or is in `AGENTS.md`,
`docs/design.md` or `docs/plan/process.md`, or is russ's decision.

## 1. C++

### 1.1 Enforced mechanically

- **Formatting is enforced, not requested.** clang-format (Google style,
  `.clang-format`: `IndentWidth: 2`, `ColumnLimit: 80`, `PointerAlignment:
  Right`) for C and C++, buildifier for Bazel files. Both are pinned
  through Bazel. `//tools:format_test` (tier `small`) runs them in check
  mode (`clang-format --dry-run -Werror`, `buildifier -mode=check`) over
  the tracked files, so `bazel test --config=fast //...` and CI fail on an
  unformatted file. `bazel run //tools:format` reformats (it replaces
  `tools/format.sh`). An opt-in `.githooks/pre-commit` runs the check
  (`git config core.hooksPath .githooks`). `tools/*.c` are reformatted,
  not excluded. None of this exists yet (Appendix A, F1-F5); it arrives
  with phase 7's LLVM toolchain.
- **Include what you use, enforced**: a target includes only headers of its
  direct deps (`layering_check`) and clang-tidy's `misc-include-cleaner`
  finds missing and unused includes, both with phase 7's clang toolchain
  (F6). Until then, state each direct dep in BUILD and add no include that
  nothing uses.
- Include blocks are what clang-format's Google style produces: own header,
  C headers, C++ headers, then every other header in one sorted block
  (`"absl/..."`, `"dcfs/..."`, `"fuse_lowlevel.h"`).
- Warnings: `-Werror`, `-Wimplicit-fallthrough`, `-Wno-sign-compare`
  today (`.bazelrc`); from phase 7 `-Weverything -Werror` with every
  disabled warning listed in `.bazelrc` with a reason. A suppression
  carries a comment saying why.

### 1.2 Narrowing Google's rules

- **`std::string_view`, `std::optional`**, not the `absl::` aliases (339
  vs 5, 107 vs 4).
- **ADL hooks** (`AbslStringify`, `AbslHashValue`, `operator<<`, `swap`)
  are hidden friends: defined inside the class they belong to, in that
  class's namespace, never in another namespace or file
  (`FileHandle::AbslHashValue`, `file_handle.h:33`).
- **Test-only code** goes in `*_test.cc` or `dcfs/testonly/` (BUILD
  `default_testonly = 1`), never in a production file; production code has
  no test hooks.
- **A module** is `foo.h` + `foo.cc` + `foo_test.cc` and BUILD targets
  `foo` and `foo_test`; header-only modules have a test only if they have
  logic. Unpaired by design: `main.cc`, `errno.cc` (the errno table behind
  `status.h`), `protocol_events_main.cc`. A module's header comment says
  what it is for and which layering rule it obeys (`dcfs/backing.h:27-52`).
- **ASCII only** in code and prose (one micro sign in `docs/design.md`); a
  comment's dash is ` -- ` (219 uses).
- **No exceptions** (0 `throw`/`catch`); no `[[nodiscard]]` (`Status` is
  must-use already); a status dropped on purpose is `.IgnoreError()` (15).
- **Integers**: `int64_t` row ids (`using InodeId = int64_t;`,
  `metadata_cache.h:56`), `uint64_t` node ids, generations and backing
  inode numbers, `size_t` sizes, `off_t` offsets, `int` for descriptors,
  flags and errnos.
- **Ownership**: an owned descriptor is a `FileDescriptor` (`dcfs/fd.h`), a
  borrowed one an `int fd` parameter; scope-exit work is `absl::Cleanup`
  (6 uses). In `dcfs/` only `syscalls.cc` calls `close()` by hand.
- `TODO(topic): ...` names what the work waits for (4 uses, all
  `TODO(coroutines)`).

### 1.3 Namespaces

All code is in `namespace dcfs`, flat: no nested namespaces, with one
exception, `dcfs::syscalls`, whose wrappers keep their libc names and so
need the qualifier. Call them as `syscalls::open(...)`, never
`dcfs::syscalls::open` and never with a `using`. The other sub-namespaces
(`cache`, `backing`, `testonly`, `sqlite3`, `events`, `internal`) go: name
things so that a flat `dcfs::` stays unambiguous (N1-N6).

### 1.4 Enums

`enum class`, nested inside the type it belongs to when it belongs to one
(`struct LookupResult { enum class Kind { kFound, ... }; Kind kind; };`),
converted when touched. The four nested unscoped `enum Kind`s convert
(E1).

### 1.5 `syscalls.h`

One thin wrapper per documented Linux syscall or libc call, named for its
manpage (`man 2 openat`, `man 3 ...`), returning `absl::Status` or
`StatusOr` through `dcfs::ErrnoToStatus`. One call, no composition (no
retry loops, no decoding into containers, no policy). Anything else is a
helper in `backing.cc` built on the plain wrappers: the `/proc/self/fd/N`
trick is a backing helper calling `syscalls::getxattr(path, ...)`. A
wrapper that is thin but has a non-manpage name is renamed to its manpage
name, with a comment on any per-thread raw-syscall detail
(`setgroups_thread` becomes `setgroups`). Nothing that is not a wrapper
lives in `dcfs::syscalls`, so no ADL hook can be found through it: checked
by grep, the only template there is `ioctl`, and the one hook in the file
(`LogOpenFlags`/`AbslStringify`) is at `dcfs` scope (`syscalls.h` closes
`namespace syscalls` at line 181 before it). Current exceptions: S1-S3.

### 1.6 Errors

**`absl::Status`/`StatusOr<T>` are the only error channel**: no error
codes, no `errno` outside the syscall wrappers. `std::optional<T>` means
"absent is normal"; `StatusOr<std::optional<T>>` when it can also fail.

**Propagate with Abseil's macros** (`absl/status/status_macros.h`):
`ABSL_RETURN_IF_ERROR` (260 uses) and `ABSL_ASSIGN_OR_RETURN` (317). The
unprefixed aliases exist in the pin (`status_macros.h:477`) and have 0
uses: write the `ABSL_` form. The tree defines no propagation macros. Its
own macros are `RET_CHECK`, `RET_CHECK_EQ/NE/GT/OK` (`dcfs/ret_check.h`, 68
uses): they return a `kInternal` `StatusBuilder` and take `<<` context. Use
`RET_CHECK` where a `Status` can be returned; use `CHECK`/`CHECK_NE`
(`absl/log/check.h`) where it cannot (a libfuse callback, startup) and the
failure is a programming error or unrecoverable (`fuse_ops.cc:27`,
`backing.cc:118`). Never `assert`.

**Build a status one of three ways (russ: StatusBuilder, not `StrCat`).**

1. A failed syscall or libc call: `dcfs::ErrnoToStatus(errno, what)`,
   immediately after the call, never `absl::ErrnoToStatus`: only the dcfs
   wrapper attaches the `kErrnoTypeUrl` payload that `GetErrnoFromStatus`
   and `StatusToErrno` read (`status.h:15-19`). An error the kernel should
   see as a given errno is built the same way (`ErrnoToStatus(ENOTDIR,
   "the source is not a directory")`, `backing.cc:462`).
2. Any other new error: a builder helper from `dcfs/status.h`. Abseil has no
   `absl::InternalErrorBuilder` and the like, so `dcfs/status.h` provides
   `InternalErrorBuilder()`, `FailedPreconditionErrorBuilder()`,
   `NotFoundErrorBuilder()`, `InvalidArgumentErrorBuilder()`,
   `AbortedErrorBuilder()`, `UnimplementedErrorBuilder()`,
   `AlreadyExistsErrorBuilder()` and `ResourceExhaustedErrorBuilder()`, each
   returning `absl::StatusBuilder(absl::StatusCode::kX, loc)` with `loc` a
   defaulted `absl::SourceLocation::current()` parameter, so the status
   records the caller's line:

   ```
   return InternalErrorBuilder()
          << "fuse_session_mount(" << mountpoint << ") failed";
   ```

   Not `absl::InternalError(absl::StrCat(...))` or any `absl::XError(...)`
   in production code. A code with no helper uses
   `absl::StatusBuilder(absl::StatusCode::kX)` directly. (The helpers do
   not exist yet: C1.)
3. Context on a status from elsewhere: `absl::StatusBuilder(status) <<
   "..."` or `ABSL_RETURN_IF_ERROR(expr) << "..."` (tested,
   `status_test.cc:94`; no production use yet).

**StatusBuilder keeps the errno payload.** Verified three ways. The header:
`StatusBuilder(const Status &)` wraps the original status and `SetPayload`
only adds (`status_builder.h:162-175`, `:290-297`). The source: the built
result is made by `StatusRep::Clone(message, include_payloads=true, ...)`
(`status_builder.cc` `SetMessage`; `internal/status_internal.cc:216`). The
tests: `ErrnoPayloadTest.SurvivesStatusBuilderAnnotation` and
`...SurvivesAbslReturnIfErrorAnnotation` (`status_test.cc:86,94`) recover
`ENOENT` from an annotated `ErrnoToStatus`. So add context to an errno
status with `StatusBuilder`, never by rebuilding it with
`absl::Status(code, message)` (that drops the payload and `StatusToErrno`
falls back to its code table). Not verified: `.SetCode()` on an errno
status (nothing does it). The streamed text is joined as `original;
added`, so start it with words, not `;` or a space (C2).

**Codes** (production constructor counts):
- `kFailedPrecondition` (16): a startup check failed (`main.cc:144,186,300`),
  the cache database is corrupt or belongs to another filesystem
  (`migrate.cc:54,75,301`, `backing.cc:483`), an object used in a state it
  cannot be (`sqlite.cc:335`).
- `kNotFound` (10): the cache has no such row; callers use
  `absl::IsNotFound` (`metadata_cache.cc:88`, `mount_fds.cc:25`).
- `kInternal` (5, and every `RET_CHECK`): a broken invariant, or a libfuse
  call that failed without an errno (`main.cc:483-531`): a bug in dcfs.
- `kInvalidArgument` (4): malformed input (`file_handle.cc:83`).
  `kAborted` (2): the inode changed since the mutation resolved it; the
  caller retries three times, then `EAGAIN` (`metadata_cache.cc:1413`,
  `dir_cache_fs.cc:824-831`). `kUnimplemented`, `kAlreadyExists`,
  `kResourceExhausted`: as named.
- A status that reaches a FUSE reply and means something to the client
  carries an errno (item 1); without one `StatusToErrno` uses its code
  table (`kInternal` becomes `ELIBBAD`, `kFailedPrecondition` `EBUSY`): a
  diagnostic, not an answer.

**Messages.** Say what failed and with what, in lower case unless the
first word is an identifier (`DeviceId::Parse: expected a 24-byte value`,
`no cached inode 7`), no trailing period. A syscall failure names the call
and the arguments that identify the object: `openat(%d, <name>)`
(`syscalls.cc:39`); a bare name when every argument is a descriptor
(`"fstat"`: 41 sites; call form 16). A name, symlink target or xattr name
from the backing filesystem goes through `EscapeBytes` (`dcfs/escape.h`) in
every message and log line (35 uses; `backing.cc:387`; `docs/design.md`,
"File names are bytes"). Paths dcfs builds (`/proc/self/fd/N`) and flag
values are text.

### 1.7 Logging

Abseil logging. `main.cc:240-246` sets the stderr threshold to WARNING
before flag parsing. There is no syslog today (0 uses; a daemonised dcfs's
stderr goes to `/dev/null`); phase 15 (the wrapper and daemonisation) adds
it.
- `LOG(ERROR)`: dcfs refuses something or the environment is unusable: a
  kernel capability missing (`dir_cache_fs.cc:184`), an inode number
  refused (`backing.cc:343`), a reply or close failed (`fuse_request.cc:222`,
  `fd.cc:17`).
- `LOG(WARNING)`: the cache survives but an operator should know: an
  out-of-band change on the backing filesystem (`backing.cc:583`), the
  backing change happened but recording it failed (`dir_cache_fs.cc:353`),
  recovery after an unclean shutdown (`backing.cc:1683`), a loose cache
  mode (`main.cc:202`).
- `LOG(INFO)` (2 uses): startup facts. `VLOG(1)`: per-request decisions
  (README: `--v=1`); `VLOG(2)`: SQL (`sqlite.cc:204`).

### 1.8 The dcfs invariants as code rules

The review checklist of `docs/plan/process.md`; the reviewer greps for
each.
- **`Context &ctx` first** in every function that reads or writes the
  cache database, mount table, fill guards or dirty set (`const Context &`
  to only read, `cache::BeginFill`). Backing helpers that touch only a
  descriptor take none (`backing::ReadFile(int fd, ...)`). Exception: the
  protocol-event scopes take the recorder first (`protocol_events.h:381`).
- **No globals, singletons or `thread_local`** (0 of each). Shared state
  lives in `Context` (`context.h:84`) or in an object the caller owns.
- **No paths after startup.** Objects are reached by descriptor or file
  handle (`FileHandle::Open`); a child is `openat(dir_fd, name, ...)`, never
  a joined path. Only startup uses a path (`--source`, the cache database).
- **Only `backing.cc` touches the backing filesystem for request work.**
  `DirCacheFS` and `cache::` never name `syscalls::`. The allowed peers are
  `file_handle.cc`, `device_id.cc`, `fd.cc` and startup in `main.cc`
  (`docs/design.md`, "Architecture and layering"); `mounts_below.cc` is not
  listed (C9). Only `syscalls.cc` calls libc directly (C10). `cache::` is
  pure SQLite: it never sees a descriptor.
- **No transaction spans a backing syscall.** Backing I/O first, then one
  short synchronous transaction (`ctx.db.Transaction(...)`); no statement
  cursor held across a syscall; `AsCaller` wraps one syscall.
- **Tri-state records.** A record mirroring the backing filesystem is
  present, absent or unknown; a mutation sets it unknown before the
  backing syscall and present or absent after; a failure leaves it unknown.
- **A protocol change updates the TLA+ model** in the same change
  (section 6).
- **Names are bytes**: `std::string`/`string_view`, `char *` only at the
  syscall and libfuse boundary, `EscapeBytes` when printed, `memcmp` order.
- **Requests are cancellable** (from phase 22): an operation over about
  100 ms checks for interruption at safe points and has a cancellation
  test.

### 1.9 Comments

Google asks for comments on non-obvious declarations; the norm here is
more: about one line in four (3,706 of 15,580), saying why, not what.
- Code added for a reason names its source: the plan step (`step 23.2`,
  `dir_cache_fs.cc:532`), the audit finding (`audit-tristate F1`,
  `context.h:37`), the conformance case ("found via pjdfstest's chmod/02.t:
  a name one byte past NAME_MAX got ENOENT ... instead of ENAMETOOLONG",
  `backing.cc:1185`), the model step (`// Model: UnlinkPhase1's ENOENT
  branch`, `dir_cache_fs.cc:836`) or the commit (`fuse_request_channel_
  test.cc:1`: "Regression test for 0795d12").
- Length: one to three lines for a block, up to about ten for a non-local
  invariant; longer reasoning goes in `docs/design.md` and the comment
  points to it. No restating the code, no changelog ("fixed in step N" is
  the commit's).
- `/* */` only for argument names (`/*is_dir=*/false`) and in `tools/*.c`.

## 2. Tests

- **Test first.** Write the test, run it on the unchanged code, quote the
  failure in the commit message, then fix. Never weaken a test to make it
  faster or pass (`AGENTS.md`).
- **Every test of dcfs runs in a QEMU guest as root**: `qemu_cc_test` for
  GoogleTest binaries, `qemu_test`/`qemu_test_matrix` for guest scripts
  (`test/qemu/README.md`). Host-side tests need neither root nor a kernel
  (Python, shell smoke tests of pinned tools, TLC, `//tools:format_test`).
- **GoogleTest.** `TEST_F(<Subject>Test, ...)` with a fixture (220 of 299
  tests; plain `TEST` 79); a `PascalCase` sentence for the behavior
  (`RecycledInodeNumberGetsANewRow`). No `DISABLED_` (0 uses): fix or
  delete. Check with `ASSERT_THAT(status, IsOk())`, `IsOkAndHolds`,
  `StatusIs` (`absl/status/status_matchers.h`) and `ASSERT_OK_AND_ASSIGN`;
  `ASSERT_*` when the test cannot continue, `EXPECT_*` otherwise.
  `GTEST_SKIP() << reason` when the guest filesystem lacks a capability (23
  uses).
- **Fakes, not mocks**: no `MOCK_METHOD` (0 uses; matchers are fine). A
  dependency is injected and a small fake stands in (`ProtocolEvents`,
  `FakeChannel` in `fuse_request_channel_test.cc`). Syscall faults are
  link-time `-Wl,--wrap=<symbol>` fakes in a test target of their own, so
  the wrap affects no other test (`dcfs/BUILD.bazel:225-246`).
- **The forged-request harness** (`dir_cache_fs_test.cc`,
  `fuse_request_channel_test.cc`): a real libfuse session with
  `fuse_session_custom_io()` fed hand-built requests through
  `fuse_session_process_buf()`: a real `FuseRequest`, no kernel mount. Use
  it for request-layer behavior and the interleavings coroutines will
  produce.
- **Guest scripts** (`test/qemu/guest/*.sh`, section 4): one line per check,
  `TEST <name> PASS`, `TEST <name> FAIL (<why>)` or `TEST <name> SKIP
  (<why>)` (`lib.sh:17-19`; names lower case with hyphens: 768 vs 0
  underscores); a FAIL sets `FAILED=1`; the script ends `exit "$FAILED"`.
  There is no `DISABLED` state. Shared helpers are in `lib.sh`
  (`start_daemon`, `restart_daemon`, `drop_caches_quiesced`,
  `backing_fstype`, `sectors_read`). "Served from the cache" is checked by
  dropping every kernel cache and comparing the backing device's sectors
  read before and after (`check_cold`, `write.sh:101`; `warm-metadata-vdb`
  in `copy.sh`). A test about filesystem-dependent behavior is a
  `qemu_test_matrix` (ext4, xfs, btrfs) and branches on `backing_fstype`
  only for a genuine per-filesystem difference.
- **Tiers**: Bazel `size`. `small` (about a minute in all,
  `--config=fast`), `medium` (`--config=presubmit`), `large`/`enormous`
  (CI). Every test declares `size` and `timeout` (the QEMU macros refuse a
  missing one); pick the tier from the measured duration. The soak test is
  `manual`.
- New code arrives covered, error paths included (from phase 7/8).

## 3. Bazel

- Targets are `snake_case` after the file stem (`metadata_cache`,
  `metadata_cache_test`); a matrix test generates `<name>_ext4`, `_xfs`,
  `_btrfs`. Load rules explicitly (`dcfs/BUILD.bazel:1-3`). `testonly = 1`
  on test-only libraries.
- Comment a target when its name and attributes do not say why it exists
  (about 64 of 149 targets have a comment; 25 name a plan step). Always
  comment a non-obvious flag (`copts = ["-O2"]`, `dcfs/BUILD.bazel:119-121`),
  a `-Wl,--wrap`, and a list shared by several targets (`MAIN_DEPS`). An
  obvious `cc_library` needs none; a plan-step reference helps.
- Dependency lists: one per line, sorted (`:local`, `//`, `@`), elements
  indented 4 spaces (buildifier, enforced by `//tools:format_test`).
- `third_party/<name>/` holds only what we write for a pin (BUILD overlay,
  patches, config, lock files) and a `README.md` with a "Pin" section
  (version, URL, sha256, how the sha256 was obtained, date) and an
  "Updating the pin" procedure (`third_party/busybox/README.md`).
  Third-party code is fetched by Bazel: the Bazel Central Registry
  (`bazel_dep`), else `http_archive`/`http_file` with a sha256.
- `MODULE.bazel`: a comment above a pin says what it is for and where its
  README is (7 of 13 `http_archive`s, 4 of 14 `bazel_dep`s; the others are
  self-evident). `MODULE.bazel.lock` is never rewritten silently
  (`--lockfile_mode=error`). `.bazelrc` flags carry the reason.

## 4. Shell

Google's shell guide is not adopted: this is the repository's own rule.
- Two dialects, by where the script runs.
  - POSIX `sh` (`#!/bin/sh`, 55 scripts): all guest scripts (busybox ash:
    `test/qemu/guest/*`, `guest/init`), `third_party/*` build and smoke
    helpers, `test/qemu/scripts/`. No `[[`, arrays, `local` or `function`;
    `[ ]`, `$(...)` (never backticks), `printf`.
  - bash (`#!/bin/bash`, 7 scripts): `.github/ci/*`,
    `formal/trace_validate.sh`, `tools/tool_identity_test.sh`; `set -euo
    pipefail`, `[[ ]]` allowed.
- Host-side scripts begin `set -eu` (bash: `-euo pipefail`; 25 scripts).
  **Guest test scripts do not use `set -e`**: a failed check is a `TEST ...
  FAIL` line and the script goes on. They set `FAILED=0`, source `lib.sh`,
  `trap cleanup EXIT` (dumping the daemon log when something failed) and
  `exit "$FAILED"`.
- A comment right after the shebang says what the script does and how it
  runs (63 of 63 scripts). Comments say why, as in C++.
- Indent with tabs (48 scripts; 5 use spaces, C11). Quote every expansion
  (`"$SRC"`, `"$@"`). Check names are lower case with hyphens, functions
  with underscores. The zsh caveats belong to `CLAUDE.md`, not scripts.

## 5. Python

- Google Python Style Guide, 80 columns; a formatter pinned through Bazel
  joins `//tools:format_test` in phase 7. Only `tools/` and `man/` use
  Python (6 files): Python 3.12 from the hermetic `rules_python`
  toolchain, standard library only.
- A module docstring says what the file does and its plan step
  (`tools/sbom/sbom.py:1`); `argparse` for a command line; `main(argv)`
  returns the exit code, called as `sys.exit(main(sys.argv[1:]))`.
- Tests are `unittest` classes in `<name>_test.py`, built as `py_test` with
  an explicit `size`, data passed as `$(location ...)` arguments
  (`tools/sbom/BUILD.bazel`). They run on the host.

## 6. TLA+

`formal/README.md` ("Changing the model", "Trace validation") has the
conventions; follow it. In short: a protocol change edits the step in
`formal/dcfs.tla` that stands for the code and keeps its comment naming the
dcfs function; a fixed bug the model can express gets a `known_bugs/`
variant whose test expects the counterexample; a changed step gets its
protocol event (`dcfs/protocol_events.h`) and `Trace.tla` action;
`bazel test //formal/...` passes. Comments are boxed `(* ... *)` blocks
(`formal/dcfs.tla:1-20`).

## 7. Docs and commits

- **Where things go.** Decisions and reasons: `docs/design.md`. User-facing
  behavior and limitations: `README.md` (also the source of the `dcfs(8)`
  man page). Test usage: `test/qemu/README.md`. A pin: its
  `third_party/<name>/README.md`. Plan, log, status: `docs/plan/` (the
  orchestrator edits it; an agent reports what it needs).
- **Prose.** Full sentences, plain and specific, no marketing; say what the
  program does and does not do. Measured numbers carry units, date and
  environment ("about 1 minute (KVM)", "released 2026-05-13; the latest
  stable release as of 2026-10-05"). A decision names its maker and date
  ("(russ, 2026-10-06)"). Markdown wraps at 80 columns (tables and URLs
  excepted); headings are sentence case; a list item stating a rule may
  open with it in bold (`docs/design.md`, "Architecture and layering").
- **Commit subject**: `N.M: area: what changed` for a plan step (`23.4:
  copy_file_range, the ioctl allowlist and O_TMPFILE`); `plan:`, `notes:`
  and `agents:` for orchestrator edits, notes and agent instructions. No
  trailing period; about 72 columns (62 of 323 are longer).
- **Commit body**: wrapped at 72, says why. A bug fix or behavior change
  quotes the failing-first run under a "Failing first" heading (about 30
  bodies), then the model and test results. It ends with the model's
  `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>` trailer
  (history: Opus 5.5 x103, Sonnet 5 x90, Fable 5.1 x82, Sonnet 5.5 x34,
  Haiku 4.5 x5). Never push.

## Appendix A: Convergence

Sites that break Google style or a rule above, as of c632ca4. Run each
command from the repository root in bash. Rows marked (new) come from the
2026-10-07 decisions. Google rules surveyed: formatting, includes,
`using namespace`, `typedef`, `thread_local`, exceptions, casts, naming,
header guards, `explicit` constructors, macros; others (and the Google
Python rules beyond line length) were not.

| # | Rule | Count | Find them |
|---|---|---|---|
| C1 | Errors via `dcfs::XErrorBuilder()` (1.6) (new) | The 8 helpers do not exist: add them to `dcfs/status.h` (with a `SourceLocation` default argument), then convert 41 `absl::XError(...)` constructors in 9 files (16 FailedPrecondition, 10 NotFound, 5 Internal, 4 InvalidArgument, 2 AlreadyExists, 2 Aborted, 1 Unimplemented, 1 ResourceExhausted; Internal at `main.cc:483,488,494,531`, `sqlite.cc:91`) and 5 direct `StatusBuilder(kX)` sites (`errno.cc:294`, `sqlite.cc:376`, `ret_check.h:33,75,86`) | `grep -rnE 'absl::[A-Za-z]+Error\(\|StatusBuilder\(absl::StatusCode' dcfs bench --include='*.cc' --include='*.h' \| grep -v -e _test.cc -e testonly` |
| C2 | No leading `;` or space in StatusBuilder text | 5: `main.cc:285`, `sqlite.cc:127,311,351,412` (they print `; ; while ...`) | `grep -rn -A2 'StatusBuilder(' dcfs --include='*.cc' \| grep -E '<< "(; \| \()'` |
| C3 | Google: 80 columns | 102 lines in 28 files (`backing.cc` 14, `syscalls.cc` 10, `metadata_cache_test.cc` 8) | `grep -rnE '^.{81,}$' dcfs bench tools --include='*.cc' --include='*.h' --include='*.c'` |
| C4 | Google/clang-format include blocks and order | 16 out-of-order lines in 10 files (`backing.cc`, `errno.cc`, `fd.cc`, `main.cc`, `syscalls.cc`, `syscalls.h`, 4 tests); 37 files have one `<...>` block where Google has C and C++ headers apart (26 files have two or more); clang-format settles both | `LC_ALL=C awk 'FNR==1{p=""} /^#include/{if(p!=""&&$0<p)print FILENAME":"FNR": "$0;p=$0;next}{p=""}' $(git ls-files 'dcfs/*.cc' 'dcfs/*.h')` |
| C5 | `std::` not `absl::` aliases | 9: `main.cc:96,214,461`, `status.h:19`, `status.cc:10,19`, `file_handle.cc:62,145,146` | `grep -rnE 'absl::(string_view\|optional)' dcfs bench --include='*.cc' --include='*.h' \| grep -v _test.cc` |
| C6 | Message starts lower case unless an identifier | 5: `mount_fds.cc:25`, `sqlite.cc:84,91,133`, `status.cc:21` | `grep -rnE 'Error\($' -A2 dcfs --include='*.cc' \| grep -E '"(No\|Cannot\|Malformed\|Extra) '` |
| C7 | `ASSERT_OK_AND_ASSIGN` defined once | defined in 7 test files (`metadata_cache_test.cc:36` and six more; the pinned `status_matchers.h` lacks it) | `grep -rln 'define ASSERT_OK_AND_ASSIGN' dcfs` |
| C8 | Syscall message: `name(args)` or bare name | 2: `main.cc:191,323` | `grep -rnE 'ErrnoToStatus\(.*"[a-z_0-9]+ "' dcfs --include='*.cc'` |
| C9 | `syscalls::` only in `backing.cc` and the listed peers | `mounts_below.cc:77,95` is not in `docs/design.md`'s list (add it there or route through `backing.cc`) | `grep -rln 'syscalls::' dcfs --include='*.cc' --include='*.h' \| grep -v -e _test -e testonly` |
| C10 | Raw libc only in `syscalls.cc` | 8: `device_id.cc:126,140,144,148,150`, `main.cc:180,321,325` | `grep -nE '(^\|[^_a-zA-Z:.])(::)?(ioctl\|fstatfs\|open\|close\|stat\|mkdir)\(' dcfs/device_id.cc dcfs/main.cc` |
| C11 | Shell indented with tabs | 5 scripts with spaces: `formal/trace_validate.sh`, `tools/smoke_readonly.sh`, `tools/tool_identity_test.sh`, `tools/format.sh`, `.github/ci/osv.sh` (1 line) | `grep -lP '^ +\S' $(git ls-files '*.sh')` |
| C12 | Every `third_party/<name>/` has a README with the pin | 1: `pjdfstest` (pin only in `MODULE.bazel:21-26`) | `for d in third_party/*/; do [ -f $d/README.md ] \|\| echo $d; done` |
| C13 | BUILD list elements indented 4 (buildifier) | 292 lines at 6 spaces, all in `dcfs/BUILD.bazel` | `grep -cP '^      \S' dcfs/BUILD.bazel` |
| C14 | No unused include | `syscalls.h:21` (`absl/base/nullability.h`); others need F6 | `grep -n 'nullability\|absl_nonnull' dcfs/syscalls.h dcfs/syscalls.cc` |
| C15 | Guest helpers shared in `lib.sh` | duplicated: `cleanup` 25, `normalize_stat` 5, `populate_tree`/`run_pass`/`expect_fail` 4 each, `start_daemon` 3, six more 2 each | `grep -hE '^[a-z_]+\(\) \{' test/qemu/guest/*.sh \| sort \| uniq -c \| sort -rn` |
| C16 | Google: no using-directives | 1: `bench/dcfs_bench.cc:396` (`using namespace dcfs_bench;`) | `grep -rn 'using namespace' dcfs bench tools` |
| P1 | Google Python: 80 columns (5) | 50 lines over 80: `sbom.py` 25, `sbom_test.py` 22, `tool_keys_test.py` 3 | `grep -nE '^.{81,}$' $(git ls-files '*.py')` |
| N1 | Flat `dcfs`: remove `dcfs::cache` (1.3) (new) | 3 declarations (`metadata_cache.h/.cc/_test.cc`); 673 `cache::` uses (407 production) in 20 files. Clash if flattened: `ParentOf` (same parameters as `backing::ParentOf`, differing only in return type), `SetXattr`, `RemoveXattr` all also exist in `backing` (3 names: rename one side first) | `grep -rn 'namespace cache\|cache::' dcfs bench \| wc -l` |
| N2 | Remove `dcfs::backing` | 3 declarations; 147 uses (138 production) in 15 files; clashes: the same 3 names as N1 | `grep -rn 'namespace backing\|backing::' dcfs bench \| wc -l` |
| N3 | Remove `dcfs::testonly` | 8 declarations (all in `dcfs/testonly/`); 4 uses | `grep -rn 'namespace testonly\|testonly::' dcfs bench` |
| N4 | Remove `dcfs::sqlite3` | 3 declarations (`sqlite.h/.cc`, `sqlite_test.cc`); 104 uses (62 production) in 13 files; generic names become dcfs-wide (`Connection`, `Statement`, `ConnectionFactory`, `Durability`); no clash found; `sqlite3` is also the C library's struct tag | `grep -rn 'namespace sqlite3\|sqlite3::' dcfs bench \| wc -l` |
| N5 | Remove `dcfs::events` | 2 declarations (`protocol_events.h`); 182 uses (164 production) in 7 files; generic names (`Request`, `Op`, `Ino`) become dcfs-wide; no clash found | `grep -rn 'namespace events\|events::' dcfs bench \| wc -l` |
| N6 | Remove `dcfs::internal` | 2 declarations (`ret_check.h`, `sqlite.h`); 9 uses; helpers would need distinct names (`RetCheck*`, `IsOptional`) | `grep -rn 'namespace internal\|internal::' dcfs` |
| N7 | `syscalls::` never `dcfs::syscalls::` or `using` | 0 code sites (2 in comments: `backing.h:33`, `syscalls_fault_test.cc:81`); the 190 `syscalls::` uses (99 production) are fine | `grep -rn 'dcfs::syscalls::\|using .*syscalls' dcfs bench` |
| E1 | `enum class`, nested (1.4) (new) | 4 unscoped nested `enum Kind`: `LookupResult::Kind` (`metadata_cache.h:92`, about 130 `LookupResult::k*` uses), `Probe::Kind` (`protocol_events.h:110`), `Frame::Kind` and `Mapping::Kind` (`trace_recorder.h:161,190`); enumerators become `Type::Kind::kX` | `grep -rn '^\s*enum [A-Z]' dcfs` |
| S1 | `syscalls.h` holds manpage-named thin wrappers only (1.5) (new) | not named for a manpage: `listxattr_opath`, `getxattr_opath`, `setxattr_opath`, `removexattr_opath`, `fchmod_opath`, `futimens_opath` (`syscalls.h:92-120`), `ReopenPathFd` (:84), `setgroups_thread` (:176; rename `setgroups`, with a comment on the per-thread raw syscall), `fsuid`/`fsgid` getters (:164-165), `GetInodeGeneration` (:183, outside `syscalls`). Each moves to a `backing.cc` helper over the plain wrappers, or is renamed | `grep -nE '^[A-Za-z].*(_opath\|ReopenPathFd\|GetInodeGeneration\|setgroups_thread\|fsuid\|fsgid)' dcfs/syscalls.h` |
| S2 | No composition in a wrapper | 4 wrappers loop and decode: `flistxattr`, `fgetxattr` (`syscalls.cc:168,196`, 2 attempts) and the `_opath` pair (:287,306, 4 attempts); the retry moves to backing | `grep -n 'attempt <' dcfs/syscalls.cc` |
| S3 | ADL hooks are hidden friends in their class's namespace (1.2) (new) | 1: `LogOpenFlags` and its `AbslStringify` (`syscalls.h:187-190,209`: friend declared in the class, defined out of line). They leave `syscalls.h` together, the hook defined in the class as a hidden friend, namespace unchanged (`dcfs`) so ADL still finds it. Already conforming: `AbslHashValue` in `file_handle.h:33`, `device_id.h:32`. Nothing in `dcfs::syscalls` defines a hook (only the `ioctl` template), so the `syscalls::name` rule and a flattening cannot break ADL | `grep -rnE 'AbslStringify\|AbslHashValue\|operator<<\|swap\(' dcfs --include='*.h' --include='*.cc' \| grep -v _test.cc` |
| F1 | `//tools:format_test` (small) in check mode (1.1) (new) | does not exist | `bazel query //tools:format_test` |
| F2 | Pinned clang-format and buildifier | not in `MODULE.bazel`; neither is on the host (phase 7 LLVM toolchain) | `grep -n 'clang\|buildifier' MODULE.bazel` |
| F3 | `bazel run //tools:format` | `tools/format.sh` is a host script that skips missing tools | `cat tools/format.sh` |
| F4 | `.githooks/pre-commit` (opt-in) | does not exist | `ls .githooks` |
| F5 | `tools/*.c` reformatted | 2,537 tab-indented lines (`fhtest.c`, `testutil.c`) | `grep -lP '\t' tools/*.c` |
| F6 | `layering_check` and `misc-include-cleaner` | no `.clang-tidy`, no `layering_check` in `.bazelrc` (phase 7) | `ls .clang-tidy; grep -n layering .bazelrc` |
