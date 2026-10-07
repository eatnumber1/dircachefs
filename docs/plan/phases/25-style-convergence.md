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
