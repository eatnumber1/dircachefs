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

## 25.4 Test-writing rules (russ, 2026-10-08)

Docs first (style.md's tests section), then the 8.2e tests follow them;
the mechanical conversion of existing tests is 7.5/7.5b.
- **Extend an existing test or write a new one.** A new test when the
  behaviour is new (a new operation, failure mode or invariant) or when the
  setup differs (fixture state, fault injection, mount options, a
  different guest image); its name states the behaviour. Extend an
  existing test when the gap is an unasserted consequence of a scenario
  that test already runs (most mutation survivors are this): add the
  expectation there, in the guest scripts as another `TEST name` check of
  the same scenario rather than another boot. The cost model: a guest
  boot plus setup is the expensive unit, an assertion is free, a harness
  test without a fresh mount is cheap. The debuggability rule that limits
  extending: a failure must say what broke from the test's name plus the
  expectation's message, so an added expectation carries a matcher or a
  message naming the property, and unrelated scenarios never share a test.
- **EXPECT, not ASSERT**, unless continuing would use an invalid value or
  make the later checks meaningless (`ASSERT_OK_AND_ASSIGN` for a value
  the test goes on to use; an `ASSERT` on a precondition every later line
  depends on). One failure must not hide the next.
- **Matchers.** `EXPECT_THAT(value, Matcher)` with gMock and the status
  matchers (`IsOk`, `IsOkAndHolds`, `StatusIs`, `Contains`,
  `ElementsAre`, `UnorderedElementsAre`, `HasSubstr`, `Field`,
  `Property`, `Pointee`) over `EXPECT_TRUE`/`EXPECT_EQ` on computed
  booleans or hand-formatted strings, because the failure message shows
  the whole value and the expectation. Shell guest tests print the
  observed and expected values in the FAIL line for the same reason.
- Owner: a docs commit by an implementer; 7.5/7.5b (matchers, ASSERT to
  EXPECT across existing tests) stay as the conversion step at a quiet
  point.

## 25.5 Style rules: one abstraction over variants; avoid branching (russ, 2026-10-08)

Docs commit to style.md (code section), then applied as code is touched:
- **One abstraction over variants, not an `if` at every use.** When a
  value can come from two sources or a behaviour has two forms (the
  filesystem UUID or the mount-point+fsid fallback; relatime vs noatime;
  a kernel capability present or absent), build one object once from
  whichever applies and give the rest of the code a single interface;
  never branch on the variant at each use site.
- **Branching is discouraged, not forbidden** (russ, 2026-10-08: the rule
  must not come off too strong; sometimes a branch is necessary). Every
  branch is another code path to understand and to test separately, so
  look for the shape that needs none before writing one:
  - inject dependencies and use small fakes instead of test-only
    conditionals: never `if (!in_test) talk_to_db()`; inject the database
    and use an in-memory fake in the test, so production has zero
    test-only branches (AGENTS.md's "fakes, not mocks" and "no code paths
    exist only so a test can run outside the guest");
  - use the numeric or structural properties of values so the absent or
    "off" case needs no test: `int num_foos = 0` rather than
    `optional<int> num_foos` when zero is the right absent value (not
    always: say when it is not), `options.quota = INT_MAX` to turn quotas
    off with no `if` anywhere, an empty set that every loop handles for
    free, a no-op implementation of an interface instead of a null check;
  - one object built once over variants rather than an `if` per use (the
    rule above); tables over if-chains where a table fits.
  `Status` returns with `RETURN_IF_ERROR` are the normal shape of error
  handling and are not what this rule is about.
- `[[nodiscard]]` is permitted (russ, 2026-10-09): expected on functions
  whose return value is the result the caller must handle (a Status, a
  StatusOr, an fd wrapper). Written into style.md by lane-6 with the 15.x
  fix round.

## 25.7 No test-only things in production code (russ, 2026-10-09)

russ: "Style rule: don't add test-only things to prod code. If a test needs
a knob in the prod code, make that knob a real feature of the class /
method. There should be zero `// only use in tests` style comments."
Stronger than AGENTS.md's "test-only code never lives in production
files": a seam a test needs (an injected clock, an observer, a fault
point, a size limit) is designed, named and documented as a feature of
the class, with its production default, and nothing in production code
says it exists for tests. Do, in one step (dcfs-implementer):
- docs/style.md: the rule, with the pattern (constructor injection of an
  interface with a production implementation; options with production
  defaults; observers such as ProtocolEvents, which are the model) and
  the anti-pattern (a method, flag, comment or branch whose only reason is
  a test).
- Inventory: every seam production code exposes that tests use (the
  ProtocolEvents observer and its hooks such as `SqliteTransaction`, the
  `--wrap` fault points, `Checkpoint`, any `*_for_test`, `Hook`, `ForTest`
  name, any comment mentioning tests in dcfs/, bench/, tools/*.c; a first
  grep found one comment, session_loop.h:65 "they nest only in tests", and
  no `Hook`/`ForTest` names in headers); for each, either it is already a
  feature (keep; make the comment say what it is for in production terms)
  or convert it, or move it to testonly/ injection.
- Mechanical enforcement: `tools/repo_shape.py` refuses, in production
  sources, comments that mention tests as the reason for code (`only in
  tests`, `for tests`, `test-only`, `for testing`, `ForTest`, `_for_test`),
  with the known-bad fixture self-check; allowlist entries need a reason.
Owner: dcfs-implementer, next free lane after the P0 queue.
Clarification (russ, 2026-10-09): "Some fakes are meant to be used in
production (such as the no-op trace logger that we use in tests for
formal verification)." A fake that is the production default (the no-op
`ProtocolEvents` the daemon runs with, which the trace tests replace) is
the pattern the rule wants, not a violation; style.md says so.

Status 2026-10-09: 25.7 merged 704f11e (with 25.8 and 7.1d; lane-5). As
built: style.md section 8 (the rule in russ's words, the pattern, the
anti-pattern, the production-default fake as the pattern); repo_shape
`no_test_only_comments` over dcfs/ and bench/ production sources (comments
giving tests as the reason, `ForTest`/`_for_test` names; `tools/*.c` are
the suite's own tools and are not scanned; empty allowlist, a reason per
entry, known-bad self-check failing first); seven comments reworded; one
conversion, `ErrnoNameTable()` removed (the test uses `ErrnoToErrorName`);
inventory verdicts in the commit message (ProtocolEvents and its hooks,
Context::events/clock/interrupts, Checkpoint, FillGuards::max_touched are
features; the `--wrap` fault points are link-time, no seam).

## 25.7b Remove the test peer (russ, 2026-10-09)

The inventory flagged `DirCacheFS`'s `friend testonly::DirCacheFSPeer`,
which the runtime checker's tests use to read the daemon's bookkeeping.
russ: "This is exactly the thing I want to get rid of. A test peer allows
test code to reach into the private internals of a class. Don't do that."
So: remove the friend declaration and the peer. What the checker's tests
read through it becomes observable through a real feature: the
`ProtocolEvents` observer (events carrying the bookkeeping the tests
assert on) or a documented query on `DirCacheFS` with a production use
(statistics, Phase 20, or the idle INFO line), never a getter whose only
caller is a test. Tests first: each test that used the peer is rewritten
against the new feature and shown passing; the peer's header and the
`testonly` target go; repo_shape gains a check that no production class
declares a `friend` from a `testonly` namespace or target (known-bad
fixture). Owner: dcfs-implementer, next free lane; coordinate with 23.11
(which adds a checker rule and may touch the same tests).
Done 2026-10-09, merged a10f58c (lane-5, one commit). As built: a
read-only view `events::Bookkeeping` in protocol_events.h beside the
SharedFd and Lifetime reports; `DirCacheFS` implements it privately and
exposes `bookkeeping()`, which fuse_ops.cc passes to the check hooks on
every request (the production caller; the no-op observer never queries
it); the invariant checker reads only the view (`Lookups`, `Written`,
`HeldFdCount/Limit`, `OpenForWrite*`, `Removed*`, `SharedFileOf`); tests
that break an invariant on purpose edit a copy (`testonly::FakeBookkeeping`,
`InvariantChecker::TamperBookkeeping`), 17 tests rewritten; repo_shape
`no_testonly_friends` (friend declarations naming `testonly::` in
production sources; the "or a testonly target dependency" half is not
checked: add it if a production target ever depends on one); style.md
section 8 updated. Fast 233 + 2; asan on dir_cache_fs_test and
trace_recorder_test green. 23.11's code half will conflict textually in
invariant_checker.{h,cc} and the checker tests on its rebase.

## 25.8 Abseil's Tips of the Week as design guidance (russ, 2026-10-09)

russ: "we adopt all of https://abseil.io/tips/. It's a 'rule', but not as
strong of a rule as the Google style guide or other style rules I've
established in the past. Coding and review agents should use it as design
guidance rather than firm rules." So: docs/style.md gets a section near
the top stating the precedence (AGENTS.md and style.md's own rules, then
the Google C++ style guide, then the Tips of the Week as guidance); a
review finding that rests on a tip cites its number (TotW #NNN) and is
advisory unless it also breaks a rule; agents may read a tip online when
a design question matches one (the index page lists titles). The agent
definitions in .claude/agents/ (implementer, protocol, reviewer) name the
tips as guidance in their instructions. No vendoring of the tips. Owner:
dcfs-implementer, together with 25.7.

