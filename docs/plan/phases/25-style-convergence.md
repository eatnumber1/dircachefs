# Phase 25 — Style convergence

`docs/style.md` (russ, 2026-10-07) describes the rules; its Appendix A
counts the sites that break them, with a grep for each. This phase makes
the tree conform. No behavior changes: every step is refactoring under
the existing tests, and a step that finds a bug stops and reports it as
its own test-first step.

- 25.1 Errors and wrappers (after Phase 23 merges: same files). The eight
  `*ErrorBuilder()` helpers in `dcfs/status.h` (each returning
  `absl::StatusBuilder(kCode)` with the caller's `SourceLocation`), the 41
  `absl::XError(...)` constructors and 5 direct `StatusBuilder(kX)` sites
  converted (C1), the 5 `; ;` separators (C2), `absl::string_view`/
  `absl::optional` → `std::` (C5), capitalised messages (C6), the two
  `"open <path>"` messages (C8), the 8 raw libc calls outside
  `syscalls.cc` (C10: through `syscalls::`), the unused include (C14),
  `using namespace` in bench (C16). `syscalls.h` keeps only manpage-named
  thin wrappers: the six `*_opath`, `ReopenPathFd`, `fsuid`/`fsgid`,
  `GetInodeGeneration` move to helpers in `backing.cc` built on plain
  wrappers; `setgroups_thread` → `setgroups`; the retry loops (S2) move
  with their callers; `LogOpenFlags` + `AbslStringify` move out together
  as a hidden friend, namespace `dcfs` unchanged (S1-S3). `enum class`
  for the four nested `enum Kind`s (E1; `LookupResult::Kind` has ~130
  uses). `ASSERT_OK_AND_ASSIGN` once, in `dcfs/testonly/` (C7).
  `third_party/pjdfstest/README.md` (C12). Owner: dcfs-mechanical for the
  conversions, dcfs-implementer for the `syscalls.h` moves; review by
  dcfs-reviewer (the moves touch error paths).
- 25.1b (russ, 2026-10-07; same lane, after 25.1): every syscall in the
  tree goes through `syscalls.h`, tests, testonly, bench and tools
  included ("a firm rule": errors are handled there and become Status;
  russ found `::mkdir` in `dir_cache_fs_test.cc`; the guide's C10 count
  was production-only). A host-side `raw_syscalls_test` fails on any
  unqualified libc syscall call outside `syscalls.cc` (regex over the
  tracked C++; clang-tidy takes over in 7.5); every site converted. The
  one exception: `tools/fhtest.c` and `tools/testutil.c` are C programs
  for the guest and cannot use the C++ wrappers; they stay as they are
  (russ, 2026-10-07: fhtest.c is a hand-synced copy from
  fuse-generation-qemu, third-party; testutil.c stays C beside it).
- 25.2 Flat namespaces (after 25.1). `cache` (673 uses), `backing`
  (147), `events` (182), `internal` (9), `testonly` (4) fold into `dcfs`;
  only `dcfs::syscalls` and `dcfs::sqlite3` stay (russ, 2026-10-07),
  called as `syscalls::x` and `sqlite3::x`. `backing` does not get an
  exception (russ). Default: free functions in `dcfs`, renamed only
  where they clash (`ParentOf`, `SetXattr`, `RemoveXattr` exist in both
  `cache` and `backing`: give the three pairs distinguishing names, e.g.
  `BackingSetXattr`, and leave everything else as is). A wrapper class
  is allowed only if the clashes turn out to be more than those three
  and renaming reads worse (russ: "don't do the wrapper class unless it's
  needed"); say so in the commit if used. Naming keeps the layers apart: `syscalls::` wrappers
  carry their libc names (`setxattr`), everything else is CamelCase
  (`SetXattr`), so those never clash. `events` and `sqlite3`-adjacent
  names get a prefix only where a bare name is ambiguous. Owner:
  dcfs-implementer, mechanical renames with the compiler as the check;
  full presubmit after.
- Formatting, includes and Python line length (C3, C4, C11, C13, F1-F6,
  P1) are Phase 7.6/7.7 (pinned clang-format, buildifier, format_test,
  layering_check, misc-include-cleaner), not this phase: nothing is
  hand-formatted twice.
- Guest-script helper duplication (C15: `cleanup` ×25 into `lib.sh`) is
  a dcfs-mechanical step when a lane is idle; it touches every guest
  script, so after any open branch that edits them.

Done when: Appendix A of `docs/style.md` lists only the Phase 7 items,
and the guide's "Derived from the tree at" line is updated.

## 25.3 Logging levels and error messages (russ, 2026-10-08)

Make `--stderrthreshold`, `--minloglevel` and `--v` useful, with Abseil's
own semantics and nothing on top (`absl/log/log.h`; `LOG_EVERY_N`,
`LOG_EVERY_N_SEC`, `LOG_FIRST_N` where the agent judges a site needs
rate limiting; no blanket rule; no `--log_dir`).
- Default stderr threshold WARNING (russ: errors and warnings visible by
  default); `--stderrthreshold=0` adds INFO; `--minloglevel`, `--v=N`,
  `--vmodule` documented in README "Flags" and the man page.
- FATAL: dcfs cannot continue safely (invariant violation in the checking
  build; a cache whose schema or identity cannot be reconciled).
- ERROR: the caller got an error dcfs produced rather than forwarded from
  the backing, or dcfs refused to do its job (startup refusals, failed
  reply or close, cache-disk I/O error as EIO, a backing change that
  could not be recorded, a failed recovery probe).
- WARNING: nothing failed for the caller but state is degraded or
  surprising (out-of-band change, unclean shutdown recovered, loose cache
  mode, missing kernel capability with a fallback).
- INFO: the lifecycle narrative (start with source/cache/mount/options,
  recovery summary with counts, each sync point with rows cleared and
  duration, shutdown clean/unclean with reason, first backing access
  after an idle period).
- `--v=1`: one line per request that reached the backing and why;
  `--v=2`: every request with its reply; `--v=3`: SQL statements and step
  counts (today's VLOG(2) SQL moves here).
- Style rules (docs/style.md 1.6/1.7): **return a failed Status or log,
  never both** (the caller or its caller logs, with more context;
  logging too duplicates lines with less context); the Status
  error-message rules from russ's "Style Guidelines for Accumulating
  absl::Status Error Messages" (first error carries what was operated
  on, not why; passing through adds what the callee was asked to do, not
  what the enclosing function does; no terminal punctuation; capitalise
  the first error, not added context; not every level adds context),
  rewritten for this codebase's `StatusBuilder` helpers and `ErrnoToStatus`.
- Delivery: style.md 1.6/1.7 and a design.md "Logging" subsection first;
  then reclassify every existing site (about 63) and every "log and
  return" pair, add the INFO lifecycle lines, document the flags; tests
  assert a line's level where behaviour depends on it.

