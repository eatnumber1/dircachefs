# How dcfs enforces style and code quality

This document is for an engineer deciding whether dcfs is, or is on its way
to being, code that stays maintainable. It describes what the repository
enforces **today**, how each rule is enforced, how much of the rule set that
covers, and what is still done by a reviewer's attention or not done at all.
It is written so that each claim can be checked: every claim names a file, a
Bazel target, a CI job or a plan step, and the last section gives the
commands.

It does not restate the rules. [`docs/style.md`](style.md) is the rulebook,
[`AGENTS.md`](../AGENTS.md) holds the rules every contributor follows, and
[`docs/design.md`](design.md) holds the design decisions and their reasons.
How the code is shown to be correct (test tiers, the TLA+ model, crash tests,
differential tests, coverage in depth) is the subject of
[`docs/testing.md`](testing.md) (written on a sibling branch that merges with
this one); this document touches it only where it is a maintainability matter
(dead code, untested branches).

Numbers were counted from the tree at commit `ac3374df` (2026-10-18) unless a
number says otherwise. Line numbers cited in other files are at that commit
and drift. The plan, [`docs/plan/README.md`](plan/README.md), is
where a step number such as 25.39 is defined; steps are cited as "25.39" and
live in [`docs/plan/phases/`](plan/phases/).

## Contents

- [Summary](#summary)
- [1. Philosophy, and where it is
  written](#1-philosophy-and-where-it-is-written)
- [2. Compiler gates](#2-compiler-gates)
  - [What is enforced](#what-is-enforced)
  - [What is not enforced yet](#what-is-not-enforced-yet)
- [3. clang-tidy, clang-query and the allowlist
  backlog](#3-clang-tidy-clang-query-and-the-allowlist-backlog)
  - [How it runs](#how-it-runs)
  - [The check set](#the-check-set)
  - [The clang-query matchers](#the-clang-query-matchers)
  - [The allowlist backlog and its trend](#the-allowlist-backlog-and-its-trend)
- [4. Formatting](#4-formatting)
- [5. `tools/repo_shape.py`](#5-toolsrepo_shapepy)
- [6. Banned symbols in the shipped
  binary](#6-banned-symbols-in-the-shipped-binary)
- [7. Error handling and logging conventions, and how they are
  held](#7-error-handling-and-logging-conventions-and-how-they-are-held)
- [8. The syscall layer](#8-the-syscall-layer)
- [9. Dependencies and the build](#9-dependencies-and-the-build)
- [10. Review process and the findings
  ledger](#10-review-process-and-the-findings-ledger)
- [11. Commit discipline and the design
  history](#11-commit-discipline-and-the-design-history)
- [12. Documentation conventions](#12-documentation-conventions)
- [13. Coverage and mutation, as a maintainability
  matter](#13-coverage-and-mutation-as-a-maintainability-matter)
- [14. Known debt and open work](#14-known-debt-and-open-work)
- [15. Metrics](#15-metrics)
- [16. How to check these claims
  yourself](#16-how-to-check-these-claims-yourself)
- [Where `docs/style.md` and other documents run ahead of the
  tree](#where-docsstylemd-and-other-documents-run-ahead-of-the-tree)
- [Appendix: the rules, by mechanism](#appendix-the-rules-by-mechanism)

## Summary

The honest short version.

- **What is strong.** Most of the structural rules a code reviewer would
  otherwise check by eye are checked by the build graph instead: formatting,
  a clang-tidy set of ten check groups run as errors, eight project-specific
  AST matchers, a ban list of 69 symbols applied to the shipped binary, a
  build-graph rule that confines backing-filesystem syscalls to one layer,
  twelve repository-shape rules, and a commit-subject gate. A gate is required
  to carry a self-check that feeds it known-bad input (step 26.1); the style
  gates under `tools/` mostly do (eight `*_self_check_test` targets, and a
  bad and a good fixture for each matcher). The clang-tidy and repo_shape
  allowlists of known violations are keyed by file, function and check (or
  by count per file), so an unrelated edit does not disturb them, and their
  tests fail both when a new violation appears and when a listed one is fixed
  but not removed. That the lists do not grow is a review policy, not a check
  (section 3).
- **What is weaker than the documents suggest.** The compiler gate is
  `-Werror` plus one extra warning, not the `-Weverything` that `AGENTS.md`
  and `docs/style.md` describe as the goal (step 7.3, not started). The
  clang-tidy backlog is 558 findings in 330 allowlist keys, and a further 496
  report-only matcher findings are not enforced. Formatting is enforced for
  C and C++ only: Bazel files, shell, Python and Markdown are not under a
  pinned formatter. Several `docs/style.md` rules say "mechanically: ..." for
  a check that is planned, not built (the section "Where `docs/style.md`
  and other documents run ahead of the tree" lists them).
- **What is review only.** The rules about comments, naming, reachable
  causes, one abstraction over variants, test-first, and the contract
  citations. The review is structured (a style reviewer on every diff, a
  findings ledger) and a plan step (25.39) aims to mechanize every remaining
  rule, but today these rules depend on a reviewer. Step 25.39 estimates
  that about 60 % of should-or-blocker review findings are style, docs,
  comment or citation findings. The ledger at `ac3374df` gives 40 of 79
  (51 %) with docs and 35 of 79 (44 %) without, which is evidence of where
  the gaps are.
- **By count.** Of 59 rule groups distinguished (Appendix), 28 (47 %) are
  enforced by something that fails, 3 are report-only and 28 are review only
  or not done. The share moves with how the rules are grouped; a narrower
  grouping of 50 rows gave 56 %. Of 176 review findings in the ledger, 68
  (39 %) were marked as checkable by a tool.
- **How to read the rest.** "Enforced" below means a test, build action or CI
  job fails when the rule is broken. "Report-only" means a tool prints
  findings and nothing fails. "Review" means a person (or a reviewer agent)
  is expected to notice. "Planned" means a plan step is written and the thing
  does not exist.

## 1. Philosophy, and where it is written

The rules are consequences of a few principles. Each is recorded where the
maintainer stated it, usually with a date and a quotation, so that a reader
can tell a considered rule from an accident of the code.

| Principle | What it means in practice | Where it is written |
|---|---|---|
| Mechanical enforcement over review | A rule that a tool can check is a failing test, not a reviewer's task. "Keep it. Mechanical invariant enforcement is more valuable." (russ, 2026-10-09, on keeping three syscall libraries so the build graph can prove who reaches the backing filesystem). | `docs/design.md`, "Syscall fault injection stays link-time"; `docs/style.md` 1.1; step 25.39 |
| Gates carry self-checks | A gate (a test whose job is to reject something) ships a known-bad input that it must reject, so it cannot rot into a vacuous pass. | `test/qemu/README.md`, "Gates and their self-checks" (step 26.1); `docs/plan/process.md` step 5 |
| Types over comments | "It's easy to ignore a comment. The compiler enforces types. Express it in the type system." (russ, 2026-10-17). Also: move logic to compile time where possible. | `docs/style.md` 1.4 and 1.1; steps 25.35, 25.36, 25.38 |
| One way to do each thing | A change that adds a mechanism must say whether it subsumes an existing one and remove the old one in the same change. Prompted by a duplicate xattr-write path (russ, 2026-10-10). | `.claude/agents/dcfs-implementer.md`, `dcfs-investigator.md`, `dcfs-mechanical.md`, `dcfs-protocol.md` and `dcfs-reviewer.md` carry the text (the style reviewer's definition does not); `docs/style.md` 1.10 |
| No unnecessary code | Every branch names an input, syscall answer or event that reaches it. Code for the impossible is false documentation; the alternative is a `RET_CHECK` naming the invariant. A surviving mutant blocks a merge. | `docs/style.md` 1.10a; step 25.13 |
| No test-only code in production | A seam a test needs is a real feature with a production default. Zero "only used in tests" comments. Test code is in `*_test.cc` or `testonly/` targets marked `testonly = 1`. | `AGENTS.md`, "Tests"; `docs/style.md` section 8; steps 25.7, 25.7b |
| Fakes, not mocks | Dependencies are injected and replaced by small hand-written fakes. Syscall fault injection uses link-time `-Wl,--wrap` fakes. | `AGENTS.md`, "Tests"; `docs/design.md`, "Syscall fault injection stays link-time"; `docs/style.md` section 2 |
| No intentional crashes | No `CHECK`, `LOG(FATAL)`, `abort`, `exit` or `assert` in production. A violated invariant is a `RET_CHECK` that returns a Status. | `docs/style.md` 1.6b; step 25.9 |
| Status everywhere | Every fallible call returns `absl::Status` or `StatusOr`; every syscall goes through a wrapper that makes the errno into a Status. | `docs/style.md` 1.5, 1.6; step 25.1 |
| No retry loops, no timers | A failed operation is reported, not retried; code waits for the event, not for a duration. Both need russ's approval case by case. | `docs/style.md` 1.11, 1.12; `docs/plan/process.md`, "Timers"; `docs/plan/notes/retry-loops-2026-10-10.md` |
| No backward compatibility before the first deployment | A removed option, format or behaviour is simply gone: no shim, deprecation message or migration. (The migration machinery is kept and tested.) | `docs/plan/process.md`, "Parallelism and budget" |
| File names are bytes | A name, symlink target or xattr name is never text; it is escaped whenever it is printed. | `AGENTS.md`, "Code"; `docs/design.md`, "File names are bytes"; phase 9 |
| Bounded memory | A request's working memory is O(1) in the size of the files and directories it touches. | `AGENTS.md`, "Code"; `docs/design.md`, "Bounded memory" |
| Behave like the backing, only faster | For each operation, ask what the backing filesystem does over time and do that. A claim about what the backing, kernel or POSIX promises cites its source where it is written. | `AGENTS.md`, "Code"; `docs/design.md`, "Behave like the backing, only faster"; `docs/plan/process.md` step 5 |
| Abseil Tips of the Week as guidance | All of abseil.io/tips is design guidance, ranked below `AGENTS.md`, `docs/style.md` and the Google style guide. A finding that rests on a tip cites its number and is advisory. | `docs/style.md`, "Precedence, and the Abseil Tips of the Week"; steps 25.8, 25.32 |
| Submitted code obeys; working trees may do anything | A `sleep` or a `CHECK` to reproduce a bug is fine in a working tree; the checks refuse it in a commit. | `docs/style.md` 1.11 |

Two more rules shape how the rest of this document should be read:

- "While working, anything goes" means the rules are enforced at the point of
  submission, by checks and review, not by restricting what an author may try.
- russ (the maintainer) is the only source of exceptions: a retry loop, a
  crash site, a loosened complexity threshold each need his approval and are
  recorded with his name and date.

## 2. Compiler gates

### What is enforced

The compile flags that apply to **our** code and only ours are in
`.bazelrc`:

```text
build --per_file_copt=.*,-external/.*,-\\x40.*@-Werror,-Wimplicit-fallthrough,-Wno-sign-compare
build --linkopt -Wl,--fatal-warnings
```

- Every warning clang emits by default is an error, plus
  `-Wimplicit-fallthrough`. `-Wno-sign-compare` is the one disabled warning
  and the file says it "goes away with phase 7.3's -Weverything".
- External repositories (Abseil, libfuse, liburing, SQLite, and the rest) are
  excluded by both label and path, so they build with their own flags.
  `//tools/warnings` tests assert that four external targets
  (`@absl//absl/strings`, `@googletest//:gtest`, `@libfuse//:libfuse`,
  `@sqlite//:sqlite3`) do not receive `-Werror`, `-Wno-error`,
  `-Wimplicit-fallthrough` or `-Wno-sign-compare`.
- The linker fails on any warning (`--fatal-warnings`), for every link in the
  graph. `//dcfs:warning_flags_compile_test`, `warning_flags_link_test` and
  `warning_flags_test_link_test` check the flags on real command lines.
- Two exceptions for external code that prints warnings anyway are named in
  `.bazelrc` with their reasons (liburing's `-Wno-unused-parameter`,
  google_benchmark's `-Wno-thread-safety-analysis`).
- The toolchain is a pinned LLVM 22.1.8 with a pinned Debian sysroot
  (`BAZEL_DO_NOT_DETECT_CPP_TOOLCHAIN=1`); a host compiler is never used, and
  `//tools:toolchain_test`, `toolchain_hermetic_test` and their self-checks
  check that.
- Sanitizer configurations exist (`--config=asan`, `--config=ubsan`, with
  `-fno-sanitize-recover=all`) and run as CI jobs `asan` and `ubsan`; the
  details are in `docs/testing.md`.

### What is not enforced yet

`AGENTS.md` says: "Today: `-Werror`. From phase 7: clang's `-Weverything
-Werror` for our code, with every disabled warning listed in `.bazelrc` with
a reason." Phase 7 is partly done (the toolchain, coverage, sanitizers, the
clang-tidy set). **Step 7.3, `-Weverything`, has not landed.** So:

| Item | Today |
|---|---|
| Warnings enabled beyond clang's defaults | 1 (`-Wimplicit-fallthrough`) |
| Warnings disabled with a stated reason | 1 (`-Wno-sign-compare`, "goes away with phase 7.3") |
| `-Weverything` | not enabled; step 7.3 (parked, `docs/plan/README.md` Queue item 6) |
| `-Wnullability-completeness`, `-Wnullable-to-nonnull-conversion` as errors | not enabled; step 25.16 |
| `layering_check` (a target includes only headers of its direct deps) | not enabled; `tools/style_checks.bzl` turns it off for the clang-tidy actions, and the `.bazelrc` does not turn it on. `docs/style.md` 1.1 lists it as part of phase 7 |

A reader should therefore not infer from the guide that an exhaustive
warning set protects the code. What does the work in its place is the
clang-tidy set below, which is much wider than the compiler's default
diagnostics, but is an analysis run separately from the build.

## 3. clang-tidy, clang-query and the allowlist backlog

Source: [`.clang-tidy`](../.clang-tidy),
[`tools/style_checks.bzl`](../tools/style_checks.bzl),
[`tools/style_checks.py`](../tools/style_checks.py),
[`tools/style_checks_allow.txt`](../tools/style_checks_allow.txt),
[`tools/style_matchers/`](../tools/style_matchers/). Plan steps 7.5, 25.21,
25.23, 25.25, 25.30.

### How it runs

- **Where.** `//tools:style_checks_test` is a `small` Bazel test, so it runs
  in CI's `fast` job (`bazel test --config=fast`) and in every local
  `bazel test --config=fast //tools/...`. It reads the output of Bazel
  *actions* (`tools/style_checks.bzl`): the pinned LLVM's `clang-tidy` and
  `clang-query` are run per source file over the real compile command, in the
  sandbox, cached by content. A no-op rerun is seconds. At landing, a change
  to a core header cost 9 minutes cold and a no-op 26 s (`docs/plan/log.md`, the 25.21 merge entry,
  2026-10-10); that figure is from before later steps changed the scope and
  the check set, and was not re-measured for this document.
- **What is covered.** C++ in `dcfs/`, `dcfs/testonly/`, `test/qemu/guest/`,
  `bench/` and `tools/` (the five `style_findings` targets listed in the
  test's `data`). Third-party code and external repositories are not.
- **Pass or fail.** Every clang-tidy finding is an error
  (`WarningsAsErrors: '*'`). A finding passes only if it is in the allowlist.
  A `clang-diagnostic-*` finding, or a file that fails to parse, fails the
  test.

### The check set

- **Enabled groups (10):** `google-*`, `abseil-*`, `bugprone-*`, `cert-*`,
  `concurrency-*`, `misc-*`, `modernize-*`, `performance-*`, `portability-*`,
  `readability-*`.
- **Not enabled, by group:** `cppcoreguidelines-*` (not our guide), `llvm-*`,
  `fuchsia-*`, `hicpp-*`, `altera-*`, `android-*`, `boost-*`, `mpi-*`,
  `objc-*`, `openmp-*`, `zircon-*`, `linuxkernel-*` (other projects' rules),
  and `clang-analyzer-*` (it took 120 s of 140 s on one test file and over
  20 minutes on another; a separate large-tier target is planned under 7.5).
- **Denied inside the enabled groups: 21 checks**, each with a one-line
  reason in the `.clang-tidy` header. `tools/style_checks.py` checks that a
  denied check has a reason line, so the deny list cannot grow silently. They
  fall into four kinds:
  - Contradicts the Google guide or our guide:
    `modernize-use-trailing-return-type`,
    `readability-braces-around-statements`, `readability-named-parameter`,
    `modernize-use-ranges` (we use `absl::c_*`), `modernize-use-std-format`
    (we use `absl::StrFormat`), `modernize-use-nodiscard`,
    `modernize-use-auto`, `misc-const-correctness` (style 1.2: locals are
    non-const by default), `readability-uppercase-literal-suffix`,
    `misc-non-private-member-variables-in-classes`.
  - Out of proportion for a syscall layer: `readability-magic-numbers`,
    `readability-identifier-length`, `bugprone-easily-swappable-parameters`,
    `modernize-avoid-c-arrays`, `performance-enum-size`.
  - Conflicts with the link-time fakes (`-Wl,--wrap`, which require the names
    `__wrap_open` and `__real_open`): `bugprone-reserved-identifier`,
    `cert-dcl37-c`, `cert-dcl51-cpp`.
  - Tool or test-framework noise: `abseil-unchecked-statusor-access` (crashes
    clang-tidy 22.1.8 in dataflow analysis of `absl::StatusOr`),
    `bugprone-throwing-static-initialization`, `cert-err58-cpp` (gtest's
    `TEST()` registrations).
- **Options.** `readability-function-cognitive-complexity` has
  `Threshold: 15` with `IgnoreMacros: true` (the complexity of what is
  written, not of a macro's expansion). The style guide says the threshold
  is tightened as the tree allows and never loosened without russ.
  `readability-else-after-return` and `readability-misleading-indentation`
  are on, as style 1.6a requires, and have zero findings.
- **Suppressions.** A `NOLINT`, `NOLINTNEXTLINE` or `NOLINTBEGIN` must name
  its check in parentheses and carry a reason at the site; a bare one, a
  wildcard, or a check name with no reason fails the test
  (`suppression_problems` in `tools/style_checks.py`, step 25.30). A `git
  grep` for `NOLINT` (fixture directory excluded) finds 59 lines. 31 are the checker's
  own code and test strings (`tools/style_checks*.py`, `.bzl`). The other 28
  are in C++ and make 18 suppressions: 9 `NOLINTBEGIN`/`NOLINTEND` pairs in
  `dcfs/testonly/` (link-time fakes that need external linkage), 4 in
  `test/qemu/guest/guest.cc`, 1 in `dcfs/dir_cache_fs_test.cc:529`, 3 in the
  macro in `dcfs/ret_check.h:119-144` and 1 in `dcfs/testonly/process.cc:4`.
  One more line (`ret_check.h:114`) is prose naming them. No production
  `.cc` file has one.

### The clang-query matchers

For structural rules that clang-tidy has no check for, one file per rule in
`tools/style_matchers/<rule>.query` holds an AST matcher. Each file begins
with the `docs/style.md` section it enforces, and each has a `_bad.cc` and a
`_good.cc` fixture that `//tools:style_matchers_test` runs the matcher over
(the bad fixture must be reported, the good one must not). `tidy_bad.cc` and
`tidy_good.cc` do the same for `.clang-tidy`.

| Matcher | Rule | State |
|---|---|---|
| `banned_std` | no `std::function`, `std::unordered_*`, `std::chrono` (style 1.2) | enforced; no allowlist entries now |
| `iterator_pair_algorithm` | `absl::c_sort(v)`, not `std::sort(v.begin(), v.end())` (1.2) | enforced; no allowlist entries now |
| `dcfs_qualified_inside_dcfs` | no `dcfs::` qualifier inside `namespace dcfs` (1.3); replaced a regex that guessed the namespace | enforced; zero sites |
| `check_in_production` | no `CHECK`, `LOG(FATAL)`, `assert`, `abort`, `exit` and similar outside tests (1.6b) | enforced; zero sites (25.9 removed all 23) |
| `status_message` | never `absl::Status::message()`, which drops the code and errno payload (1.6, 1.7) | enforced |
| `backing_door_guard` | every backing syscall wrapper has a statement calling `BackingDoor` in its body (not that it comes before the syscall; `SyscallsBackingTest` shows the order), so a poisoned door refuses them all (1.6b) | enforced |
| `capturing_mutating_lambda` | a lambda that mutates a by-reference capture hides state change from its caller (1.12) | enforced; 91 backlog findings |
| `happy_path_nested` | an `if (x.ok()) { ...three or more statements }` holds the rest of the function (1.6a) | enforced; heuristic; 1 backlog finding |
| `pointer_without_nullability` | every pointer parameter, member or return says whether it may be null (1.2) | **report-only**; 455 findings; waits for step 25.16 |
| `status_dropped` | a failed `Status` is returned, logged or put into the replacement message, never dropped (1.6) | **report-only**; 23 findings |
| `status_uninitialized` | a `Status` local is made by the statement that declares it (1.6a) | **report-only** by design (a preference); 18 findings |

(`00_functions.query` is not a rule. It lists function definitions so the
backlog can be keyed by function.) The three report-only matchers are listed
in `REPORT_ONLY` in `tools/style_checks.py`, each with its reason.

### The allowlist backlog and its trend

The findings that existed when a check was turned on are listed in
`tools/style_checks_allow.txt`, one line per `file|function|check|count`.
There is no line number in a key, so editing code above a finding does not
disturb it. The test fails if a finding is not listed, if a listed key has
more findings than its count (a new finding in the same function), or if a
listed key has fewer (it was fixed and the line must come off). The file's
header says: "Do not add a line for new code; fix the code." That the list
never grows is review policy, not a check: nothing compares the list with
its previous version, so a change that adds a violation and a matching
allowlist line passes the test and relies on a reviewer to refuse the line.

Today:

| | Keys | Findings |
|---|---|---|
| Backlog at the first run (step 25.21, 2026-10-10) | 719 | 1354 |
| Backlog now | 330 | 558 |
| Report-only findings now (not in the allowlist) | | 496 (455 + 23 + 18) |

The 558 enforced findings are 210 in dcfs production code, 322 in dcfs tests
and `testonly/`, 23 in `bench/` and 3 in `tools/`. By check, the largest are:
`capturing_mutating_lambda` 91, `misc-include-cleaner` 71, `cert-err33-c` 49
(an unchecked return value), `misc-unused-parameters` 44,
`readability-function-cognitive-complexity` 39,
`readability-inconsistent-ifelse-braces` 36, `google-runtime-int` 31,
`bugprone-unchecked-optional-access` 29, `concurrency-mt-unsafe` 24,
`readability-implicit-bool-conversion` 23.

How the count went down: step 25.23 ran clang-tidy's own auto-fixes by check
(`bazel run //tools:style_fix`) and 25.17 converted macros and algorithms.
Examples recorded in the plan: `misc-include-cleaner` from 570 to 393 to 88
(it is 71 now), `modernize-use-designated-initializers` from 60 to 0, and
`banned_std` and `iterator_pair_algorithm` emptied. About 59 % of the findings
present in the first run are gone in eight days. What is left is the
judgement-shaped part (complexity, unused parameters, lambdas, unchecked
returns), which step 25.25 (a function-shape sweep) and 25.20 are meant to
address, and both are **parked** (see section 14).

Two caveats a skeptic should hold. First, the backlog counts only what the
check set detects; a rule with no check has no backlog number at all.
Second, the first-run count was taken with the then-current check set, so it
is not a trend of one quantity: checks were also added and fixed in between.

## 4. Formatting

- `.clang-format` is `BasedOnStyle: Google` plus `PointerAlignment: Right`
  (and `DerivePointerAlignment: false`). It is deliberately close to
  unmodified Google style (russ, 2026-10-11).
- `bazel run //tools:format` rewrites files with the pinned clang-format.
  `//tools:format_test` (size `small`) runs it in `--dry-run -Werror` mode
  over every tracked `.c`, `.cc` and `.h` file except third-party code and the
  known-bad fixtures, so it runs in CI's `fast` job. Its self-check formats a
  committed unformatted file (`tools/format_bad_fixture.cc`) and expects
  failure.
- A mechanical reformat commit uses the `style:` subject prefix and is listed
  in `.git-blame-ignore-revs`, so `git blame` is not buried by it (step 7.6a
  reformatted 87 files).
- **Not enforced:** formatting of Bazel files (buildifier), shell (shfmt,
  shellcheck), Python (the 80-column rule and the rest of Google's Python
  guide) and Markdown. `docs/style.md` section 3 says the BUILD list layout is
  "enforced by `//tools:format_test`"; that is not true today because the
  test's file filter is C and C++ only (`tools/format.py`, `wanted()`).
  Pinning buildifier, shfmt and shellcheck is step 7.6b (in milestone M2,
  not started). No pre-commit hook exists (`.githooks`, row F4 of the guide's
  appendix).
- Include order within a file is whatever clang-format's Google style
  produces; "include what you use" is checked by `misc-include-cleaner`
  (71 backlog findings), not by `layering_check` (section 2).

## 5. `tools/repo_shape.py`

[`tools/repo_shape.py`](../tools/repo_shape.py) (612 lines) holds
repository-shape rules too small or too textual for a matcher. Each rule is a
function that takes the tree root and returns problems.
`//tools:repo_shape_test` runs them on the real tree and
`//tools:repo_shape_self_check_test` runs them on fabricated trees to show
each one fails. Both are `small`.

Eleven rules run together (`all_problems`), plus the SPDX check, which the
test calls separately:

| # | Rule (function) | What it refuses | Allowlist (file, size today) |
|---|---|---|---|
| 1 | `third_party_readmes` | a `third_party/<name>/` with no `README.md` saying what it is and how to update it | none |
| 2 | `guest_scripts_used` | a `test/qemu/guest/*.sh` that no test target or sourcing script uses (dead guest scripts) | built-in list of five non-test names |
| 3 | `guest_sleeps` | a bare `sleep` in a guest script (a timer standing for an event, style 1.11); the count per file must equal the list's | `tools/repo_shape_sleeps.txt`: 26 files, 50 sleeps, each with the event that would replace it |
| 4 | `no_new_guest_shell` | a `*.sh` under `test/qemu/` not on the list (guest tests are C++ from step 25.34) | `tools/repo_shape_guest_shell.txt`: 124 scripts; only shrinks |
| 5 | `disabled_checks_listed` | a guest check marked `disabled` that the README's Limitations section does not name | none |
| 6 | `no_test_only_comments` | a production comment that gives tests as the reason for code, or a `ForTest` / `_for_test` name (style section 8) | none |
| 7 | `no_testonly_friends` | a production class that declares a `testonly::` class as a friend (step 25.7b) | none |
| 8 | `fixed_arrays` | a local buffer sized once as `std::vector<T> v(n);` or `std::string s(n, '\0');` instead of `absl::FixedArray` (style 1.2, step 25.12) | `tools/repo_shape_fixed_arrays.txt`: 1 entry (a vector that is a caller's `Credentials::groups`, resized after construction) |
| 9 | `project_prefix` | an identifier beginning `Dcfs` + capital in `dcfs/` (the namespace says it, style 1.3, step 25.14) | none |
| 10 | `absl_prefixed_status_macros` | `ABSL_RETURN_IF_ERROR` / `ABSL_ASSIGN_OR_RETURN`; use the short names from `//dcfs:status` (style 1.6, step 25.17) | `tools/repo_shape_absl_macros.txt`: empty |
| 11 | `nodiscard_on_status` | `[[nodiscard]]` before `absl::Status` or `StatusOr`, which are must-use already (step 25.17) | none |
| 12 | `spdx_licenses` | a source file with no `SPDX-License-Identifier` on line 1, a licence not in `tools/licenses_allowed.txt`, or a non-MIT file not listed in `tools/third_party_files.txt` (copyleft only if test-only) | `tools/third_party_files.txt` (files we hold that are not MIT, with reasons) |

Notes for a skeptic:

- The count-based allowlists (sleeps, fixed arrays, `ABSL_` macros) hold a
  count per file with a reason, and the count must be *equal* to the file's,
  so both new violations and forgotten removals fail. The guest-shell list
  has a different shape: one path per line, each of which must exist. That
  the lists only shrink is a header convention that nothing compares with
  the previous version (section 3). The guest-shell list is the clearest
  sign of direction: 124 scripts, 18,622 lines of shell under `test/`
  (section 15), is the size of the shell-to-C++ conversion still to do, and
  the rule stops it growing.
- Rule 3's allowlist is what is left of an earlier rule ("no timers") in the
  test scripts; 50 bare sleeps remain, each with a stated reason (time is the
  subject, a background process that waits for nothing, a settle to be
  replaced by an event). The daemon itself has none (section 6).
- Some regex rules are imprecise by nature (rule 8 matches a text pattern).
  A rule that needed the AST was moved to a matcher: step 25.21's
  `dcfs_qualified_inside_dcfs` replaced a repo_shape regex that guessed the
  enclosing namespace from brace depth.

A separate branch (the rules-index branch, not merged when this was
written) adds a `repo_shape` rule, `rule_index`, and a check that the guide
does not restate a rule. Rule counts and the table above change when it
merges; the counts here are those of `ac3374df`.

Other repository-level checks, each a `small` Bazel test with a self-check:

| Check | Target | What it holds |
|---|---|---|
| Commit subjects | `.github/ci/commit_subjects.sh`, `//tools:commit_subjects_test` | section 11 |
| Tips of the Week numbers | `//tools:totw_citations_test` | every `TotW #N` cited in `style.md`, `abseil-utilities.md`, `design.md` and C++ is a published tip, checked against a pinned index (`//third_party/abseil_tips`, step 25.32) |
| Banned symbols | `//tools:banned_symbols_test` | section 6 |
| Raw syscalls | `//tools:raw_syscalls_test` | section 8 |
| Backing users | `//tools:syscalls_backing_users_test` | section 8 |
| Shipped dependencies | `//tools:shipped_deps_test` | section 9 |

## 6. Banned symbols in the shipped binary

[`tools/banned_symbols.txt`](../tools/banned_symbols.txt) is a deny list of
**69 `ban` lines** and **81 `allow` lines**, each with the rule it enforces.
`//tools:banned_symbols_test` (`small`) reads the linked, shipped binary
(`//dcfs:main_static`), uses `nm` and `objdump` from the pinned LLVM, and
fails if

- an object linked into the binary references a banned symbol, unless an
  `allow` line covers that origin and symbol; or
- a banned symbol is *defined* in the binary but no scanned object references
  it (something outside the Bazel graph, glibc or libc++, pulled it in).

This is a check on the artifact, not on source text, so a rule broken through
a macro, a template or a transitive dependency is still caught. The 69 lines
are patterns, many of them globs or mangled names, and are grouped by the
rule behind them:

- **No paths after startup:** `realpath`, `getcwd`, `nftw`, `fts_open`,
  `glob`, `std::filesystem`. Once the daemon runs, objects are reached by
  file descriptor or handle (`docs/design.md`, "File descriptors and handles,
  never paths").
- **No subprocesses or plugins:** `system`, `popen`, `exec*`, `posix_spawn*`,
  `dlopen`.
- **Static glibc's run-time loading:** `getaddrinfo`, `getpw*`, `getgr*`,
  `iconv_open`, which would load NSS or gconv modules.
- **Single-threaded by design:** `pthread_create`.
- **No timers (style 1.11):** `sleep`, `usleep`, `nanosleep`,
  `clock_nanosleep`, `alarm`, `timer_create`, `timerfd_create`,
  `sqlite3_busy_timeout`, `sqlite3_busy_handler`.
- **The time comes from `Context::clock`:** `clock_gettime`, `gettimeofday`,
  `time`, `absl::Now` (step 26.10).
- **Statuses carry the message; output goes through Abseil:** `strerror`,
  `strerror_r`, and the `printf` family and `puts`.
- **No intentional crashes (style 1.6b):** `abort`, `exit`, `__assert_fail`,
  `std::terminate`, and Abseil's fatal and debug-fatal log messages (the
  symbols behind `CHECK` and `LOG(FATAL)`). Abseil's headers put
  `__assert_fail` into every object, so those `allow` lines name the
  functions they cover (`via`); a function of ours that references it is
  still a violation.

The `allow` lines are the visible exceptions: 17 have the origin `<runtime>`
(symbols that glibc or libc++ pulls in by itself, which no scanned object
references), and the other 64 name the objects or functions that may reference
a symbol (for example, the `__assert_fail` that Abseil's headers put into
every object). The self-check (`//tools:banned_symbols_self_check_test`)
links a fixture (`tools/banned_symbols_fixture.cc`) that references five of
the 69 patterns (`realpath`, `sleep`, `usleep`, `alarm`, `nftw64`), by a call,
by a reference from data alone, and by a definition with no reference, and
shows each is found. The other 64 patterns are not exercised by the
self-check.

Limits: the check sees what the shipped binary links, so test code, bench
code and shell are out of its scope; a banned behavior reachable only by
inlined code with no symbol reference would not be seen.

## 7. Error handling and logging conventions, and how they are held

The conventions are `docs/style.md` 1.5 to 1.7. Their enforcement is mixed.

| Convention | Mechanism today |
|---|---|
| Errors are built with `StatusBuilder` helpers (`InternalErrorBuilder() << ...`), not `absl::XError(StrCat(...))`; the helpers live in `dcfs/status.h` | review; step 25.1 converted 41 constructors and 5 direct builder sites at that time; no matcher or regex checks it now |
| An errno becomes a Status through `dcfs::ErrnoToStatus` (a forwarded errno) or `ProducedErrnoToStatus` (an errno dcfs chose), never `absl::ErrnoToStatus` directly | syscalls go through wrappers (section 8), which call `ErrnoToStatus`; the direct use is not separately checked |
| The short propagation macros `RETURN_IF_ERROR`, `ASSIGN_OR_RETURN` | enforced: repo_shape rule 10, empty allowlist |
| `[[nodiscard]]` is not written on `Status` returns | enforced: repo_shape rule 11 |
| `Status::message()` is never read; the whole Status goes into the replacement message | enforced: `status_message` matcher (exceptions: `StatusToString(const absl::Status&)` and `testonly::SerializeStatus`) |
| A failed Status is returned, logged or put whole into the replacement, never dropped | **report-only**: `status_dropped`, 23 findings |
| Function shape: guards, then a straight line of effects; no `else` after `return` | `readability-else-after-return` (enforced, 0 findings), `readability-function-cognitive-complexity` at 15 (39 backlog), `happy_path_nested` (heuristic; 1); the rest is review |
| Production code has no crash sites | enforced: `check_in_production` and the banned-symbols list (section 6) |
| A `Status` is serialized one way | there is one serializer for the guest tests, `dcfs/testonly/status_wire` (`SerializeStatus`, a protobuf), added in step 25.34; the `status_message` matcher names it as an exemption. That there is exactly one is a review and design fact, not a mechanical one |
| Logging uses Abseil logging with Abseil's semantics; ERROR means dcfs produced the failure rather than forwarded the backing's | review plus tests: `ProducedByDcfs` marks a Status dcfs made, so the reply handler logs ERROR only for those (step 25.3); end-to-end tests assert at `--v` levels (e.g. `cancel.sh`). No static check of log levels |
| Three kinds of message (step 25.33b) | review |

The reachable-cause rule (style 1.10a) has no mechanical check yet. Step
25.13 specified a comment-phrase check ("cannot happen", "just in case",
"defensive", "paranoia") in `repo_shape.py` and a rule that surviving
mutants fail the per-push job; neither exists. A case-insensitive `git grep`
for those phrases in production comments found 2 hits when this was written:
`dcfs/file_handle.cc:155` ("just in case") and `dcfs/backing.cc:2078-79`
("be exact anyway", in `GetGroups`, the example style 1.10a is written
around).
See section 14.

## 8. The syscall layer

The rule: every syscall in the tree goes through `dcfs/syscalls*.h` wrappers
that turn the errno into a Status. Three libraries, so that the build graph
can prove which code reaches the backing filesystem:

| Library | Header (lines) | Contains |
|---|---|---|
| `//dcfs:syscalls` | `syscalls.h` (169) | process-local and by-path calls |
| `//dcfs:syscalls_backing` | `syscalls_backing.h` (162) | calls that can reach the backing filesystem; each calls `BackingDoor` first |
| `//dcfs:syscalls_process` | `syscalls_process.h` (32) | process control |

How the rules about it are held:

- **No raw libc syscall outside the wrappers.** `//tools:raw_syscalls_test`
  scans the C++ of `dcfs/`, `dcfs/testonly/`, `bench/` and
  `test/qemu/guest/` for any of **158 names** in
  `tools/raw_syscalls_names.txt` that is not qualified `syscalls::`, plus any
  use of `std::filesystem`, `std::ifstream`, `std::ofstream`, `std::fstream`
  and `absl::Now(`. It cannot see a call through a macro or function pointer,
  a name not on the list, or a stream type used without the `std::` prefix
  (the header comment says so). `tools/fhtest.c` and `tools/testutil.c` (C
  programs for the guest) are out of scope. Step 25.1c (done, commit 752c573).
- **Only the backing layer reaches the backing filesystem.**
  `//dcfs:syscalls_backing_users` is a `genquery` of the direct dependents of
  `//dcfs:syscalls_backing`, compared with the golden
  `dcfs/syscalls_backing_users.txt` by `//tools:syscalls_backing_users_test`.
  A new dependent needs a deliberate, reviewed edit to the golden, with a
  comment saying which syscalls it uses. This is the build-graph form of the
  invariant in `docs/style.md` 1.8: every syscall that can reach the backing
  is made in `backing.cc` or a layer below it, plus startup and fsck.
- **The wrapper's return type records whether a call can fail.** A wrapper
  returns `Status`/`StatusOr` unless the manpage says the call cannot fail.
  Because `Status` is must-use, a dropped result of a fallible call does not
  compile; a bare call to a wrapper that returns void is "correct by
  construction" (style 1.5, 1.6a).
- **Wrappers are thin.** Names are the manpage names; no composition, retry
  or policy. Helpers built on them live in `backing.cc`. This is review; the
  `backing_door_guard` matcher checks only that each backing wrapper calls
  the door.
- **Fault injection is link-time.** The 49-site fault sweep and the unit
  tests use `-Wl,--wrap` fakes (`docs/design.md`, "Syscall fault injection
  stays link-time"). A wrap needs a comment at the BUILD rule (style 3).

Planned, and not built:

- **Step 25.35 and 25.36** (strong types for ids and sentinels) and
  **25.38** (`syscalls::` typed): `OpenFlags`, `AtFlags`, `Mode`, `DirFd`
  distinct from `FileDescriptor`, `PathComponent`, `std::span<std::byte>`
  buffers, `absl::Time` and `absl::Duration`, fcntl's operation as a template
  argument. The step also says: "Every rule in this step gets a mechanical
  check: a clang-tidy or clang-query matcher, or a type that makes the wrong
  code not compile." State: queued in milestone M2 (25.38 follows 25.37 and the
  dependency-injection step).
- **Step 25.41** (dependency injection in place of link-time `--wrap`):
  drafted 2026-10-18 by an agent, reviewed by the orchestrator, **awaiting
  russ's review**. It would replace the fakes' mechanism. The history is
  instructive: a first virtual-`Syscalls` implementation was reverted on
  2026-10-12 because it collapsed the three libraries and left two ways to
  call a syscall. The draft is
  `docs/plan/notes/dependency-injection-2026-10-18.md`.

## 9. Dependencies and the build

Short, because it is largely infrastructure.

- **Everything goes through Bazel.** Each build step, tool, image and test is
  a Bazel target with declared inputs (`AGENTS.md`, "Running Bazel"); the
  disk cache makes unchanged tests count as passed soundly. Exceptions are
  named in the tool that has them (for example, the mutation tool drives
  Bazel itself, `tools/mutation/README.md`).
- **Third-party code is fetched and built by Bazel.** `MODULE.bazel` has 14
  `bazel_dep` entries (the Bazel Central Registry where possible) and 11
  `http_archive` / `http_file` calls with a sha256; `third_party/<name>/` holds
  only overlays, patches, config, and a README that records the pin and how
  to update it (repo_shape rule 1 enforces the README). The exception is
  Alpine's packages (test kernel, QEMU, mkfs tools, busybox), which are pinned
  to a release branch and verified by Alpine's package signatures
  (`third_party/alpine/README.md`, `//third_party/alpine:signature_test`).
  `MODULE.bazel.lock` is read in `--lockfile_mode=error`, so it is never
  rewritten silently.
- **Hermetic toolchain.** LLVM 22.1.8 and a Debian sysroot are fetched and
  pinned (`third_party/llvm`); `//tools:toolchain_test`,
  `toolchain_hermetic_test` and their self-checks verify there is no host
  compiler, libc or header in the build. The host still provides a glibc able
  to run clang, bash, and a few utilities (`docs/plan/README.md`, step 7.1b).
- **Reproducible builds.** CI job `reproducible` builds `main_static` and the
  man page twice, from two copies of the repository at different paths, in two
  output bases with no disk cache, and requires byte-identical results
  (`tools/reproducible_build.sh`, `tools/repro_compare.py`,
  `//tools:repro_compare_self_check_test`; step 26.12).
- **What the daemon links is a golden.** `//dcfs:linked_deps` writes the
  external repositories behind `//dcfs:main` and `//dcfs:main_static`;
  `//tools:shipped_deps_test` compares them with `dcfs/shipped_deps.txt`, and
  `//tools/sbom:sbom_test` checks the SBOM against the same list. Adding a
  linked dependency is thus a reviewed edit.
- **SBOM and vulnerability scan.** `tools/sbom/sbom.py` writes CycloneDX 1.5
  SBOMs of the pins; the CI `osv` job scans the shipped one with OSV-Scanner
  and fails on any finding not in `osv-scanner.toml` (which carries expiring
  ignores) and on an expired ignore. The test-only pins are scanned
  informationally. The same job runs weekly on a schedule, since advisories
  change without a commit.
- **Licences.** `docs/licenses.md` is the inventory, with each licence read
  from a primary source; repo_shape rule 12 holds the headers. Eight
  components are linked into the binaries, under Apache-2.0, LGPL-2.1, MIT
  and public domain.

## 10. Review process and the findings ledger

Source: [`docs/plan/process.md`](plan/process.md), "A step, start to finish",
step 5; [`.claude/agents/`](../.claude/agents/);
`tools/agents/review_findings.py`;
[`docs/plan/review-findings.tsv`](plan/review-findings.tsv).

**What happens to every step.**

1. An implementing agent works in a lane (a separate checkout), writes the
   failing test first and reports with the failing output quoted, the test
   commands run with real results, coverage of the new code, and deviations
   from the plan. The agent definitions require it to run the style checks
   (`bazel test --config=fast //tools/...`) on its own diff before reporting.
2. **`dcfs-style-reviewer` reviews the full diff of every step before it
   merges** (russ, 2026-10-16). It reads `docs/style.md` in full, checks
   `AGENTS.md`, `docs/style.md` (including the comment rules in 1.9), the
   Tips of the Week and the project invariants, and gives an approval or
   findings. Where the guide does not settle a point it asks rather than
   deciding by taste, so the answer can become style text; rulings
   accumulate in `docs/style.md` (for example, six rulings from the first
   pass entered style 1.6a, and the 22.5 and 17.6 reviews added comment-length
   rulings to 1.9).
3. For steps touching the write-through protocol, identity or crash handling,
   the Opus-level `dcfs-reviewer` also reviews: invariants, races, crash
   safety, test quality.
4. The orchestrator checks the same list again, runs the step's tests and the
   tier the phase calls for, rebases, runs the commit-subject gate, and
   fast-forwards `main`. A step that fails review twice moves up one agent
   tier (`CLAUDE.md`, "Orchestration").
5. Every gate a step adds must carry a self-check with a known-bad fixture; a
   gate without one is itself a review finding.

**The ledger.** `docs/plan/review-findings.tsv` records one row per finding
with its date, step, round, reviewer, severity (`blocker`, `should`, `nit`,
`none`), category, rule violated, file, summary, whether it could be
mechanized (`checked`, `could`, `no`), and outcome.
`tools/agents/review_findings.py` maintains it (`add` appends findings from a
block on stdin, `summary` prints statistics) and has a unit test. The
committed file is counted here; the main checkout's working copy may be ahead
of it. It is young: all
183 rows are from 2026-10-17 and 2026-10-18. 176 are findings, from six
steps (25.34 with 77, 22.5 with 40, 25.28b with 23, 17.6 with 16, 12.24 with
10, 17.2e with 10); the other 7 are clean-review rows (severity `none`: four
`agents`, one each for 12.24, 25.33 and 25.33b). Treat the percentages below
as indications.

| Cut | Counts |
|---|---|
| Severity | nit 97, should 70, blocker 9, none (clean review) 7 |
| Category (176 findings, clean reviews excluded) | style-other 50, comment-accuracy 28, docs 16, test-shape 14, test-gap 13, correctness 12, contract-claim 12, commit-hygiene 8, status-handling 6, naming 6, model-gap 6, dead-code 5 |
| Category (should + blocker only, 79) | style-other 15, comment-accuracy 13, test-gap 12, correctness 10, contract-claim 7, model-gap 6, docs 5, test-shape 4, dead-code 3, status-handling 2, commit-hygiene 2 |
| Could a tool have caught it (176 findings) | no 108, could 52, checked 16 (already caught by a gate) |
| Outcome (183 rows) | fixed/merged 143, decided 17, open 18, other 5 |
| Reviewer | `dcfs-style-reviewer` 116, `dcfs-reviewer` 42, Gemini-family reviewers 25 |

What this says. About 39 % of the findings (68 of 176) are rules a tool
could check or already does, which is the concrete argument for step 25.39.
`comment-accuracy` is the second largest category after style-other: comments
are the thing the mechanical set least covers. Correctness findings (12) are
a small share, and test gaps (13) and model gaps (6) are the ones that
weight the review of risky code. The dead-code category (5) is why a
"no production caller" check is in 25.39.

## 11. Commit discipline and the design history

- **Subjects start with a plan step** (`25.34: area: what changed`) or one of
  the prefixes `plan:`, `style:`, `process:`, `warnings:`, `notes:`,
  `audits:`, `agents:`, `R<n>:`. The rule is `.github/ci/commit_subjects.sh`,
  run by the CI job `subjects` over each push's range and by the orchestrator
  before a merge; `//tools:commit_subjects_test` is its self-check. The job
  fails the run's status but gates no test job (nothing `needs` it); the
  rule stays strict because the orchestrator checks branches before merging.
  `docs/plan/process.md` records the incident that caused the gate (eight
  commits with a review-finding label as the subject reached `main`).
- **Test-first** is a rule (`AGENTS.md`, "Tests"): a behavior change or bug
  fix begins with a test that fails on the current code, and the failure is
  quoted in the commit body under a "Failing first" heading (style 7).
  Nothing checks this mechanically; it is verified from the implementing
  agent's report and by the reviewers.
- **Commit bodies** wrap at 72, say why, and end with a `Co-Authored-By`
  trailer naming the model that wrote the commit (style 7). Not mechanically
  checked.
- **Never push** is an agent rule; the orchestrator merges into `main` by
  fast-forward from a lane clone.
- **The plan and the log are the design history.** `docs/plan/phases/` (one
  file per phase, with each step's text, rulings and "As built" notes),
  `docs/plan/log.md` (4,235 lines of dated entries, including merge records
  and the evidence behind each), `docs/plan/decisions.md`,
  `docs/plan/notes/` (investigations, audits and handoffs), and
  `docs/plan/audits/` (the original read-only audits, including a
  2026-09-28 style and dead-code audit of the code at an early commit). A
  reader can follow a rule from the incident that caused it to the commit
  that enforced it. This is a strength for a reader who wants reasons. It is
  also a large body of prose to keep current, and parts are stale
  (section 14).

## 12. Documentation conventions

- Decisions and their reasons go in `docs/design.md`; user-facing behavior
  and limitations in `README.md`; test usage in `test/qemu/README.md`; each
  pin in its own `third_party/<name>/README.md` (style 7). A design decision
  that has a real alternative is reported with that alternative, and, if it
  changes user-visible behavior or a guarantee, goes on the questions table
  for russ before merge (`docs/plan/process.md`).
- **A decision names its maker and date** ("russ, 2026-10-06"), measured
  numbers carry units, date and environment, and prose is plain and specific
  (style 7).
- **Contracts cite their source where they are written.** A claim about what
  the backing, the kernel or POSIX promises cites POSIX text, a man page, or
  the kernel code path. A kernel citation gives the path, the line and the
  kernel version once per comment: `fs/ext4/namei.c:1768 (Linux v7.3-rc4)`;
  never a host path (style 1.9). A native differential test (the same
  operation with dcfs out of the loop) backs a claim wherever one can be
  measured. The kernel source is pinned (`third_party/linux`), so a citation
  can be checked against that tag, but **a checker for citations does not
  exist**: wrong line numbers were found by reviewers in steps 22.5, 17.6 and
  25.28b, and a checker is an item of step 25.39.
- **The style guides are pinned.** Google's C++, Python, Shell and Markdown
  guides are rendered locally by `bazel build
  //third_party/google_styleguide:guides` (step 25.29), and the Abseil Tips of
  the Week by `//third_party/abseil_tips` (step 25.32), so reviewers read the
  pinned text rather than memory or the network. `docs/abseil-utilities.md`
  (step 25.19, 463 lines at merge) is a swaps table of what Abseil already
  provides, read before writing a helper.
- **Comments are governed by style 1.9:** self-explanatory code first;
  document timeless invariants, footguns and deliberate omissions; no
  docstring that restates the function's name; length limits. Review only.
- **Not enforced:** the Markdown style (fenced-block languages, 80-column
  wrap). Appendix A of the guide counts 37 bare fences and 21 over-long prose
  lines as of 2026-10-07; those numbers are stale (the fact-check of this
  document found about 207 over-long lines now), and the current counts were
  not taken here. No Markdown lint target exists: querying the
  test targets for `doc`, `markdown` or `link` finds only unrelated names
  (`warning_flags_link_test`, `*unlink*` formal tests, the `abseil_tips`
  index tests).

## 13. Coverage and mutation, as a maintainability matter

The correctness reasons for these gates are in `docs/testing.md`. Here the
question is only whether they keep untested or dead code out.

- **Coverage.** Source-based clang coverage is measured over `dcfs/`,
  `bench/` and `tools/`, merged across the QEMU guests (step 7.2,
  `.bazelrc`). CI job `coverage` runs `tools/coverage_gate.sh` (with
  `--config=presubmit`, so the small and medium tiers only) against the
  committed baseline `dcfs/coverage_baseline.txt` (97.27 % lines, 83.97 %
  branches today). **The gate covers `dcfs/*.cc` only:** headers, `bench/`
  and `tools/` are printed, not gated (`tools/coverage_gate.sh`, lines 7-8
  and 55-58). The gate fails below the baseline on either number and
  reports a rise; the
  baseline only moves up by policy (`AGENTS.md`). Gaps that cannot be covered
  are listed with reasons in `docs/coverage.md`. **The limit:** the gate is on
  the total, so a new uncovered branch passes while the total stays above the
  floor. A per-change gate is item 5 of step 25.39 (planned).
- **Mutation.** `tools/mutation/mutate.py` mutates clang AST sites of
  `dcfs/metadata_cache.cc`, `dcfs/backing.cc`, the protocol paths of
  `dcfs/dir_cache_fs.cc` and the sync-point calls in `dcfs/fuse_ops.cc`
  (`tools/mutation/scope.txt`: four files; the TLA+ model is mutated
  separately). A mutant that no
  test kills is a missing test or unnecessary code. CI runs it per push for
  the functions the push touches (`mutation-changed`, at most 30 mutants) and
  weekly in full (`mutation.yml`). **The limit:** the job lists survivors in
  the summary and fails only on a tooling error. `AGENTS.md` and style 1.10a
  say a survivor blocks the merge; today the block is applied by the
  orchestrator, not by the job (step 25.13 item 1 and 25.39 item 6 are the
  planned change), and the scope is four files.
- **Dead code.** `-Weverything` is not on, so the compiler finds only unused
  statics, locals and parameters of the default set; clang-tidy's
  `misc-unused-parameters` has 44 backlog findings. Nothing detects a public
  function with no production caller (section 14).

## 14. Known debt and open work

Each item is something a skeptical engineer should weigh, with the step that
addresses it and its state. "Parked" means deferred until after the
spare-disk trial and the quota reset (`docs/plan/README.md`, Queue item 6).

| Debt | Size or evidence | Addressed by | State |
|---|---|---|---|
| `-Weverything` is not on | one extra warning beyond defaults | 7.3 | parked (Queue item 6) |
| clang-tidy backlog | 558 findings / 330 keys (210 in dcfs production code) | 25.23, 25.25 (function-shape sweep), 25.20 (candidates sweep), ordinary edits ("the backlog shrinks as code is touched for other reasons, not as a campaign") | 25.23 done; 25.25, 25.20 parked |
| Report-only findings | 455 nullability, 23 dropped statuses, 18 uninitialized | 25.16 (nullability on every pointer). The 23 dropped-status sites (matcher added by 25.33) have no step of their own that was found | 25.16 parked |
| `layering_check` off | nothing stops a target using a header of a transitive dep | 7.7 | parked (Queue item 6) |
| Bazel, shell, Python, Markdown not formatted by a gate | 18,622 lines of shell under `test/`; 17,035 lines of Python (section 15) | 7.6b (pin buildifier, shfmt, shellcheck; in M2), 7.7 (parked) | not started |
| Guest shell not yet converted to C++ | 124 scripts on the allowlist | 25.34 (rule and first port, merged), the shell ports (M2), 26.18 (e2e oracles in C++; parked) | ongoing |
| Bare sleeps in guest scripts | 50 sleeps in 26 files | 6.5 scrub | allowlist shrinking |
| Retry loops in the daemon | Three bounded `kAttempts` loops in `dcfs/dir_cache_fs.cc` (unlink, rename, readdir: lines 1194, 1323, 2342); two ERANGE loops of 4 attempts in `dcfs/backing.cc` (`ListXattrOPath` at :163, `GetXattrOPath` at :181); and `GetGroups` (`backing.cc:2073`), an unbounded `while (true)` whose comment ("be exact anyway") is style 1.10a's own example. The plan's 25.10 text counts six loops | 25.10 (retry loops out; the model gains a liveness property) | queued (Queue item 5) |
| Duplicate mechanism: xattr writes | `ApplyXattrOpOn` in `dcfs/backing.cc` reopens regular files and directories for `fsetxattr` while every other type uses the `/proc/self/fd` path form, which already serves reads; this is the case that produced the "one way to do each thing" rule | 25.11 | parked (Queue item 6) |
| Confession-phrase check, "survivors block" as a job failure | 2 hits by grep; the job reports only | 25.13 | parked (Queue item 6) |
| `docs/style.md` rules that say "mechanically" for checks that do not exist | `const_local_value` matcher, the function-pointer matcher, the `char *` matcher (25.24), the `ABSL_FLAG`-outside-main repo_shape rule (25.20), the nullability warnings as errors (25.16), `layering_check`, buildifier in `format_test` | 25.39 (inventory every rule with its mechanism today). 25.39 comes after 25.38, which comes after 25.37 and the dependency-injection step (25.41, awaiting russ), so it is late in M2 | queued, raised in priority 2026-10-18 |
| Rules enforced by review only | comments (1.9), names, one-abstraction (1.10), reachable cause (1.10a), fakes not mocks, kernel-citation form, time as `absl::Time`, in-band sentinels, `std::optional<std::string_view>` | 25.39: kernel citation checker, time-type ban, sentinel comparisons, TLA+ comment length, host-path grep; strong types 25.35, 25.36, 25.38 | queued, M2 |
| No dead-code check | `syscalls::kill` and `syscalls::waitpid` had no production caller | 25.39 item: link shipped binaries with `-ffunction-sections --gc-sections --print-gc-sections` and fail on any discarded function of ours not in a reasoned allowlist | queued |
| No diff-coverage gate | the floor is on the total | 25.39 item 5 | queued |
| Mutation is advisory per push and covers four files | | 25.13, 25.39 item 6 | queued |
| No duplicate-code detection | duplicates found by review (three fork/exec copies and others) | 25.27 | listed in M2; its phase text says parked; reassigned to an agent 2026-10-17 |
| Link-time `--wrap` fakes | works (49-site fault sweep) but is the only way to fake syscalls; fixed in the linker's naming | 25.41 | drafted, awaiting russ |
| Stale text in the style guide and elsewhere | Appendix A of `docs/style.md` is dated at `e680508` (2026-10-07); its rows F1, F2 and F6 are false now (the format test, pinned clang-format and `.clang-tidy` exist); C3 is 9 lines, not 92; C13 is 642, not 292; D2 is about 207, not 21; N1's `cache` namespace is gone. Other items are in "Where `docs/style.md` and other documents run ahead of the tree" | none specific; 25.39 item 1 would reconcile; another branch is restructuring `docs/style.md` and `AGENTS.md` | open |
| Young review ledger | 183 rows over two days; 18 rows are still `open` | accumulates | ongoing |
| Plan bookkeeping is large | `log.md` is 4,235 lines; phase 25's file is 1,905 lines and carries stale text (for example, 25.39 item 4 says the findings ledger does not exist, and it does) | orchestrator's clerk | ongoing |
| Style reviews depend on agents | reviewer quality is a variable; the ledger shows reviewers disagree with each other about tests | 25.39 reduces the surface | structural |
| Enforcement tooling is itself large | `tools/` and the rest outside `dcfs/`, `test/` and `formal/` is 25,157 SLOC against 14,837 SLOC of production code (section 15) | none; a cost of the approach | structural |

Things to worry about, in one list, in the order a reader should weigh them:

1. A rule's mechanical status is not uniform, and the guide does not say
   which are which. Section 7's table and the list in this section are the
   map. Until step 25.39 produces the official inventory, treat the guide as
   the rulebook and this document as the enforcement map.
2. The compiler is not the strongest line of defence; clang-tidy is, and it
   is a separate analysis with a backlog. A broken rule inside the backlog
   keys passes.
3. About half of the SLOC (45,968 of 91,496) is test code, and 18,622 lines
   of it are shell the project intends to convert; those lines are the least
   checked code in the tree
   (shell has no formatter or linter gate).
4. The review step is carried out mostly by AI agents, with russ's rulings
   feeding back. The ledger exists to measure that; its sample is small.
5. Some safety properties (allowlists never growing, a surviving mutant
   blocking a merge, diff coverage) are policy, applied by the orchestrator,
   not by CI.

## 15. Metrics

**Size.** `tools/agents/dashboard_lanes.py` (`compute_sloc_rows`) counts
non-blank, non-comment lines over `git ls-files`, by area and language.
Its areas: **prod** (`dcfs/` minus tests and `testonly/`), **test**
(`dcfs/*_test.cc`, `dcfs/testonly/`, `test/`), **model** (`formal/`), and
**tools** (everything else, including `bench/`, `third_party/` overlays and
`.github/`). Run at `ac3374df`:

| Area | Language | SLOC |
|---|---|---|
| prod | C++ | 13,060 |
| prod | Starlark | 1,777 |
| **prod** | | **14,837** |
| test | C++ | 22,892 |
| test | shell | 18,622 |
| test | Starlark | 2,719 |
| test | Python | 1,107 |
| test | C | 628 |
| **test** | | **45,968** |
| model | TLA+ | 3,762 |
| model | Starlark | 1,476 |
| model | shell | 296 |
| **model** | | **5,534** |
| tools | Python | 15,928 |
| tools | Starlark | 3,286 |
| tools | C | 2,728 |
| tools | shell | 1,561 |
| tools | C++ | 1,466 |
| tools | clang-query | 117 |
| tools | TLA+ | 71 |
| **tools** | | **25,157** |

Test code is about 3.1 times production code by this count. The repository
has 1,098 tracked files; `dcfs/` has 31 production `.cc` files and 38 test
`.cc` files (42 counting `dcfs/testonly/`), with 787 `TEST`, `TEST_F` or
`TEST_P` cases in them (292, 494 and 1).

**Backlogs** (section 3, 5): clang-tidy allowlist 558 findings in 330 keys
(1354 in 719 keys at first run), report-only 496, guest shell 124 scripts,
bare guest sleeps 50, `FixedArray` 1 entry, `ABSL_` status macros 0.

**The style census (step 25.22).** A one-time pattern count by a low-cost
agent, committed under `docs/plan/notes/style-census-2026-10-10/` (one JSONL
file per rule: break-up, comment-restates-code, confession, hand-rolled
utility, name-repeats-scope, nested-happy-path, repeated-handler,
status-carried). Its own status note is candid: the agent used pattern
detectors over a line scan, not a reading pass, so the judgement-shaped items
came back empty and the counts are line matches. It reported 32 functions
over 60 body lines, 63 hand-rolled utilities in 30 functions and 6 nested
happy paths on 2026-10-10. It was an ordering aid for the sweeps, not a
measurement of quality.

**Review ledger counts:** section 10.

## 16. How to check these claims yourself

From the repository root. Bazel needs `/dev/kvm` only for the QEMU tests; none
of the commands below boots a guest.

```text
# Everything in section 3 to 8: the style gates, in tier "small".
bazel test --config=fast //tools/...

# The clang-tidy / clang-query verdict, with per-check counts and the
# report-only lines in the log.
bazel test --config=fast //tools:style_checks_test
cat bazel-testlogs/tools/style_checks_test/test.log

# The matchers against their known-bad and known-good fixtures.
bazel test --config=fast //tools:style_matchers_test

# Formatting; then format the tree in place.
bazel test --config=fast //tools:format_test
bazel run //tools:format

# Repo-shape rules, banned symbols, raw syscalls, backing users, Tips numbers.
bazel test --config=fast //tools:repo_shape_test \
  //tools:repo_shape_self_check_test //tools:banned_symbols_test \
  //tools:raw_syscalls_test //tools:syscalls_backing_users_test \
  //tools:totw_citations_test

# Warning flags, in our code and in external repositories.
bazel test --config=fast //tools/warnings/... //dcfs:warning_flags_compile_test \
  //dcfs:warning_flags_link_test //dcfs:warning_flags_test_link_test

# The commit-subject gate over a range.
bash .github/ci/commit_subjects.sh origin/main..HEAD

# The compiler flags that apply to our code (section 2).
grep -n 'copt\|linkopt' .bazelrc | grep -i 'werror\|Wno\|Wl,'

# The numbers in section 3.
awk -F'|' '!/^#/ && NF==4 {n++; s+=$4} END {print n, "keys,", s, "findings"}' \
  tools/style_checks_allow.txt
grep -c '^  -[a-z]' .clang-tidy             # denied checks
grep -c '^ban ' tools/banned_symbols.txt    # banned symbols
grep -c '^allow ' tools/banned_symbols.txt  # allow lines

# The repo_shape allowlists (section 5).
grep -v '^#' tools/repo_shape_guest_shell.txt | grep -c .
grep -v '^#' tools/repo_shape_sleeps.txt | awk 'NF {n++; s+=$2} END {print n, "files,", s, "sleeps"}'

# The review ledger (section 10).
python3 tools/agents/review_findings.py summary --repo .

# SLOC by area (section 15).
python3 -I -c 'import importlib.util as u; s=u.spec_from_file_location("d","tools/agents/dashboard_lanes.py"); d=u.module_from_spec(s); s.loader.exec_module(d); [print(r) for r in d.compute_sloc_rows(".")]'

# Coverage and mutation baselines (section 13).
cat dcfs/coverage_baseline.txt
bazel test //tools/mutation:all
```

To see the gates reject something, break a rule in a scratch branch and run
`bazel test --config=fast //tools/...`. For example, add `std::function<void()>
f;` to a `dcfs/*.cc` file and the `banned_std` matcher fails
`//tools:style_checks_test`; add a line `sleep 1` to a guest script and
`//tools:repo_shape_test` fails; call `::mkdir(...)` and
`//tools:raw_syscalls_test` fails; add a commit with a subject such as
`fixed a thing` and `commit_subjects.sh` fails. (Such edits are for a working
tree; nothing in this document asks you to commit them.)

## Where `docs/style.md` and other documents run ahead of the tree

A reader moving between this document and the guide will meet statements
that read as if a check or a state exists. Line numbers are at `ac3374df`
and drift. Another branch is restructuring `docs/style.md` and `AGENTS.md`,
and this list is the input to that work; nothing here edits them.

Statements in the present tense that are false today:

- `docs/style.md:158-162`: the nullability warnings "as errors (7.3's
  `-Weverything` set)"; neither is enabled (step 25.16).
- `docs/style.md:165`: "today 0 annotations"; 7 exist.
- `docs/style.md:124-131`: "the injected syscall interfaces
  (`BackingSyscalls`, `LocalSyscalls`, `ProcessSyscalls`)"; none exist (step
  25.41 is a draft).
- `docs/style.md:1093`: `FATAL` (`CHECK`) as a log level, beside 1.6b's
  no-crash rule.
- `docs/style.md:1677`: shfmt "run by `//tools:format_test`"; the test covers
  C and C++ only. The same applies to the BUILD-layout claim of section 3
  (buildifier "enforced by `//tools:format_test`").
- `docs/style.md:1074-1076`: `happy_path_nested` is described as report-only
  (a count, not a failure); it is enforced, with one backlog finding.
- `docs/style.md:77-78`: names two report-only matchers
  (`status_uninitialized`, `pointer_without_nullability`); `status_dropped`
  is a third.
- `docs/style.md:70`: the checked directories omit `dcfs/testonly/` and
  `test/qemu/guest/`.
- `docs/style.md:1653-1655`: shell and Python line counts are stale (section
  15 has current ones).
- `docs/style.md:1708-1713`: the thin-glue ruling for a new guest script,
  superseded by the `no_new_guest_shell` rule (repo_shape rule 4).
- `docs/style.md`, Appendix A: rows F1, F2 and F6 are false; C3 (9 lines, not
  92), C13 (642, not 292) and D2 (about 207, not 21) are stale; N1's `cache`
  namespace is gone.
- `test/qemu/README.md:1747` and `docs/plan/phases/25-style-convergence.md:1671`
  speak of `-Weverything` as if it were on.
- `AGENTS.md:34-36`: "every test declares `size` and `timeout` explicitly"; a
  reviewer counted 72 of 663 tests without an explicit `timeout`. That count
  was not re-measured for this document.

Mechanisms described in the present tense for checks that do not exist
(planned in the step named):

- `docs/style.md` 1.2: matchers `const_local_value` and for function and
  member pointers, `char_pointer_string`, and the `repo_shape.py` refusal of
  `ABSL_FLAG` / `absl::GetFlag` outside main files (steps 25.20 and 25.24,
  parked).
- `docs/style.md` 1.10a: the confession-phrase check in `repo_shape.py`
  (25.13, parked).
- `docs/style.md` 1.1: `layering_check` as an include rule (7.7).
- Sections 4, 5 and 7 of the guide: shfmt, shellcheck, Python and Markdown
  formatting are conventions only.

Statements that are plans, not errors: `docs/style.md:109-111` and
`AGENTS.md` say `-Weverything -Werror` comes "from phase 7". That is the plan
for step 7.3 and is accurate as a plan; section 2 says it has not landed.

The same sections also describe real checks accurately; the matchers in
section 3 and the `repo_shape` rules in section 5 were each read against the
guide's text when this document was written. The reconciliation of the
guide's text with the tree is item 1 of step 25.39.

## Appendix: the rules, by mechanism

This is the share question answered with a count. `docs/style.md` has no
inventory of its rules by mechanism (producing one is item 1 of step 25.39),
so the table below is the author's own reading of `AGENTS.md` and the guide,
grouped into 59 rules. It is a judgement of grouping, not a count the tree
can reproduce; another reader would split or merge some rows. The mechanism
column is checked against the code cited in the sections above.

States: **E** a test, build action or CI job fails when the rule is broken
(an allowlist of old violations may exist); **R** report-only, a tool prints
findings and nothing fails; **V** review only (a person or a reviewer agent
must notice); **N** not done: the rule is stated, no mechanism exists and
review is not the intended one either; **P** a different or additional
mechanism is planned (the step is named). A rule with a backlog is still E.

| # | Rule | Mechanism today | State |
|---|---|---|---|
| 1 | C and C++ formatting (Google, `PointerAlignment: Right`) | clang-format, `//tools:format_test` | E |
| 2 | Warnings are errors in our code; links fail on warnings | `.bazelrc`, `//dcfs:warning_flags_*_test` | E (narrow set) |
| 3 | The wide warning set (`-Weverything`) | none | N, P (7.3) |
| 4 | Google/Abseil/bugprone/cert/modernize/performance/readability structural rules | clang-tidy, `//tools:style_checks_test` | E (558 backlog) |
| 5 | Every suppression names its check and reason | `suppression_problems` | E |
| 6 | No `std::function`, `std::unordered_*`, `std::chrono` | `banned_std` matcher | E |
| 7 | Abseil container algorithms, not iterator pairs | `iterator_pair_algorithm` matcher | E |
| 8 | No by-reference mutating lambda | `capturing_mutating_lambda` matcher | E (91 backlog) |
| 9 | No `dcfs::` inside `namespace dcfs` | matcher | E |
| 10 | No `Dcfs`-prefixed identifiers | repo_shape rule 9 | E |
| 11 | Short Status macros; no `[[nodiscard]]` on Status | repo_shape rules 10, 11 | E |
| 12 | No `Status::message()` | `status_message` matcher | E |
| 13 | A failed Status is never dropped | `status_dropped` matcher | R |
| 14 | A Status is initialized where declared | `status_uninitialized` matcher | R |
| 15 | Function shape: no else after return, bounded complexity | clang-tidy (threshold 15) | E (39 backlog) |
| 16 | Happy path unindented | `happy_path_nested` matcher (heuristic) | E (1 backlog) |
| 17 | Sized-once buffers are `absl::FixedArray` | repo_shape rule 8 | E (1 entry) |
| 18 | Nullability on every pointer | `pointer_without_nullability` matcher | R, P (25.16) |
| 19 | Include what you use | `misc-include-cleaner` | E (71 backlog); `layering_check` P (7.7) |
| 20 | No intentional crashes in production | `check_in_production` matcher, banned symbols | E |
| 21 | No timers in the daemon | banned symbols | E |
| 22 | No sleeps in guest scripts | repo_shape rule 3 | E (50 allowlisted) |
| 23 | No retry loops | `capturing_mutating_lambda`, SQLite busy-handler ban; the loops listed in section 14 remain | V, P (25.10) |
| 24 | No paths after startup; no subprocesses or plugins | banned symbols | E |
| 25 | Every syscall through `syscalls::` | `//tools:raw_syscalls_test` | E |
| 26 | Only the backing layer reaches the backing filesystem | `//tools:syscalls_backing_users_test`, `backing_door_guard` | E |
| 27 | Wrappers are thin and named for their manpage | review | V |
| 28 | No test-only comments, names or friends in production | repo_shape rules 6, 7 | E |
| 29 | Test code only in `*_test.cc` or `testonly/` | `testonly = 1` and build graph; review | V |
| 30 | Fakes, not mocks | review (no `MOCK_METHOD` in code) | V |
| 31 | No new guest shell | repo_shape rule 4 | E (124 allowlisted) |
| 32 | Every guest script and disabled check is accounted for | repo_shape rules 2, 5 | E |
| 33 | Every gate has a self-check | review | V |
| 34 | Test first, failure quoted | agent report, review | V |
| 35 | Test verdicts do not depend on host load | review; `docs/testing.md` | V |
| 36 | Pointer, time and sentinel typing (strong types, `absl::Time`) | review | V, P (25.35, 25.36, 25.38, 25.39) |
| 37 | Comments: why not what, no echo docstrings, length | review | V, P (25.39 in part) |
| 38 | Every branch names its reachable cause | review; mutation testing (advisory) | V, P (25.13) |
| 39 | One abstraction over variants; branching discouraged | review | V |
| 40 | One way to do each thing | review (agent definitions) | V |
| 41 | Names and namespaces | review | V |
| 42 | Behave like the backing; cite sources; differential test | review; `docs/testing.md` | V, P (citation checker, 25.39) |
| 43 | Bounded memory | review | V, P (26.41) |
| 44 | File names are bytes | review; tests (phase 9) | V |
| 45 | Commit subject starts with a plan step | `commit_subjects.sh`, CI `subjects` | E |
| 46 | Commit body, trailer, test-first quote | review | V |
| 47 | Tips of the Week numbers are real | `//tools:totw_citations_test` | E |
| 48 | Licence headers; third-party READMEs | repo_shape rules 12, 1 | E |
| 49 | What the daemon links is a golden; no known-vulnerable shipped dependency | `shipped_deps_test`, `sbom_test`, CI `osv` | E |
| 50 | Formatting of Bazel, shell, Python, Markdown | none | N, P (7.6b, 7.7) |
| 51 | Errors are built with the `StatusBuilder` helpers, not `absl::XError(StrCat(...))` | review | V |
| 52 | An errno becomes a Status through `ErrnoToStatus` / `ProducedErrnoToStatus`, never `absl::ErrnoToStatus` | indirect (wrappers do it); a direct use is not checked | V |
| 53 | Log levels: ERROR means dcfs produced the failure | review; some end-to-end tests assert at `--v` levels | V |
| 54 | Three kinds of message; the whole Status goes into a replacement message | `status_message` covers `.message()` only; review for the rest | V |
| 55 | One Status serializer | review | V |
| 56 | No `char *` for strings (25.24) | none | V, P (25.24) |
| 57 | No function pointers or pointers to members for callables (25.20) | none | V, P (25.20) |
| 58 | Locals are non-const by default (`misc-const-correctness` is denied) | none | V, P (25.20) |
| 59 | Flags are defined only in main files | none | V, P (25.20) |

Counting the table: **E** is rows 1, 2, 4 to 12, 15 to 17, 19 to 22, 24 to
26, 28, 31, 32, 45 and 47 to 49: 28 of 59 rules (47 %). **R** (report-only)
is rows 13, 14 and 18: 3 rules (5 %). **N** (not done) is rows 3 and 50: 2
rules. The other 26 rules (44 %) are review only: rows 23, 27, 29, 30, 33 to
44, 46 and 51 to 59. Fourteen rows name a planned step that would move them
(rows 3, 18, 19, 23, 36, 37, 38, 42, 43, 50 and 56 to 59).

The share depends on grouping: the first draft of this table had 50 rows
(without rows 51 to 59, which are conventions of section 7 and style 1.2
rules with no check) and gave 56 %. Counting rules the guide states and
nothing checks lowers the share; counting broad, well-checked groups such as
row 4 as one rule raises it. The split by rule count says little about
risk: the enforced rows are the structural ones and most of the review-only
rows are the ones the review ledger shows generating the most findings
(comments, style-other, test shape). Section 10 gives 68 of 176 findings as
tool-checkable ("could" or "checked").

Rule 3 is the cheapest gap to see and the guide's description of the compiler
gate is the one most likely to mislead; sections 2 and 14 say so.

