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

## 25.9 No intentional crashes (russ, 2026-10-09)

russ: "no intentional crashes. E.g. no use of `LOG(FATAL)` or `CHECK`
(`RET_CHECK` is ok because it doesn't crash). I understand the necessity
sometimes, but try hard to avoid it (for instance, another style rule
that's in an Abseil TOTW somewhere: don't call methods that can fail
inside constructors, instead add a static Create method). If you
_really_ feel you need a crash, bring it to me for review and I'll
consider on a case-by-case basis."
- docs/style.md: the rule for production code (dcfs/, bench/, tools/*.c
  shipped binaries): no `LOG(FATAL)`/`LOG(QFATAL)`, `CHECK*`, `QCHECK*`,
  `DCHECK*`, `abort()`, `assert()`, `std::terminate`, or an `exit()` that
  stands in for an error return; a failure is a `Status` (`RET_CHECK` for
  invariants, which returns Internal); fallible construction goes through a
  static `Create` returning `StatusOr` (TotW #42 "Prefer Factory Functions
  to Initializer Methods" and the Google style guide's "Doing Work in
  Constructors"); every remaining crash site is listed in style.md with
  russ's approval and the reason (why the process cannot continue
  correctly), nothing else. Test code and `testonly/` may crash (death
  tests, the checking build's invariant abort are gates).
- Mechanical enforcement: `tools/banned_symbols.txt` bans `abort`,
  `__assert_fail` and Abseil's fatal-log internals in the shipped
  binaries; `tools/repo_shape.py` refuses the macros in production
  sources outside an allowlist whose entries carry russ's approval date
  and the reason (known-bad fixture); clang-tidy's equivalent when 7.5
  lands.
- Audit (a subagent, dcfs-implementer, low effort for the mechanical part):
  the first count on 2026-10-09 found in production code 25 `CHECK`
  family sites (23 `CHECK_*`, 2 `CHECK(`: backing.cc:282 after a
  credential switch fails to restore root, fuse_request.cc:279 on a double
  reply), 1 `abort()` (fork_split.h:40, 26.14f's "parent returned"), 1
  `assert(`, 10 `exit`/`_exit` sites (the wrapper's and daemoniser's
  legitimate exits among them), 0 LOG(FATAL); tests hold 4 LOG(FATAL) and
  44 CHECKs, allowed. For each production site: convert (a Status up the
  stack; a `Create` factory where a constructor does fallible work), or
  write the case for russ in one paragraph each (what state the process
  would be in if it continued). Russ decides the list; the allowlist then
  holds exactly his approvals.
Owner: dcfs-implementer, after the current rounds land (budget), before
25.2.

Exception and its inverse (russ, 2026-10-09): "crashes are permitted in
tests when invariants fail that aren't under test. Like if we fail to
write to disk while setting up the database for a test that's checking
whether open() succeeds through dcfs, that's obviously not related to the
test itself, so in test code it's ok to crash (but still prefer to return
errors as appropriate). The inverse applies too. Don't ASSERT/EXPECT
properties that aren't under test. E.g. disk write failures in my example
above should not be an ASSERT (using the googletest ASSERT which
attributes it to a test failure). The way to think about this is that
there's a clear distinction between 'the test failed' and 'the
infrastructure failed to run the test'." So, in the same step: style.md's
tests section says it in those words; setup that is not under test uses
Abseil's plain `CHECK`/`CHECK_OK` (russ: no custom helper, "googletest
understands the difference between 'the process crashed' and
'ASSERT/EXPECT failed'"); the audit converts setup
`ASSERT_OK_AND_ASSIGN`/`ASSERT_THAT(..., IsOk())` in fixtures and test
preambles (the dup of a source fd, a backing mkdir, a database open) to
`CHECK_OK`, and for a `StatusOr` a `CHECK_OK_AND_ASSIGN(lhs, expr)` macro
in the style of `ASSIGN_OR_RETURN` (russ: "use CHECK_OK. I'd also be fine
with adding a CHECK_AND_ASSIGN macro that allows assignment from StatusOr
like how ASSIGN_OR_RETURN works"), defined where `ASSIGN_OR_RETURN` lives
and usable in production code too (it is a crash, so in production only
with russ's approval like any other), leaving ASSERT/EXPECT only on the
property under test (25.4's EXPECT-over-ASSERT rule stands for those);
guest scripts likewise distinguish `fail <check>` (the test failed) from
an infrastructure abort (today `die`/`exit 2` in lib.sh; the harness's
verdict rules already treat a missing RESULT line as a harness failure:
say so in test/qemu/README). Mechanical: repo_shape refuses
`ASSERT_OK_AND_ASSIGN` inside `SetUp()`/fixture constructors of `*_test.cc`
(known-bad fixture); the rest is review.
- Added 2026-10-10: `RestoreRoot` (`backing.cc`) ends in a `CHECK` that the
  thread's credentials are back to root; a production crash for russ's
  ruling (the comment argues carrying on as the wrong user is worse).

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


## 25.11 Xattr writes: one path form for every file type (russ's question, 2026-10-10; queued after the quota reset)

`backing::ApplyXattrOpOn` does a statx for the type and, for a regular
file or directory without an open fd, reopens the object through
`/proc/self/fd/<fd>` for `fsetxattr`/`fremovexattr`, while every other
type takes `setxattr`/`removexattr` on the same `/proc/self/fd/<fd>`
path. The magic link resolves to the same object either way, and the
path form already serves reads of every type (`XattrsOf`, `XattrOf`),
so the reopen branch buys nothing and costs an `open(2)` of the file.
Step: drop the type check and the reopen; apply the path form whenever
no open fd was given (or always: `ProcFdPath` works on the open fd too,
which leaves one lambda per operation and no `ApplyReal`). Keep
`AsCaller` around the syscall; note why the path form works under it
(only fsuid/fsgid/groups switch, the capabilities stay, so the magic
link's ptrace access check passes). Tests: an unprivileged `setfattr`
on a regular file, a directory, a symlink and a FIFO through dcfs, each
read back through the backing, before and after. No behaviour change
intended; the trace model is untouched.

## 25.10 Retry loops out of the daemon (russ's rulings, 2026-10-10; queued after the quota reset)

Per `docs/style.md` 1.12 and `docs/plan/notes/retry-loops-2026-10-10.md`:
(1) unlink, rename and readdir lose `kAttempts` and the EAGAIN fallback
and retry without bound; the model's retry-or-EAGAIN branch becomes retry
only and gains a liveness property (every begun unlink/rename/readdir
eventually begins its mutation or serves its listing) that TLC checks
under the fairness the code provides; the loop comments name it;
(2) `ListXattrOPath`, `GetXattrOPath` and `GetGroups` size once, read
once, and report ERANGE/EINVAL as the backing would; where a FUSE
request carries the client's buffer size it is passed through once;
(3) `tools/repo_shape.py` refuses `kAttempts`, `attempt`, `attempts`,
`retry` and `retries` as identifiers in `dcfs/*.cc` and `dcfs/*.h`
(comments excepted), with no allowlist unless russ approves a loop.
The governing principle (russ, 2026-10-10, style 1.12): each FUSE
operation keeps the contract the backing's syscall gives its caller;
ERANGE-on-races where the backing says ERANGE, atomicity where the syscall
is atomic, however many internal retries that takes. The step audits every
FUSE handler against that question, not only the six loops found, and
lists any handler whose answer can depend on dcfs's own bookkeeping racing
itself. Protocol agent (the model changes), tests first for (2): a reader
racing a writer sees ERANGE once. No behaviour change for (1) today, since one
request runs at a time.

## 25.12 `absl::FixedArray` for buffers of known size (russ, 2026-10-10; dispatched)

Style 1.2: a buffer sized once from a syscall's answer, a header field
or a count, filled once and read, is an `absl::FixedArray`, not a
`std::vector<T> v(n)` or a `std::string(n, '\0')`. Sweep `dcfs/*.cc`,
`dcfs/*.h` and `tools/*.cc` (production files; tests where the same
pattern is a one-line change); the example that set the rule is
`GetGroups` in `backing.cc:2094`. Where a caller expects a vector
(`GetGroups` returns `StatusOr<std::vector<gid_t>>`) the return type
changes too if every caller only reads it, else the FixedArray is the
buffer and the vector is built from it once. The retry loop in
`GetGroups` is left as is (25.10 removes it). Mechanically: a
`tools/repo_shape.py` check refusing `std::vector<T> name(expr);` and
`std::string name(expr, '\0')` in `dcfs/` production files outside
`tools/repo_shape_fixed_arrays.txt` (an allowlist with reasons that only
shrinks, empty if the sweep is complete), with its unit test. Owner:
dcfs-mechanical.

## 25.13 No code for the impossible: survivors block, confessions refused (russ, 2026-10-10, "I agree")

Style 1.10a. Two mechanical checks and one backlog:
1. **Surviving mutants fail the per-push job.** `mutation-changed`
   (.github/workflows/ci.yml, tools/mutation/mutate.py `changed`) exits
   non-zero when a C++ mutant of the touched functions survives the small
   tier and trace validation (today: a summary line). Resolution is a
   test, a deletion, or an `equivalent.txt` entry with a reason; the
   job's summary says so next to each survivor. The TLA+ half (12.13)
   stays advisory for now (its survivors are design findings). First run:
   triage the backlog the last weekly `mutation-survivors` artifact lists
   for the files the next pushes touch; a push that touches a function
   with an old survivor pays for it then, which is the intended pressure.
   Owner: dcfs-investigator for the job, the orchestrator files survivors.
2. **Confession phrases refused.** `tools/repo_shape.py` refuses, in
   comments of `dcfs/*.cc`, `dcfs/*.h` and `tools/*.cc` production files,
   the phrases of style 1.10a ("cannot happen", "can't happen", "shouldn't
   happen", "just in case", "be exact anyway", "defensive", "paranoia",
   "for safety"; case-insensitive, word-bounded), outside
   `tools/repo_shape_confessions.txt` (`path count | reason`, only
   shrinks). Unit-test fixtures as the sleep check has. The first run's
   hits are the backlog: each is deleted, turned into a RET_CHECK that
   names the invariant, or listed with a reason. Owner: dcfs-mechanical,
   after 25.12 (same file, same lane).
3. The agent definitions carry the reachability question (done 2026-10-10).

## 25.14 No `dcfs::` inside dcfs; no `Dcfs` prefix (russ, 2026-10-10; after 25.12, lane-2)

Style 1.3's two new paragraphs. Mechanical agent, two commits, check
first each time: (1) `tools/repo_shape.py` refuses `dcfs::` outside
`#define` lines in `dcfs/*.cc` and `dcfs/*.h` (tests included: they are
in `namespace dcfs` too), with fixtures; then the 38 call sites lose the
qualifier (`dcfs::DcfsErrnoToStatus` 27, `dcfs::ErrnoToStatus` 10,
`dcfs::StatusToErrno` 1; `syscalls_backing.h:141` among them). (2)
`repo_shape.py` refuses identifiers matching `^Dcfs[A-Z]` in `dcfs/*.h`
and `dcfs/*.cc`; `DcfsErrnoToStatus` becomes `ProducedErrnoToStatus`
(russ to confirm the name; the agent uses it unless told otherwise) with
`status.h`'s comments and `docs/style.md` 1.6's mentions updated;
`DcfsMountDevice` (`mount_dcfs.h:86`) is renamed to say what it is (the
source device of a `fuse.dcfs` mount in mountinfo: e.g.
`MountDeviceOfFuseDcfs`, with the contrast named in its comment) or
allowlisted with that reason if no name reads better. No behaviour
change; clang-format; `bazel test --config=fast //...` and
`//tools/...` green.

25.12 merged 2026-10-10 (7c23d43, mechanical agent, two commits): the
repo_shape `fixed_arrays` rule with fixtures; eleven sites converted
(backing.cc six, file_handle.cc two, fuse_request.cc two, main.cc one);
`GetGroups` keeps its vector return (callers and tests hold one) and
builds it once from the FixedArray; one allowlist entry remains
(fuse_request.cc's groups buffer, resized to a second call's count).
Finding for 7.6: the pinned clang-format (22.1.8) reflows hundreds of
lines of the untouched tree, so no format check exists today and the
baseline is not clean; 7.6's one-time reformat is where that lands.

## 25.15 Function shape: guards, straight line, cancellable undo (russ, 2026-10-10; after 25.14, lane-2)

Style 1.6a. Rewrite `SwitchTo` exactly as the worked example (the
read-back checks go: the cause they guarded is unreachable from a FUSE
request, say so in the commit), then the other `absl::Status status;`
accumulators in `dcfs/*.cc` (ten `if (status.ok())` ladders today) where
the preferred shape fits; where something must happen between the
failure and the return that a `Cleanup` cannot express, leave the ladder
and add the one-line reason. Hand-written undo in error branches becomes
a `Cleanup` with `Cancel()` at the commit point. No behaviour change;
the existing tests cover these paths (check `backing_test.cc`'s SwitchTo
cases still pass and still fail on the old bug they were written for).
Also: `RestoreRoot`'s `CHECK` is a production crash; it goes on 25.9's
list for russ, not changed here. Owner: dcfs-implementer (judgement per
site), medium effort. The clang-tidy checks named in 1.6a land with 7.5.

## 25.16 Nullability on every pointer; references where never null (russ, 2026-10-10; after 25.15, lane-2)

Style 1.2's nullability bullet. Implementer, in this order, each a
commit: (1) the warnings: `-Wnullability-completeness` and
`-Wnullable-to-nonnull-conversion` as errors for our targets (how 7.3
scopes warnings to our code; if 7.3 has not landed, the two flags on our
`copts` now), which pass trivially while no file annotates anything;
(2) `dcfs/*.h` first, one header at a time: each parameter or member
that is never null becomes a reference where a reference can go, else
`absl_nonnull`; an optional, non-owning, read-only argument becomes
`absl::optional_ref<const T>` (russ, 2026-10-10); each other that may be
null becomes `absl_nullable`, and its
dereferences are checked to sit behind a null test; `unique_ptr` and
`shared_ptr` members and returns get the qualifier too; the first
annotation in a file turns the completeness warning on for that file,
so each header's `.cc` and tests follow in the same commit; (3) the
`.cc` files' internal pointers; (4) the repo-shape check for a raw `*`
or a `unique_ptr`/`shared_ptr` without a following `absl_nonnull` /
`absl_nullable` in `dcfs/*.h` (declarations only; `*` in expressions is
not a pointer declarator: the check may be approximate and allowlist
what it cannot parse, with reasons). The libfuse callbacks and
`fuse_req_t`-style C types keep their C signatures at the boundary and
get the qualifier on our side of it. No behaviour change; a nullable
pointer found dereferenced without a check is reported, not silently
fixed (it is a bug with a test first). About 30 raw pointers in headers
today, 2 smart pointers.

25.14 merged 2026-10-10 (cf12b7d, mechanical agent, two commits): the
`namespace_qualifiers` rule is scoped by namespace depth (global-scope
uses in main.cc and four test files keep their qualifier, rightly); 40
in-namespace sites dequalified; no ambiguity found; `project_prefix`
rule; the two renames with docs/design.md updated. No allowlist for
either rule.
Amended 2026-10-10 after 25.21: the gate is the `pointer_without_nullability`
clang-query matcher (report-only today, 430 sites: 197 production, 233
test), switched to enforced at the end of this step; no repo_shape check.
A pointer behind a typedef is not seen by the matcher: the sweep greps for
those by hand.

## 25.17 The short status macros everywhere (russ, 2026-10-10; after 25.15, lane-2)

russ: "create macros called `RETURN_IF_ERROR` and `ASSIGN_OR_RETURN` that
are aliases of `ABSL_RETURN_IF_ERROR` and `ABSL_ASSIGN_OR_RETURN`
respectively, then use the shorter versions throughout the codebase."
Abseil ships the aliases behind `ABSL_DEFINE_UNQUALIFIED_STATUS_MACROS`;
25.15's first commit turns that on from `//dcfs:status` and makes
`dcfs/status.h` refuse to build without it. This step: mechanical agent
replaces every `ABSL_RETURN_IF_ERROR(` and `ABSL_ASSIGN_OR_RETURN(` in
`dcfs/`, `tools/` and `bench/` (837 uses in 28 files at count time) with
the short name, reformats the touched lines (shorter names re-wrap),
checks every target that uses them depends on the define (build), and
adds a repo-shape check refusing the `ABSL_` spelling of the two in our
C++ (fixture tests; no allowlist). docs/style.md's remaining mentions
follow. No behaviour change.
Also in this step (russ, 2026-10-10, style 1.2's algorithms bullet):
every `std::<algorithm>(first, last, ...)` whose range is a whole
container becomes the `absl::c_` form from `absl/algorithm/container.h`
(`std::copy` 3, `std::sort` 3, `std::end` 4 today, in dcfs/, tools/ and
bench/, tests included; dep `@absl//absl/algorithm:container`), and a
repo_shape rule refuses the `std::` spelling of the algorithms Abseil
wraps (the list from the header's `c_*` names) outside
`tools/repo_shape_std_algorithms.txt` (`path count | reason`, only
shrinks: a deliberate sub-range is the one reason), with fixtures.
And (russ, 2026-10-10, style 1.2's nodiscard bullet): the 38
`[[nodiscard]]` before `absl::Status`/`StatusOr` return types go (the
type is must-use already); a repo_shape rule refuses the attribute, or
`ABSL_MUST_USE_RESULT`, directly before those two types, no allowlist;
`[[nodiscard]]` on `FileDescriptor` and report-struct returns stays.
And (russ, 2026-10-10, style 1.2's banned bullet): `std::function`,
`std::unordered_map`, `std::unordered_set`, `std::chrono` are refused
outright by repo_shape in all our C++ (no allowlist), and the existing
uses go: `session_loop.h`'s two `std::function` (AnyInvocable or
FunctionRef by ownership: say which and why), the one test's
`std::chrono` (absl::Time/Duration).
Amended 2026-10-10 after 25.21: the repo_shape rules this step planned
for `std::function`/`std::chrono`/`std::unordered_*` (`banned_std`, 49
sites: 2 production, 47 test) and for iterator-pair algorithms
(`iterator_pair_algorithm`, 18 sites) exist as clang-query matchers with
allowlists; this step empties those allowlists instead of adding rules.
The `ABSL_` macro spelling and the `[[nodiscard]]`-before-Status rules
stay repo_shape (text patterns).

## 25.19 Abseil utilities catalogue (russ, 2026-10-10; dispatched, lane-4)

russ: "send a researcher to look across the Abseil codebase to see what
kind of utilities there are, and make a summary for coders and reviewers
to refer to." Investigator reads the pinned Abseil (20260817.0) from the
output base and writes `docs/abseil-utilities.md`: a swaps table
(hand-written or `std::` pattern → Abseil utility, each marked with the
style rule that already demands it), a section per `absl/<dir>` with the
headers that matter and their entry points, what dcfs already uses, the
sites that hand-roll a utility instead (counts, as candidates for a
sweep step), and a ten-question reviewer checklist. Reference, not
essay; states the pin and how to refresh. docs/style.md then points to
it from 1.2, and the agent definitions tell coders and reviewers to use
it. Follow-up: the sweep of candidates (25.20, by count and risk).

25.19 merged 2026-10-10 (7c92256): `docs/abseil-utilities.md`, 463 lines,
read from the pinned headers; about 100 swap rows, per-directory entries
with dcfs's include counts, a ten-question reviewer checklist, 24 sweep
candidates with counts. Pin facts: `status_builder.h`, `Overload`,
`optional_ref`, `linked_hash_map`, `bind_back`, `simulated_clock` exist;
no `Nonnull<T>` aliases (macros only); no `ASSERT_OK_AND_ASSIGN` (ours).
Style 1.2 and the agent definitions now point at it. Items for russ:
`escape.cc` is near `absl::CHexEscape` but Abseil also escapes `'` (keep
unless ruled); `optional_ref<const T>` as a third option beside a
reference and `absl_nullable` (no ruling); the 24 `fprintf`/`std::cout`
sites that are the helpers' own output (usage, `--version`, the fsck
report) are not log lines (ask before changing).

## 25.20 Sweep of the catalogue's candidates (after 25.17; mechanical, lane-2)

From `docs/abseil-utilities.md` section 4, by count and risk, no
behaviour change, each family its own commit, tests first where a
rewrite could change output (StrFormat for printf): string `+` chains
with a literal (21 prod lines, 4 files; `umount_helper.cc:101` the one in
the daemon), `std::to_string` (3), `snprintf`/`printf` (8),
`strtoull`/`atoi` in bench (8), `find() != npos` (4), the `substr`
prefix test (`mounts_below.cc:103`), the three hand-written splits
(`SplitXattrList`, `mount_dcfs.cc`, `bench/tree.cc`) where
`absl::StrSplit` with the right delimiter fits (NUL-separated lists: check
`ByChar('\0')` on a string_view with embedded NULs keeps the bytes rule),
`std::function` in `session_loop.h` (2: `absl::AnyInvocable` or
`FunctionRef` by ownership), `std::map` in bench (1), the two stale alias
includes (`file_handle.cc:20`, `main.cc:41`), `BytesToHexString` in
`file_handle.cc` versus `absl::BytesToHexString`. Tests get the same
treatment in a second pass. Also (russ, 2026-10-10: "Switch"):
`dcfs/escape.cc` is replaced by `absl::CHexEscape` (Abseil also escapes
the single quote; accepted); test first: the escape tests' expectations
change where a `'` appears, every golden that holds an escaped name
(strace goldens, trace fixtures, log-line tests) is re-baselined in the
same commit with before/after quoted, and the bytes rule (names are
bytes, escaped whenever printed, style "File names are bytes") is
re-checked on the call sites. `device_id.cc`'s hex and the `CHECK` family (25.9) stay excluded. The 19
program-output sites (six in dcfs/: three `--version`, usage, fsck's
report; thirteen in bench/) become `absl::FPrintF`/`PrintF`/`SNPrintF`
with byte-identical output as the test (style 1.6a's program-output
ruling, 2026-10-10).

## 25.21 Zero-token style checks: clang-query matchers and clang-tidy readability now (russ, 2026-10-10, "Do it"; dispatched, lane-4)

The pinned LLVM ships `clang-query`, `clang-tidy` and `run-clang-tidy`.
A host-side Bazel test runs, over our C++ with the real compile flags
(a compilation database from the Bazel action graph, or the aspect 7.5
plans; whichever is smaller to land now), (1) clang-tidy with
`readability-else-after-return`, `readability-misleading-indentation`,
`readability-function-cognitive-complexity` (Threshold 15; a finding
reads "break up this function"); widened the same day (russ: "there's a
lot more clang-tidy checks that enforce style guide rules we already have
... Let's turn on all the existing checks that make sense in our codebase
or that enforce rules we have") to 7.5's full set plus `google-*` (the
Google style guide is ours) and `abseil-*` (the Abseil rules of 1.2 and
the catalogue), `WarningsAsErrors: '*'`, a deny-list with a reason per
entry (trailing return types, identifier length, cppcoreguidelines not
enabled, ...), the first run's findings in the shrinking allowlist and
their counts per check as the sweep backlog; this lands 7.5's check set,
leaving 7.5 the aspect if the compilation-database route is taken; and
(2) one AST-matcher file per style rule that is structural: a `Status`/`StatusOr` local declared without an
initializer (1.6a, report-only: preference); an `if` ending in `return`
followed by `else`; a happy path nested in `if (x.ok())`; a `std::`
algorithm whose range arguments are `x.begin()`/`x.end()` of one object
(1.2); a lambda capturing by reference and assigning to the capture
(ruling 1); a pointer-typed parameter, member or return with no
`absl_nonnull`/`absl_nullable` (1.2; report-only until 25.16 lands);
`CHECK`-family calls in production targets (1.6b); `std::function`,
`std::chrono`, `std::unordered_map/set` (1.2); `dcfs::` qualification
inside `namespace dcfs` (1.3; replaces the regex when the matcher is
exact). Each matcher has a known-bad fixture and a shrinking allowlist
(the existing `tools/repo_shape_*.txt` shape); the first run's counts
per rule are the backlog for 25.22's census to skip. Zero marginal
tokens afterwards. Owner: dcfs-investigator (the hermetic wiring is the
work); the matcher files are small and reviewed by the orchestrator.

## 25.22 Style census by a low-cost model, function by function (russ, 2026-10-10; after 25.21)

A Haiku mechanical agent walks every function in `dcfs/*.cc`, `dcfs/*.h`,
`tools/*.cc`, `bench/` (tests in a second pass) with a fixed card of
yes/no items written as detectable patterns with a quoted line each
(confession phrases; a comment that restates the statement under it;
the same three-statement handler repeated in one function; a hand-rolled
utility from `docs/abseil-utilities.md`'s swaps table; a name beginning
with the namespace or project word; program output through iostreams or
the printf family), skipping what 25.21's matchers already report. A
function over 60 lines or over the complexity threshold gets the single
finding "break it up" and no further items. Output: one JSON findings
file per rule with counts and sites, committed under
`docs/plan/notes/style-census-<date>/`, which becomes the ordered
backlog: each rule a check-first-then-sweep step by count. Judgement
rules (reachable cause, one mechanism, what a name distinguishes) are
not on the card. Budget note: Haiku's two sweeps this week (25.12,
25.14) moved the weekly meter imperceptibly; this replaces reviewer
tokens rather than adding to them.

25.15 merged 2026-10-10 (implementer, five commits; first
dcfs-style-reviewer pass: seven findings fixed, six questions ruled by
the orchestrator and russ into style 1.6a): the short status macros
enabled from //dcfs:status with a guard; SwitchTo as the worked example;
`syscalls::setfsuid`/`setfsgid` return a Status (sentinel refused with
EINVAL, read-back, EPERM if it did not take; `fsuid()`/`fsgid()` query
irregulars; the read-back test restored against the wrapper, failing
first); ProbeRecoveredRow helper, FallocateFd's explicit End gone,
RollBack helper, no joined statuses. Ladders kept with reasons:
FallocateFd's carried status (End before the failure's refresh),
Transaction's pair (the pragma restore runs whatever the body returned).
The wrappers' EPERM branch has no test (as root the kernel accepts every
id but the sentinel). Not done here: the other `if (x.ok()) {` nested
happy paths (12 remain; `sqlite.cc`'s RunTransaction next to touched
code), for 25.21's matcher backlog.

25.21 reported 2026-10-10 (lane-4, one commit; merge pending a rerun on
the rebased tip): an aspect (`tools/style_checks.bzl`) runs the pinned
clang-tidy and clang-query as sandboxed, cached actions per cc target
with the real flags; `//tools:style_checks_test` compares the outputs
with `tools/style_checks_allow.txt` (keys file|function|check|count,
719 keys, 1354 findings), `style_matchers_test` checks each matcher's
known-bad/good fixtures, a self-check test. `.clang-tidy`: 7.5's groups
plus google-* and abseil-*, minus clang-analyzer-* (120 of 140 s on
fsck_test.cc, over 20 min on dir_cache_fs_test.cc: a separate large
target later) and a 21-entry deny-list with reasons
(abseil-unchecked-statusor-access segfaults clang-tidy 22.1.8). Top
counts: misc-include-cleaner 560, designated-initializers 60,
cert-err33-c 49, unused-parameters 44, cognitive-complexity 41 (22
production), google-runtime-int 31, implicit-bool-conversion 26,
concurrency-mt-unsafe 26. else-after-return and misleading-indentation:
zero. Matchers enforced: capturing_mutating_lambda 89, banned_std 49,
check_in_production 26, iterator_pair_algorithm 18, happy_path_nested 1,
dcfs_qualified_inside_dcfs 0 (replaces the repo_shape regex, deleted);
report-only: pointer_without_nullability 430, status_uninitialized 18.
Cost: 198 actions, 9 min cold at 4 jobs when a core header changes, 26 s
no-op; placement (fast vs presubmit) is an open question for russ with
the agent's recommendation to follow. 7.5's remainder: the aspect
failing the build itself rather than through the test.

25.22 merged 2026-10-10 (e1d0b95, docs only): the census under
`docs/plan/notes/style-census-2026-10-10/`. Honest status: the Haiku
agent used pattern detectors over a line-scan enumeration (713
definitions in 49 production files), not a reading pass, so the
judgement-shaped card items (comment restates code, repeated handler)
came back empty and the counts are line matches. Useful anyway:
break-up 32 functions over 60 body lines; hand-rolled-utility 63 items
in 30 functions (nine of the top ten in bench/); nested-happy-path 6;
status-carried 8 (advisory); name-repeats-scope 2; confession 1. Test
pass not started. Conclusion for the orchestrator: the tier-1 reading
pass is not something Haiku does on its own; the backlog is ordered from
25.21's tidy counts plus this list, and the sweeps are the next steps.

## 25.23 clang-tidy auto-fix sweeps, by check (after 25.17; mechanical, lane-2)

A `bazel run //tools:style_fix -- --check=<name>` target (the aspect's
flags, clang-tidy `--fix` applied in the workspace, like
`//tools:format` will be) and one commit per check, largest first,
where clang-tidy's fix is mechanical and the tests prove equivalence:
misc-include-cleaner (560), modernize-use-designated-initializers (60),
misc-unused-parameters (44, where the parameter is truly unused: keep
names in overrides), readability-inconsistent-ifelse-braces (36),
modernize-raw-string-literal (32), google-readability-braces (24),
misc-use-internal-linkage (19), performance-noexcept-move-constructor
(12), modernize-use-emplace (11), performance-avoid-endl (8, superseded
by 25.20's PrintF change where it overlaps), modernize-loop-convert (7),
and the under-five checks with fixes. Each commit shrinks
`tools/style_checks_allow.txt` by exactly its findings; `--config=fast
//...` green between commits. Not auto-fixed (judgement, separate
steps): cognitive-complexity 41 plus the census's 32 break-ups (Sonnet,
function by function, 25.24), google-runtime-int 31 (types are a design
choice: 25.25 with a reading), bugprone-unchecked-optional-access 30,
concurrency-mt-unsafe 26 (getenv/strerror: syscalls.h or absl
equivalents), cert-err33-c 49 (unchecked returns of C calls: each is a
Status or a deliberate ignore).
