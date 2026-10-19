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
Status 2026-10-12: Step 25.9 complete (merged cee5005). Converted all first-party
crashes in production code:
- `dcfs/fuse_request.h/.cc`: `ReplyNone` converted to `RET_CHECK(req_.has_value())` returning `absl::Status`.
- `dcfs/fuse_ops.cc`: All 17 pointer null checks converted to `RET_CHECK_NE` inside `Serve` lambdas; redundant `Link` check removed; `Init`/`Destroy` null handling safely logged.
- `dcfs/fork_split.h`: Parent callback returns `int` exit code; `ForkSplit` calls `syscalls::_exit(status)` instead of `std::abort()`.
- `dcfs/startup_channel.cc`: `BecomeDaemon` returns `absl::StatusOr<StartupReporter>`; `DetachDaemon` errors surfaced via `Status`.
- `dcfs/syscalls_backing.h/.cc`: Implemented `PoisonBacking()` and `IsBackingPoisoned()`. Backing syscalls check `IsBackingPoisoned()` and return `absl::InternalError`.
- `dcfs/backing.cc`: `RestoreRoot` sets `syscalls::PoisonBacking()` and calls `fuse_session_exit` on credential restoration failure instead of `CHECK`.
- `tools/style_checks_allow.txt`: All 23 `check_in_production` allowlist entries completely removed and enforced by style checks.

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

**Signals (russ, 2026-10-17, added to 25.10):** first confirm in the code
what happens today to an EINTR a backing syscall returns (reported to the
client? which handlers?). Then: dcfs's own signals never
interrupt a backing syscall. The daemon blocks SIGHUP, SIGINT and SIGTERM
from the start of `main` (before any thread) and reads them through a
`signalfd` in `SessionLoop`, so shutdown happens between requests and a
backing syscall cannot return EINTR because of them; libfuse's
`fuse_set_signal_handlers` goes. Test first: a SIGTERM during a slow
backing syscall (an interruptible wait in the guest) reaches the client
as the syscall's real answer, not EINTR, and the daemon then exits
cleanly. Check whether `SA_RESTART` handlers would still cover a
reachable window (a signal before the mask is set, a forked child, another
thread); add them only for a window that exists, named in a comment (style
1.10a). 25.28c's timerfd shares the loop. The model already starts a clean
shutdown only with no request in flight (`BeginShutdown` in dcfs.tla); the
signalfd makes that true of the code (russ, 2026-10-17, asking whether
the model covers signal delivery): trace validation refuses a trace in
which shutdown begins while a request is in flight, and a test sends
SIGTERM during a slow request and sees the request answered, then a clean
shutdown. Style 1.12 gains: "dcfs's own
signals never interrupt a backing syscall; they are read in the session
loop."

**Split into reviewed slices (backlog order, russ, 2026-10-19: "Yes I approve
proposal 2 as a whole."; `process.md` `rule-reviewed-slices`).**
- **25.10a** the retry loops out of unlink, rename and readdir, the liveness
  property and the `repo_shape` ban. The green half of 12.16a; prerequisite
  12.16a. It retires `atomicity_eagain.cfg` into the model.
- **25.10b** xattr size queried once (ERANGE), tests first. No prerequisite;
  may run in parallel with 25.10a.
- **25.10c** signalfd, before 25.28c's code (they share `SessionLoop`). It
  turns 12.21's "shutdown only idle" row from unenforced to enforced.
  Prerequisite: 25.10a.

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
Also (russ, 2026-10-11, style 1.2's callables bullet): `Scope` in
protocol_events.h takes two pointers to member functions; `begin` becomes
an `absl::FunctionRef<void(Context &)>` parameter (the caller binds its
arguments with `absl::bind_front` or a lambda), `end` an
`absl::AnyInvocable<void(Context &, const absl::Status &)>` member bound
to the events object; every other function-pointer or member-pointer
callable in dcfs/ follows the same split (grep `(*` and `::*` in
parameter and member positions); a `function_pointer_callable` matcher
with the one gtest-table site allowlisted with its reason.
Also (russ, 2026-10-11, TotW #103, style 1.2's flags bullet):
`IsDcfsFlagFile` in dcfs/main.cc matches every file under a `dcfs/`
directory for Abseil's `--help`; all three flags already live in
dcfs/main.cc, so the predicate becomes "ends with `dcfs/main.cc`", its
stale comment ("our flags live in dcfs/*.cc") goes, and a repo_shape
rule refuses `ABSL_FLAG(` / `absl::GetFlag(` outside main files (no
allowlist; fixtures). A test that `--help` lists the three flags under
the installed name, if none exists.
Also (russ, 2026-10-11, style 1.2's locals bullet): the `const_local_value`
matcher under tools/style_matchers/ (a const-qualified local VarDecl of
non-reference, non-pointer type, not constexpr, not static; fixtures
both ways) with today's sites allowlisted; then the sweep drops `const`
from locals where nothing relies on it (a value a later line relies on
keeps it with a word why), about 88 production sites.

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

25.17 merged 2026-10-10 (4b804dc, mechanical agent, eight commits):
repo_shape rules `absl_prefixed_status_macros` (835 uses swept to the
short names) and `nodiscard_on_status` (38 sites); `banned_std` and
`iterator_pair_algorithm` matcher allowlists emptied (session_loop's
callback an AnyInvocable, FunctionRef where only called, a plain function
pointer for the const gtest parameter table, the test stopwatch on
`syscalls::clock_gettime(CLOCK_MONOTONIC)`; `absl::c_copy/c_sort/
c_reverse/c_count_if/c_binary_search/c_contains`); deps declared. Rulings
made here: a captureless callback stored in a const table is a plain
function pointer; the CLOCK_* include-cleaner finding is a tool conflict
(include-cleaner credits only <time.h>, deprecated-headers forbids it in
C++), one allowlist line with the reason, to be settled once in 25.23.

25.23 part 1 merged 2026-10-10 (4608f4f, mechanical agent, three
commits): `bazel run //tools:style_fix -- --check=<name> <files>` runs
one clang-tidy check with `--fix` under the aspect's own flags (a
`.flags.txt` output per source from the aspect, one flag source) and
clang-formats the changed lines; misc-include-cleaner 570 → 393 findings
(46 files fixed, 21 reverted whole because the tool removed a header it
does not credit for a macro (sys/mount.h MS_*, sys/xattr.h XATTR_*,
sys/ioctl.h), picked glibc-internal or kernel headers (bits/*,
asm-generic/*), or added a deprecated C header; those 21 are the hand
work); modernize-use-designated-initializers 60 → 0. Gaps: the fixer
has unit tests of its arithmetic but no end-to-end fixture test (a
py_test sandbox cannot reach the execution root and sysroot; a
`sh_test` with the real toolchain, or a `bazel run`-driven check, is the
follow-up); the CLOCK_* include-cleaner case is unsettled (the tool
credits no header at all for CLOCK_MONOTONIC/REALTIME: a
`syscalls.h` named constant or a deny of that symbol family in the
check's options is tomorrow's decision).

## 25.24 No `char *` for strings: convert at the boundaries (russ, 2026-10-11; after 25.20, implementer, lane-2)

Style 1.2's bullet. (1) The `char_pointer_string` matcher under 25.21's
aspect (a `char *`/`const char *` parameter, return type, field or local
VarDecl; fixtures both ways), with the boundary files' sites allowlisted
(`dcfs/syscalls.h/.cc`, `syscalls_backing.*`, `syscalls_process.*`,
`sqlite.cc`, `fuse_ops.cc`, `fuse_request.cc`) and every other file at
zero after (2). (2) The sweep, by file: `main.cc` (argv →
`std::span<const std::string_view>` or the existing vector of strings,
immediately; the `IsDcfsFlagFile`-style predicates take `string_view`),
`fsck.cc`, `backing_capture.cc`, `umount_helper.cc`, `fuse_request.h`,
`sqlite.h` (its public surface takes `string_view`; the C API calls in
`sqlite.cc` copy to a `std::string` when the API needs NUL termination,
or pass `data()`/`size()` where it takes a length, which SQLite's bind
and prepare do), `bench/`. Wrapper signatures in `syscalls.h` take
`std::string_view` (`openlog(std::string_view ident, ...)`,
`execv(std::string_view path, std::span<const std::string_view> argv)`,
...) and do the `std::string(view).c_str()` copy inside; where a
syscall keeps the pointer past the call (`openlog` keeps `ident`;
`execv`'s argv must be NUL-terminated `char *` arrays) the wrapper owns
the storage or documents the lifetime. (3) The libfuse boundary:
`fuse_ops.cc` converts each callback's `const char *name` on its first
line and passes `string_view` down; `fuse_reply_*` conversions stay in
`fuse_request.cc`. No behaviour change; the bytes rule (names are
bytes; embedded NULs cannot occur in names but can in xattr values,
which are not strings) re-checked at each conversion. Tests follow in a
second pass.

## 25.25 Function-shape sweep of the rest (russ, 2026-10-11; after 25.24, implementer, lane-2)

russ, on `RaiseFileLimit` in dcfs/main.cc: "a good example of a function
that's not following the 'do stuff; handle errors; do other stuff'
paradigm." Its shape today: the nr_open read's happy path sits inside an
`if (value.ok()) { ... } else { log }`; a `const struct rlimit limit`
copy of `*current` (1.2's non-const default, and the copy is not
needed); the two setrlimit attempts and their diagnostics are tangled
across four branches. The shape 1.6a asks for: getrlimit, handle, return;
`want` from a helper `NrOpenOrHardLimit(const rlimit &)` that reads
/proc/sys/fs/nr_open, logs once if unreadable and returns the larger of
nr_open and the hard limit; `setrlimit` to `want`; if not ok, log the nr_open
refusal right there and fall through; `setrlimit` to the hard limit; if
not ok, log that and return; blank lines between the four things (russ,
2026-10-11: the message lives where the failure is found; blank lines
separate the things, error handling is part of its thing). Sweep: this function first, then every function the
`happy_path_nested` matcher and the census list (`nested-happy-path` 6,
`status-carried` 8, the 12 `if (x.ok()) {` sites 25.15's reviewer counted,
`sqlite.cc`'s RunTransaction among them), then the 32 `break-up`
functions and the 22 production cognitive-complexity findings function
by function, each a commit with the shape it got and why, tests green,
no behaviour change; a function whose split needs a new seam gets a
one-line reason and stays. The dcfs-style-reviewer passes over the
result (questions ruled by the orchestrator).

25.23 part 2 merged 2026-10-11 (b49c64f, 18 commits, mechanical agent):
include-cleaner by hand on 27 files (IWYU keep pragmas on the macro
headers, bits/* and asm-generic/* replaced by the public headers,
deprecated C headers to their C++ forms): 390 → 88 findings; then the
auto-fixable checks one commit each: raw-string-literal 32 → 0,
braces-around-statements 24 → 0, noexcept-move-constructor 12 → 0,
use-emplace 11 → 1, loop-convert 7 → 0, and twelve smaller checks
(override, starts-ends-with, for-range-copy, redundant-string-cstr,
inefficient-vector-operation, unnecessary-value-param,
return-braced-init-list, string-find-str-contains, move-const-arg,
concat-nested-namespaces, avoid-endl, unnecessary-copy-initialization).
Left as found, with reasons: inconsistent-ifelse-braces (all 36 inside
gtest macro expansions, where fix-its do not apply); use-internal-linkage
(its fix is `static`, which trips use-anonymous-namespace; the right fix
is an anonymous namespace: judgement, 25.25's list); CLOCK_* (clang 22's
include-cleaner maps no header to the POSIX clock ids; two allowlist
lines with the reason, a tool gap); six checks whose fix changed nothing
or created another finding. Remaining for the judgement steps:
unused-parameters 44, cert-err33-c 49, google-runtime-int 31,
unchecked-optional-access 30, concurrency-mt-unsafe 26,
cognitive-complexity 41 (25.25), use-internal-linkage 19.

25.9 rulings (russ, 2026-10-11) and the step as it now stands, for the
temporary orchestrator: (1) RestoreRoot: no crash; on a failed restore it
sets a process-wide poisoned flag and calls fuse_session_exit, and the
syscalls_backing door refuses every later backing syscall with a Status
(style 1.6b's poisoned-subsystem pattern); test with the setgroups fault
fake: the daemon ends orderly, no backing syscall follows. (2) ForkSplit:
`parent` returns the exit status and ForkSplit `_exit`s with it; the abort
goes. (3) fuse_ops: the impossible null checks deleted; cannot-refuse-INIT
logs ERROR and exits the session. (4) The audit's other conversions as
written (ReplyNone to RET_CHECK; the exit sites that are ordinary child
exits stay, they are not error returns in disguise; BecomeDaemon returns
StatusOr). (5) Mechanical enforcement: the clang-query matcher
`check_in_production` already exists (23 sites allowlisted, enforced); its
allowlist empties with this step; `tools/banned_symbols.txt` gains `abort`,
`__assert_fail` and Abseil's fatal-log internals for the shipped binaries
(it has none of them today). Protocol agent; test first. Design question
left open by russ: the backing door as an injected class.

## 15.13 The cache database is root-owned 0600, always (russ, 2026-10-11)

russ: "today the cache database is not _guaranteed_ to be owned by root
... Just make it owned by root and drop the complicated logic." main.cc's
rule that the cache file's permission set may grant no more access than
the backing's (the comparison and its error naming both sets, lines
~119-182) goes: the daemon creates the database 0600 as root, refuses an
existing file that is not owned by root or not 0600 (no fix-up, no
WARNING path: the administrator fixes it), and the README's section on
cache permissions says that one rule. Tests: the permission-comparison
tests become the two refusals. No back-compat. Mechanical.

## 25.27 Mechanical duplicate-code detection (russ, 2026-10-11; queued for after the Gemini pool resets (2026-10-23 20:07 PDT); russ 2026-10-18: "Yes. Enqueue 25.27")

Queued 2026-10-19. Runs on Gemini via agy/foreman, as already reassigned.
The threshold and the allowlist come back to russ before commit. It pairs
with 25.45 (mechanize what reviewers keep finding).

russ: "let's schedule some time to investigate mechanical tools for
measuring code duplication. I've seen several cases where there's clearly
duplicate code in our codebase, and I want to catch that without relying
on AI or human review and prompt the code author to refactor instead of
duplicate." The 2026-10-11 plan was a single investigation and gate. It is
superseded by the split below (2026-10-19).

russ, 2026-10-19: "I'd like to be maximalist here. Parallel types, identical
function bodies, copied _expressions_ (whitespace doesn't matter), etc. Open to
more ideas too."

Scope: C++ only, that is `dcfs/` and the C++ test and tooling code. russ,
2026-10-19: "No. We don't care about style for languages we don't ship. This
is why I demanded that the test and tooling code be written in C++. Python is
a bit of a wart, but I may call for a rewrite there too eventually." So shell,
Python, Starlark, TLA+ and docs are out of scope.

Evidence (backlog analysis, 2026-10-19): production C++ copied-line duplication
is 0.4%. The leak is reinvented helpers: 10 of 342 findings. So the detectors
target reinvention and parallel shapes as well as copied lines.

Split by detector, one review round each. The gate: a `small` test whose
baseline only shrinks. New clones fail it and name both sites; existing ones
go on a shrink-only allowlist. Fuzzy detectors are advisory until they are
precise enough to gate. The threshold and the allowlist come back to russ
before commit.

Backlog slot (2026-10-19): M0, after 25.48 and 25.49, in the order 25.27-0,
25.27a, 25.27b, 25.27c, 25.27d; the pool resets 2026-10-23 20:07 PDT.

- **25.27-0 Prior art (read-only, first).** russ, 2026-10-19: "is there prior
  art we can reuse?" Run on Gemini via foreman `--single --read-only` once
  25.48 lands. Already enabled in `.clang-tidy`: `abseil-*`, `modernize-*`,
  `bugprone-branch-clone`, `misc-redundant-expression`. Evaluate:
  1. Clang's `alpha.clone.CloneChecker` (static analyzer, in the pinned clang
     22.1.8, off; marked "Enable only for development"): AST subtree hashing,
     renamed clones, suspicious copy-paste. Its findings and false-positive
     rate on dcfs.
  2. PMD CPD and jscpd as C++ token-clone options, measured against
     CloneChecker: runtime and packaging cost (CPD needs Java, which the TLA+
     tooling already pins; jscpd needs Node).
  3. `readability-magic-numbers` and `cppcoreguidelines-avoid-magic-numbers`
     (in the pinned clang-tidy, off because of the syscall layer): counts
     outside the syscall layer; candidate: on there, through a per-directory
     `.clang-tidy`.

  Output: which of 25.27a-d reuses what, and what remains custom (header-
  constant literals, one wrapper per syscall, per-diff similar-symbol
  suggestions).
- **25.27a AST engine.** libclang from the pinned LLVM toolchain, hermetic in
  Bazel. Hash the AST subtrees of expressions and statements above a size
  threshold: exact, and with identifiers and literals abstracted (copied
  `_expressions_`, whitespace does not matter). Identical or near-identical
  function bodies after local renaming, and gapped clones (a copy with
  statements inserted or changed).
- **25.27b Constants and literals.** Numeric literals equal to a system-header
  constant (`kBtrfsMagic`); repeated literals without a named constant;
  equal-valued, similarly named constants; duplicate string literals.
- **25.27c Parallel types and reinvented helpers.** Same-shape structs; wrappers
  of the same resource (`Fd` and `FileDescriptor`); identical strong types;
  more than one wrapper per syscall; clang-tidy `abseil-*` and relevant
  `modernize-*` checks, plus matchers for hand-rolled `StrJoin`, `c_find` and
  `StrSplit` (the swaps in `docs/abseil-utilities.md`); test bodies that differ
  only in literals (parameterize them).
- **25.27d Per-diff advisory.** For each new function or type in a diff, the
  three most similar existing ones, shown to the author and the fact-check
  reviewer. Advisory, not a gate.

Owner: Gemini via foreman for 25.27-0; not assigned for a-d (the orchestrator
chooses). Dependencies: 25.48 for 25.27-0. Not before the reset.
Reassigned (russ, 2026-10-17: "Sure"): to Gemini through agy, in lane-8
after 25.32b and the dashboard's SLOC tool, under process.md rule 4
(write work, reviewed by the orchestrator); the gate's threshold and any
allowlist come back for review before they are committed.

## 25.28 Periodic SQLite WAL checkpoints tied to sync_interval (russ, 2026-10-12)

Under continuous read loads, WAL checkpoint starvation can occur because
no mutations trigger write transactions and readers hold locks.
On request arrival, if `opts_.sync_interval` has elapsed and no sync
point is due (`!SyncDue(now)`), run `sqlite3_wal_checkpoint_v2` with
`SQLITE_CHECKPOINT_PASSIVE`, which folds committed frames back into the
main database without blocking readers or requiring timers (a permitted
exception to the "no timers" rule per russ, 2026-10-12). If a sync point
is due, `SyncBackingNow` performs the sync point and updates
`last_wal_checkpoint_ = last_sync_`. Completed in 3a0785a.


## 25.9b Finish "no intentional crashes" (review of Gemini's 25.9, 2026-10-16; protocol agent)

The 2026-10-16 review of 25.9 (which went in unreviewed) found the
conversion incomplete and some of it wrong. Test first for every item.

1. **A setgroups fault test.** The credential switch's `setgroups` failure
   path has no fault test; add one with the link-time `--wrap` fakes.
2. **Banned symbols.** `abort`, `exit` in the daemon, `CHECK*`,
   `LOG(FATAL)`, `assert` and `std::terminate` go in the banned-symbols
   gate for production code, each existing use converted or listed with
   russ's approval (no-intentional-crashes rule).
3. **Delete unreachable null checks** added by 25.9 where the type or the
   caller already guarantees a value.
4. **INIT cannot refuse.** When INIT fails in a way the kernel offers no
   refusal for, the daemon calls `fuse_session_exit` and leaves, rather
   than carrying on half-initialised.
5. **No `g_fuse_session`.** Remove the mutable global; `SessionLoop` checks
   the poisoned state itself (style 1.6b, the poisoned-subsystem pattern).
6. **Nonzero exit when poisoned.** A daemon that stopped because the
   backing was poisoned exits nonzero, and a test asserts it.
7. **One poison guard.** A single guard on the way into every backing
   wrapper, and a test that walks every wrapper and shows it refuses once
   poisoned (the 49-site sweep's list is the inventory).
8. **ForkSplit.** Revert the void-parent branch added to
   `dcfs/fork_split.h` under 7.2/7.4 (test-only code in production) and fix
   `cov_fork_fixture` in its own test code instead.
9. **FORGET double reply.** The FORGET path logs and replies twice on one
   error path; one reply, one log line, with a test.
10. **Docs.** style.md 1.6/1.6b, design.md and the README's log-message
    table match what the code does.

## 25.28b The sync interval bounds the cache's durability lag (russ, 2026-10-16; protocol agent, after 25.9b)

Merged 2026-10-18 (9d6468d3).

Replaces 25.28's separate WAL-checkpoint clock (reviewed: it never runs
under writes, because each sync point resets its clock without
checkpointing, and its premise, reader starvation, does not hold for a
single-connection daemon). russ's reason for approving the original stands
and is the step's purpose: fills and phase-3 commits are written under
SQLite's synchronous=NORMAL and stay unsynced until the next kSync commit
or a 1000-page autocheckpoint, so on a read-mostly server a power cut can
discard a long stretch of cache work, which recovery then re-reads from
the backing, spinning disks up. The step: at each sync point, and on the
same cadence when only unsynced cache commits are pending (no dirty
backing state), the cache's WAL is made durable (fsync, then a PASSIVE
checkpoint); 25.28's MaybeCheckpointWal, last_wal_checkpoint_ and its
request-arrival trigger go (one mechanism, the sync point); the false
"approved exception to the no-timers rule" comments go; the
WalCheckpointed event gets its row in formal/README.md's event table; the
TLA+ durability abstraction (12.14) gains the bound ("a power cut loses at
most one sync interval of cache commits") as a checked property; a
power-cut guest test shows fills older than one interval survive and
younger ones may not; README and design.md say it; the unreachable
SQLITE_BUSY swallow and the closed-connection early return in
Connection::Checkpoint go, and its test-only out-parameters and unused
modes with them. Cancellation: the flush is a backing-free step but sits
at a request boundary; keep it off the request path (the sync point's own
place) so no user request waits on it.

**As built (step-25.28b, 2026-10-18, before its merge).** Two of russ's
rulings changed the step as written above.

- **The durable point is a WAL fsync, not "fsync, then a PASSIVE
  checkpoint"** (russ, 2026-10-17, on FSYNC's cost: "2 fsyncs at most").
  `sqlite3::Connection::MakeDurable` fsyncs the WAL through SQLite's
  `SQLITE_FCNTL_JOURNAL_POINTER` and the file's `xSync`, only when
  `CommittedSinceDurable()` (SQLite's data version) says something
  committed since the last one. A checkpoint would add a database fsync
  and a WAL-restart header fsync. SQLite's 1000-page autocheckpoint is the
  only routine checkpoint; a clean shutdown truncates. `Connection::
  Checkpoint` is the shutdown's TRUNCATE alone (the passive mode, the
  SQLITE_BUSY swallow, the closed-connection return and the out-parameters
  are gone).
- **FSYNC and FSYNCDIR are sync points that run no syncfs** (russ,
  2026-10-17, Q1: "A client asking for FSYNC doesn't expect a syncfs";
  wording corrected 2026-10-18: they are a file's sync points, the
  periodic one is the periodic sync point). They pass the
  backing fsync through and make the cache's durable point before the
  reply: no syncfs, no ClearDirty, and the interval's clock is not reset.
  The litmus outcomes the backing allows after an fsync are pinned as
  non-guarantees (`formal/no_stronger/*.cfg`).
- **The periodic sync point** runs at the first request at least one
  interval after the last, dirty or not: its backing half (syncfs, then
  ClearDirty) only if the dirty set may be non-empty, then the durable
  point after the request's reply and before the next request
  (`DirCacheFS::FinishSyncPoint`; FORGET and BATCH_FORGET finish it too).
  25.28's `MaybeCheckpointWal`, `last_wal_checkpoint_` and its trigger are
  gone. With no timer the bound holds at requests only
  (`limitations/durability_idle.cfg`); 25.28c adds the timer.
- **The model**: `formal/durability.tla` (the clock; `BoundAtRequestEnd`,
  `LostOnlySinceSync`, `FsyncedDurable`; limitations for a failed durable
  point and for idle; known bugs for the code before 25.28, 25.28's, and
  an FSYNC that skips the durable point), and `dcfs.tla`'s `CacheDurable`
  and `FsyncDurable` with the action property `FsyncedCacheDurable` (the
  `fsync_skips_durable_point` known bug). Guest tests: `fault_power`'s
  `lag` and `lagclean` scenarios, `sqlite_durability` around an FSYNC's
  and a periodic sync point's durable points.
- **A failed durable point** (the Opus review's S1/S2): logged at
  WARNING at a sync point, `EIO` to an FSYNC or FSYNCDIR, and not repaired
  by a later one, since Linux reports a writeback error once
  (`Documentation/filesystems/vfs.rst:718`, Linux v7.3-rc7) and SQLite's
  recovery stops at the gap. The docs say a power loss can then take
  every later commit, dirty marks included, until a power loss or a clean
  shutdown's TRUNCATE checkpoint (a same-boot crash and restart keeps the
  on-disk gap);
  `durability.tla`'s `failed` stays set until `PowerLossAndRestart`.
  25.28d's poison replaces this.
- `dcfs.sync_interval_sec` must be positive and finite as a duration:
  the converted `absl::Duration` is checked, so 1e300 (infinite) and
  1e-300 (zero) are refused as 0, negatives, NaN and inf are.

Decisions with an alternative: the durable point after the reply rather
than before it (the request whose sync point it is does not wait; one
that arrives meanwhile does), and a failed FSYNC's durable point modelled
as setting the same sticky `failed` as a sync point's (`FsyncFails`),
rather than only replying `EIO`.

Note from 25.28b's re-review (clerk batch 12, 2026-10-18): the branch was
rebased onto 0068a1ba (not current main) and kept the citation
`fs/fs-writeback.c:64 (Linux v7.3-rc4)`. It must be re-verified at v7.3-rc7
when it is rebased again.

## 25.28d A failed cache fsync poisons the cache (russ, 2026-10-17; after 25.28b)

Linux can drop pages a failed fsync could not write and report success to
the next fsync, so after one failed WAL fsync a later commit can look
durable while an earlier frame is missing, and SQLite's recovery stops at
that gap. russ: "I care about a simple codebase in addition to all the
other properties. What we're talking about here should be extraordinarily
rare. Let's do a blanket poison." No in-process reinitialisation (the
kernel holds inode numbers that live in the database).

- When the cache's WAL fsync fails (a durable point, or a phase-1
  synchronous commit), the request gets EIO, dcfs renames the cache
  database's files aside (`dcfs.db` -> `dcfs.db.poisoned`, and its
  `-wal`/`-shm` likewise; russ: "Maybe rename it to a .poisoned file for
  forensics at a later time"; an older `.poisoned` set is replaced) so the
  next start cannot recover a gapped WAL, and the daemon ends through
  25.9's poisoned path with a non-zero status, logging where the files
  went. The next start creates a fresh database (the first-mount path). A
  failed rename is logged; nothing more is tried.
- Test first: a `--wrap` fault on the WAL's fsync; the EIO, the renamed
  files, the non-zero exit, and a clean start afterwards.
- The model: a durable point may fail only by ending the run with the
  cache discarded (no recovery from it); docs say so.

Owner: dcfs-protocol, small.

**Split into reviewed slices (backlog order, russ, 2026-10-19: "Yes I approve
proposal 2 as a whole."; `process.md` `rule-reviewed-slices`).**
25.28d, 25.28f and 25.28c each split into model and tests (red) first, then
the code: the model halves in M1's gap-proving order (25.28d, then 25.28f.1's
measurement as its own investigation, 25.28f, 25.28c with `ClockTest`), the
code in the same order, with 25.10c before 25.28c's code. 25.28e follows
25.28d. `limitations/durability_idle.cfg` is violated today and is retired
by 25.28c.

## 25.28e Durable cache for O_SYNC and O_DSYNC opens (review S5 on 25.28b, 2026-10-18; after 25.28d)

Milestone M1, after 25.28d (russ, 2026-10-19: "Yes, and yes.").

Passthrough O_SYNC, O_DSYNC and RWF_SYNC writes are durable on the backing
without a FUSE_FSYNC (fs/backing-file.c:95, 227; fs/fuse/passthrough.c:74,
v7.3-rc7). So a client's synchronous write can be durable on the backing
while dcfs's cache has not yet reached its durable point.

- Before replying to an OPEN or CREATE with O_SYNC or O_DSYNC, make the
  cache durable (a WAL fsync), so the cache is no less durable than the
  backing for the writes that open allows.
- Test (fault_power, C++ guest test): create a file with O_SYNC, and one
  with O_DSYNC; write; cut power without FSYNC; after recovery the file's
  NFS handle (inode identity) still resolves. Fails first on the current
  code.
- Model: the durable point on a sync OPEN/CREATE in `dcfs.tla`, with a
  `formal/known_bugs/` variant that skips it.

Owner: dcfs-protocol.

## 25.28f The periodic sync point fsyncs its dirty directories, not the backing's syncfs (russ, 2026-10-18; queued after 25.28b merges, not started; owner dcfs-protocol)

Milestone M1 (russ, 2026-10-19: "Yes, and yes."). Its model and tests
come before its code; 25.28f.1's measurement is its own investigation.

russ, 2026-10-18: "our periodic writeback should sync the database, not the
filesystem the database is on ... I'm expecting us to run syncfs only one
time: when the user runs syncfs on us."
russ, 2026-10-18, on the options: "Yes. Do option 1."

Today the periodic sync point runs syncfs on the backing, then ClearDirty,
then the cache's durable point (one WAL fsync).

Decided design (option 1). The periodic sync point does, in order:

1. For each directory D in the dirty set, fsync D on the backing. If that
   fsync succeeds, ClearDirty(D). A failed fsync keeps D's mark.
2. Make the cache durable (the WAL fsync, or a database checkpoint).

Why: a dirty mark says a mutation in D may not yet be durable on the backing.
Clearing it needs D's entries durable, and a directory fsync gives exactly that
on ext4, xfs and btrfs. syncfs also writes back unrelated data (client file
data through passthrough), so it does more I/O than the guarantee needs. The
point does I/O only when something was mutated, so an idle or read-mostly
daemon never spins up a disk, and dcfs behaves like the backing, only faster
(`docs/design.md`, "Behave like the backing").

- syncfs runs only where the kernel asks for it: FUSE_SYNCFS (a client's
  syncfs(2) or sync(2)) and, if the design needs it there, a clean unmount.
- The dirty set is cleared only after the backing made D durable: by the
  periodic point's fsync of D, by a SYNCFS the client asked for, or by a clean
  shutdown, plus any narrower clearing the model's regimes make legitimate.
  The model states each one.
- Between sync points the dirty set grows with the directories mutated. After
  a power loss, recovery re-probes those directories. The bound is what the
  backing itself could have lost.
- Terms (russ's 2026-10-18 ruling, recorded in `docs/plan/log.md`): FSYNC and
  FSYNCDIR are sync points of a file. They are not syncfs points. The periodic
  sync point is no longer a syncfs point after this step.
- Rejected, recorded: clearing only on a client's syncfs or a clean shutdown
  (the dirty set and recovery grow between them); keeping syncfs (unrelated
  writeback).

**Measurement sub-step 25.28f.1, before the design is final** (russ, 2026-10-18:
"is per-directory fsync slower than syncfs?"). The design above is decided;
the benchmark checks its cost, and the result goes in a note under
`docs/plan/notes/`.

- Benchmark the periodic sync point on ext4, xfs and btrfs with 1, 100 and
  10,000 dirty directories: per-directory fsync against one syncfs.
- Count backing I/O (block-layer writes and flushes) as well as time, with
  client file data dirty on the backing, to show syncfs's unrelated writeback.
- Expectations to verify, with kernel citations (v7.3-rc7): on ext4 and xfs the
  first directory fsync forces a journal or log commit carrying all pending
  metadata, so later directories already committed are cheap. On btrfs a
  directory fsync goes through the log tree and can be costly for large
  directories, or fall back to a full transaction commit.
- If btrfs is slow at large counts, fix within the design (ordering, spreading
  the fsyncs over periods) before any syncfs fallback.

Consequences to design and model in the same change (AGENTS.md, "The
protocol has a model"):

- `docs/design.md`: the periodic sync point and the rule that a mark clears
  only after its directory's fsync.
- The TLA+ model: a per-directory durable step (which fsync persists what,
  under each backing regime), ClearDirty guarded by that fsync (SyncDriven and
  the periodic sync, the ClearDirty guards), a failed directory fsync keeping
  the mark (interacting with 25.28d's failed-fsync handling), and a
  `formal/known_bugs/` variant that clears without the fsync. Trace validation,
  the fault_power scenarios and the ClockTests follow.
- The five guest tests that drive the periodic point with `date -s` change
  with it: `fault_ace.sh:188`, `fault_power.sh:430` and `:613`,
  `sqlite_durability.sh:294`, `syscall_traces.sh:416`.
- Test first: a fake backing that records calls shows the periodic sync point
  issues one fsync per dirty directory, clears each only after its fsync
  succeeds, issues no syncfs, and that FUSE_SYNCFS still issues one; a
  directory whose fsync fails keeps its mark. It fails on the current code.

Interacts with 25.28c (the interval clock), 25.28d (a failed fsync poisons
the cache) and 25.28e (O_SYNC and O_DSYNC opens). Ordering: after 25.28b
merges.

## 25.28c An idle daemon reaches its sync point: a one-shot timer, modelled (russ, 2026-10-17; after 25.28b)

25.28 built the "periodic" checkpoint as a clock check on request arrival,
so an idle daemon never reaches a sync point: its last interval's commits
stay losable until the next request (25.28b's model states the bound only
at request ends; `limitations/durability_idle.cfg` shows the wall-clock
property violated). russ (2026-10-12) approved tying the checkpoint to
`--sync_interval_sec` as an exception to the no-timers rule; asked on
2026-10-17 whether a real timer is that exception, he answered "This
definitely needs to get added to the formal model too", and confirmed:
"same intent".

- **The timer:** a one-shot `timerfd`, armed when something becomes
  losable or dirty with no sync point pending (a commit since the last
  durable point, a dirty row), to fire one interval after it; disarmed by
  a sync point that leaves nothing losable. `SessionLoop` waits on
  `/dev/fuse` and the timerfd together. When it fires between requests,
  it runs the periodic sync point (25.28b's shape: syncfs if dirty, then
  its one kSync commit). An idle, clean daemon never wakes, so disks spin
  down; the timer fires once after the last write.
- **The model, in the same change** (AGENTS.md, "The protocol has a
  model"): `durability.tla` gains the timer (armed, firing between
  requests) and the wall-clock property 25.28b's limitation config shows
  violated today (every commit a power loss may lose is younger than
  Interval, unless a sync point failed) becomes a real property, its
  limitation config retired into the model. `dcfs.tla` if the sync point
  between requests interleaves with anything it models.
- **Tests first:** a ClockTest in which commits are followed by no request
  and the injected clock passes the interval: a sync point runs (fails
  today); the guest `fault_power` lag scenario with an idle tail.
- **Docs:** design.md's "the one timed action" made true, the exception's
  comment at the timer naming russ's approvals (2026-10-12, 2026-10-17),
  README's idle limitation removed.
- Coordinate with 25.10's signal handling if it moves signals to a
  `signalfd` in the same loop.
- **Clock:** measure the sync interval on `CLOCK_MONOTONIC` (the timerfd
  too), not the wall clock: a backward step of the wall clock would suspend
  sync and durable points, while kernel writeback ages dirty data by jiffies
  (fs/fs-writeback.c:2033-2037, v7.3-rc7). Review finding S4 on 25.28b,
  2026-10-18; the orchestrator ruled it under "behave like the backing".
  Moving the interval to CLOCK_MONOTONIC stops the guest tests that drive
  the periodic sync point with `date -s` from driving it, so each needs
  another way to drive it. They are `test/qemu/guest/fault_ace.sh:188`,
  `fault_power.sh:430` and `:613`, `sqlite_durability.sh:295`, and
  `syscall_traces.sh:416` (`fault_power.sh:782` and `:785` step the clock
  back and forth and do not drive it).

Owner: dcfs-protocol, then an Opus review.
Related (clerk batch 12, 2026-10-18): 26.36's follow-ups suspect that
`write_test_ext4`'s I/O budget varies with the wall-clock sync interval,
because sync-point writes are counted by time. When this step lands, that
budget must be measured against the timer, not against the wall clock.

## 25.29 A pinned local copy of Google's style guides for reviewers (russ, 2026-10-17)

`docs/style.md` extends Google's C++, Python, Shell and Markdown guides but
only links them online: the style reviewer's definition never mentions them,
and Gemini's sandbox has no network. russ chose a pin over a live fetch: the
guide changes slowly, a bump shows its changes as a diff, and reviews stay
reproducible and offline.

1. Fetch github.com/google/styleguide at a commit with `http_archive` and a
   sha256 (third_party/google_styleguide/: BUILD overlay, README with the
   update procedure and the licence: CC-BY-3.0, verify from the archive).
2. Render cppguide.html, pyguide.md, shellguide.md and docguide/style.md to
   Markdown with the pinned pandoc as Bazel targets, plus one target that
   puts them in a stable place reviewers can read
   (`bazel build //third_party/google_styleguide:guides`, outputs listed in
   the README).
3. A small test that the rendered guides contain a few known section
   headings, so a bad render or an upstream move fails loudly.
4. docs/style.md cites the local copies next to the links; the style
   reviewer, reviewer, implementer and protocol agent definitions say where
   the local copy is and to read the relevant section rather than rely on
   memory; process.md's Gemini prompts point at it.
5. docs/licenses.md gets the row. The pin is bumped with the other pins
   (no timer), and a bump's diff of the guides is read for new rules.

Implementer, medium effort.

## 25.30 Every suppression carries its reason (russ, 2026-10-17) (merged 2026-10-17, de8bd57)

russ first asked, of a NOLINTNEXTLINE added in 25.9b, "what's the purpose of
ratcheting the allowlist if we just suppress?", then ruled: "I like the
nolint comments over a separate allowlist, since it acts as an apology for
breaking the style guide. However all nolint comments must have an
explanatory comment attached." So a deliberate exception is a NOLINT at
the site, with its reason; `tools/style_checks_allow.txt` is the legacy
backlog of findings to fix, which only shrinks and never gains a line.
Deliverables:

1. A gate in the style checks: every `NOLINT`, `NOLINTNEXTLINE` and
   `NOLINTBEGIN` in our code names its check(s) and has an explanatory
   comment on the same line or the line directly above (not just the check
   name), with a self-check (good and bad fixtures). The existing ones meet
   it.
2. docs/style.md 1.1 says so: fix the code where it can satisfy the check;
   otherwise a NOLINT naming the check, with the reason beside it; the
   allowlist is backlog only. Where an allowlist line is really a
   deliberate exception (the seven `dcfs/testonly/*.cc` link-time fakes for
   `misc-use-internal-linkage`), it becomes a NOLINT with its reason and
   the line is deleted.

Implementer for 1, mechanical for 2's conversions.

## 25.31 clang-tidy never sees header-only libraries (from 25.30) (merged 2026-10-17)

25.30's implementer found that `style_findings_here` (tools/style_checks.bzl)
analyses only the first header-only library in a package: they all share
`srcs = None`, so every later one is skipped, and clang-tidy has never
analysed those headers (`dcfs/ret_check.h` among them). Fix the macro so
every header-only library's headers are analysed. The findings it then
surfaces cannot go on the allowlist (25.30): each is fixed, or becomes a
NOLINT naming its check with its reason. Show the count of new findings
before fixing. Implementer, mechanical for a large batch of fixes.


## 25.32b Style review fixes for 25.32 (from the post-merge review, 2026-10-17; merged 2026-10-17, 212786d, Gemini Flash)

The post-merge style review of 8158196 found no correctness bug. To fix:
`make_index.py` gets `main(argv)` with argparse and `encoding="utf-8"`;
the plan step goes in each module docstring; `test_index.py` becomes
`index_test.py` (and 25.29's `test_headings.py` becomes `headings_test.py`);
`tempfile.TemporaryDirectory` with cleanup replaces `mkdtemp`; both pin
READMEs say how the sha256 was obtained; tip 122 is described as an uncited
spot check; comment the title split and the exact `published: true` match;
`html.unescape` titles and write `_posts/<file>` in the index; one blank
line in `tools/BUILD.bazel`. The citation regex becomes the reviewer's
whole-file, `\s+`-tolerant form (no bare `tips/`, no `TotW 25.32` false
hit), with tests for a wrapped citation, a version number and two
citations on a line. docs/style.md section 5 gains the reviewer's
decisions: `TemporaryDirectory` with cleanup, `encoding="utf-8"` in every
`open()`, `<name>_test.py`, prose scans read whole files, and one
`TotW #N` per cited tip.
## 25.33 A Status is never dropped from a message (russ, 2026-10-17)

russ, on style question 3 (bench diagnostics): "*ALWAYS* include the Status
if you have one in the message. Dropping a Status without logging or
returning it silently suppresses the root cause of errors! Also an aside:
Status implements AbslStringify. So it should be `StrCat("open ", path,
": ", status)`."

- **The rule, in `docs/style.md` 1.6/1.7:** a failed `Status` that is not
  returned is logged or put into the message that replaces it, whole:
  `StrCat(what, ": ", status)` or `LOG(...) << status`, never
  `status.message()`, which drops the code and the errno payload. Bench
  diagnostics (`SkipWithError`), fsck findings, test helpers and tools
  included.
- **The sweep:** every `.message()` of a `Status` in `dcfs/`, `bench/` and
  `tools/` (19 in production files on 2026-10-17: `bench/dcfs_bench.cc`,
  `dcfs/backing.cc`, `dcfs/fsck.cc`, `dcfs/testonly/invariant_checker.cc`)
  and in tests. Where a user-facing line changes (fsck's usage and
  findings now print the code), update its tests and README text, and list
  every changed line in the report for russ.
- **Mechanical check:** `tools/repo_shape.py` (or a clang-query matcher in
  `tools/style_matchers/`, whichever is exact) refuses `.message()` on an
  `absl::Status`/`StatusOr` in those directories, with no allowlist unless
  russ approves an entry. With its unit test.
- **Dropped statuses:** investigate a matcher for a `Status` whose failure
  branch neither returns, logs nor stringifies it (e.g. `if (!s.ok())
  return false;`, `SkipWithError("failed")`). Build it if it is exact
  enough to run with no allowlist; otherwise list what it finds, fix those
  sites, and report the design.

Owner: dcfs-implementer; a style review before merge.

As built (2026-10-17, step-25.33): `status_message` (enforced, no allowlist) and
`status_dropped` (report only, 21 findings: 3 in production, all answers or
carried statuses; 8 in bench; 10 in tests) in `tools/style_matchers/`. Every
`.message()` of a Status is gone from `dcfs/`, `bench/` and `tools/`. Bench
`Start`/`WaitMounted`/`IsMounted`/`RssBytes`/`DropCaches` still return bool or a
sentinel and are the remaining `status_dropped` bench findings.
`StrCat(status)` includes the "=== Source Location Trace: ===" lines
(AbslStringify uses kWithEverything); the invariant checker's one-line console
text uses `ToString()` for that reason.

## 25.34 Guest tests in C++: the helpers, the rule, a first port (russ, 2026-10-17)

russ, asked whether we should write shell at all: "I suspect no, since we
have very little tooling (e.g. no clang-tidy) and less developed style
guides around it"; on shell being natural for "mount, run this as uid
1000, crash, remount, compare": "perhaps, but we can do it in C++ with only
a few helper functions just fine. ... and harder-to-measure things like
style are better developed in C++". About 21,700 lines of guest shell
(`test/qemu/guest/`) exist today; `qemu_cc_test` already runs C++ tests in
the guest.

- **The helpers** (`testonly`, beside `dcfs/testonly/`): the few
  functions a guest scenario needs, each returning `Status`/`StatusOr`
  per style 1.6: mount and unmount dcfs (options as the mount helper takes
  them), run a function as a uid/gid/groups (fork, `setresuid`, wait,
  carry its result back), crash the daemon (SIGKILL) and a power cut
  where the harness supports one, remount, list and compare a tree
  (names as bytes, attributes, the backing against the mount), and the
  shared assertions the shell `lib.sh` gives today (a check's name, the
  wanted and actual values, the backing's answer). Derive the set from
  what `lib.sh`, `fault_lib.sh` and two or three scripts actually use;
  nothing speculative.
- **A first port:** one medium guest script (not `credentials.sh`, which
  17.2e is changing, nor ones 26.36 is editing), ported to a
  `qemu_cc_test` with the same checks and the same names, shown equivalent
  (the C++ test fails where the script fails: break the behaviour once
  and show both fail), the script deleted. Report lines before and after.
- **The rule:** `docs/style.md` section 4 and `AGENTS.md`: no new shell.
  New guest tests are C++ (`qemu_cc_test` with the helpers); shell is
  allowed only as thin glue (the guest `init`, Bazel wrappers, a few
  lines that `exec` a binary). A mechanical check (`tools/repo_shape.py`)
  refuses a new `*.sh` under `test/qemu/guest/` and `test/qemu/` outside
  a list of the existing files, which only shrinks.
- **The rest of the port** is listed, not done: each remaining script with
  its size and what it needs from the helpers, in order of value (most
  edited, most fragile first), as numbered follow-ups. Existing scripts
  port when a step would otherwise change them substantially.

Owner: dcfs-implementer; an Opus review of the helper API before the
port is merged, since every future guest test is written against it.

## 25.32 A pinned copy of Abseil's Tips of the Week (russ, 2026-10-17: "OK") (merged 2026-10-17, 8158196)

The Tips of the Week are adopted as design guidance (`docs/style.md`,
russ 2026-10-09) and cited by number in reviews, from memory today, and
not at all by a reviewer without network. Like 25.29's style guides:

- **Pin** `github.com/abseil/abseil.github.io` (Apache-2.0; the tips are
  Markdown in `_posts/`, 86 published, TotW 1 to 234 with gaps) as an
  `http_archive` with a sha256, in `third_party/abseil_tips/` with a
  README (why, the pin, how to update, licence) and an entry in
  `docs/licenses.md`. No render step is needed (already Markdown).
- **An index:** a build output mapping tip number to title and file
  (the files are named by date), so "read tip 122" is one lookup.
- **A test** in the style of 25.29's `headings_test`: the archive still
  has every tip we cite.
- **A citation check:** every TotW number cited in `docs/style.md`,
  `docs/abseil-utilities.md`, `docs/design.md` and C++ comments (forms
  like "TotW 103", "tips/103", "Tip of the Week #103") is a published
  tip, so a number cited from memory that does not exist fails. With its
  unit test.

Host-only. Owner: dcfs-implementer; a style review before merge.

As built (2026-10-17): `@abseil_tips` pins abseil/abseil.github.io at
`e9e24835cb88` (sha256 in `third_party/abseil_tips/README.md`);
`//third_party/abseil_tips:tips` builds `index.tsv` and `tips.md` (every tip
under `# TotW #N: title`, in place of one file per tip); `index_test`,
`make_index_test` and `//tools:totw_citations_test` (unit tests over fixture
trees plus the real tree) are the checks. The archive has 85 published
`totw` posts for 83 tips (TotW 3 and 5 are published twice; the later file is
taken) and TotW 132 unpublished, not 86. The tree cites TotW 42 and 103 only;
both are published, so the check finds nothing today.

## 25.33b Three kinds of message, and waitexit tested exactly (russ, 2026-10-17; first part done 2026-10-17, the waitexit test remains queued)

russ: "there's three types of messages: 1. I succeeded in doing the thing 2.
I failed in doing the thing 3. The infrastructure underneath me failed ...
It's really 3 where the full debugging payload for a Status comes into
play"; and "ok" to the mechanism:
- Kind (2) outcomes stop being Statuses where practical: fsck's findings
  become plain results (the FsckReport's words), benchmark skips say what
  could not be measured.
- Where a lower layer's Status is itself the domain answer, the caller
  turns it into finding text through one named helper, `FindingText(const
  absl::Status&)` (its message), which the `status_message` matcher
  exempts; nothing else may call `.message()`.
- Kind (3) keeps the whole Status, full payload, in logs and in report
  lines alike.
- Classify every line 25.33 changed (fsck, bench, the invariant checker)
  and the provisional `ToString()` uses its fix round added; docs/style.md
  1.6 states the three kinds with russ's words.

Also (from 26.36's review, russ: "why do we need to let a broken waitexit
slip past?"): a unit test of `testutil`'s `waitexit` with `-Wl,--wrap`
fakes of `pidfd_open` and `poll`: a first "not ready" must make it poll
again, a readable pidfd must make it return 0; failing first against a
`waitexit` that returns at once. boot.sh's guest check stays as an
end-to-end smoke test.

Owner: Gemini (agy) for the sweep and the test, a style review after.

**boot.sh ports to C++ next** (russ, 2026-10-17: "I think this is the
kind of 'large changes' that trigger the rule of rewriting shell scripts
in C++. Maybe it's time to rewrite boot.sh."): after 25.34 merges, the
first follow-up of its port list is `test/qemu/guest/boot.sh` as a
`qemu_cc_test` on 25.34's helpers, same checks and names, shown
equivalent by one deliberate breakage, the script deleted. Its `waitexit`
and `waitpath` checks stay end-to-end smoke tests; the "does not return
early" claim lives in the exact unit test above.

As built (2026-10-17, step-25.33b): `FindingText(const absl::Status&)` in `dcfs/fsck.cc`
is the single exempt helper in `tools/style_matchers/status_message.query`; domain outcomes
stop carrying Status where practical (benchmark skips say what could not be measured);
Kind 3 infrastructure failures retain the whole status; `waitexit` unit test
(`//test/qemu:waitexit_test`) with link-time `--wrap` fakes of `pidfd_open` and `poll`.

### Merge record (clerk batch 17, 2026-10-18)

- **25.34 merged 2026-10-18** by fast-forwarding main to 45b64446, so the
  shfmt hashes in `.git-blame-ignore-revs` (34fa44b9, 67f7230f) stay valid.
  Two style-review rounds (`docs/plan/review-findings.tsv`, rounds 1 and 2);
  their outcomes are fixed/merged, and the Q rows are decided. The merge
  includes status_wire (SerializeStatus and ParseStatus, testonly, protobuf
  36.2), `testonly::RunCommand`, `execv(span)`, `poll(Duration)`, and
  DcfsMount with an optional pid_t (pidfds deferred to 25.42).
  kernel_facts_test is a manual weekly `qemu_cc_test` with its own image; it
  fixes CI run 39138000104's bench_smoke SQLITE_FULL.
- Evidence: fast 98 pass; presubmit removed_test (three targets), testonly,
  syscalls tests and kernel_facts (three) pass; bench_smoke_test_{xfs,btrfs}
  pass under coverage. Lane 5's xattr differential test is unblocked.

### Follow-ups from the 25.34 fix round and 17.6 (clerk batch 12, 2026-10-18)

- **A mount start races an immediate unmount.** `DcfsMount::Start` returned
  as soon as the mount was visible; an unmount straight after it made the
  daemon die with "The FUSE mount of /mnt/twice is not in
  /proc/self/mountinfo" (exit 32), because the mount is visible before the
  daemon's own mountinfo check runs. Decide whether dcfs tolerates an
  unmount that races its start (behave like the backing: a native mount can
  be unmounted at once). 25.34 works around it by statting the mountpoint in
  `Start`. Owner: dcfs-protocol (mount namespaces). Open for russ's decision.
- **Note from 17.6:** `test/qemu/guest/kernel_facts.sh` becomes a
  `qemu_cc_test` once these helpers land.
- **InChild's early-return reap is untested.** `InChild`
  (`test/qemu/guest/guest.cc`) reaps the child through an early-return
  Cleanup when reading its socket fails, and no test covers that path. The
  implementer said there was no practical way to test it. russ (2026-10-18,
  "Our syscalls wrapper will allow this. No?"): it can be tested. Before
  25.41: a guest_test variant with a `-Wl,--wrap` fake of `read` that fails,
  asserting the child is reaped (no zombie: `waitid` reports it gone). After
  25.41: `InChild` takes `LocalSyscalls`, and the test injects a failing
  read. Queued, not started. Owner: dcfs-implementer. Cross-referenced from 25.41's migration list (step 9).

## 25.35 Strong types for IDs and sentinels (russ, 2026-10-17; queued, M2)

Dependencies: after the dependency-injection step, to be written.

Add `dcfs/strong.h` with `StrongInt<Tag, Rep>` (explicit construction,
`.value()`, comparisons, `AbslHashValue`, `AbslStringify`, arithmetic only
where the tag opts in, an optional per-tag validity check run by a
`StatusOr` factory and a `consteval` constructor) and `StrongString<Tag>`
(owning `std::string`, same hooks, optional validity check). Each lands
with its first users: `InodeId` (today `using InodeId = int64_t`,
`metadata_cache.h:57`), FUSE nodeids (reject 0), backing inode numbers,
generations, `fuse_gen`, `Populated::stub`'s id-0 sentinel,
`LookupResult::id = 0`, pids, and file descriptors' -1.
Note: `InterruptionPoint` (22.5) is a dedicated literal-only class and is
not built on these.

## 25.36 Codebase-wide strong-type scrub (russ, 2026-10-17; queued, M2)

Dependencies: after 25.35 and the dependency-injection step, to be written.

Convert the remaining typedef'd ints and sentinel-carrying values to
strong types, starting with `syscalls::` signatures that take `uid_t`,
`gid_t`, `mode_t`, `dev_t`, `ino_t`, `pid_t`, `off_t`, and raw fds. One
module at a time; behaviour-neutral; the tests prove it.

## 25.37 `syscalls::` buffers as spans (russ, 2026-10-17; queued, M2)

Dependencies: after 25.36 and the dependency-injection step, to be written.

Every `syscalls::` wrapper taking a pointer and a length (`write`,
`pwrite`, `read`, `pread`, `getdents64`, `getxattr`, `setxattr`,
`listxattr`, `readlinkat`, ...) takes `std::span`. The codebase's
convention is `std::span` (102 uses, no `absl::Span`), and
`docs/style.md` already writes it. `docs/abseil-utilities.md`'s swaps
table says the same for `(const T *p, size_t n)`, with that reason.

## 25.38 `syscalls::` closer to libc, but typed (russ, 2026-10-17; queued, M2)

Dependencies: after 25.37 and the dependency-injection step, to be written.

Russ's decisions:

1. Flag bitmasks are typed: `OpenFlags`, `AtFlags`, `RenameFlags` and
   `Mode`, so swapped flags, or a mode passed where flags belong, do not
   compile.
2. `DirFd` is distinct from `FileDescriptor` for the `*at` calls.
   `AT_FDCWD` is unrepresentable as a `DirFd`.
3. `PathComponent` is a strong string of bytes: non-empty, no `/`, not `.`
   or `..` unless the call asks for them, escaped when printed. The name
   comes from POSIX's "pathname component". Russ is on the fence; the
   orchestrator chose it.
4. Data buffers are `std::span<std::byte>`. Names and link targets stay
   byte strings.
5. `WriteAll` and `PreadFull`-style helpers for short transfers are not in
   `syscalls.h`, which mirrors libc calls only. They go in a separate file,
   e.g. `dcfs/file_io.h`.
6. Times are `absl::Time` and `absl::Duration`, provisional on a precision
   check first. Verify that `statx_timestamp` and `timespec` (int64 seconds
   plus nanoseconds, `UTIME_NOW` and `UTIME_OMIT`) round-trip through
   `absl::Time` and `absl::Duration` without loss across the full range,
   testing the extremes. Only then does the codebase convert, not urgently
   (russ clarified). A converted time or duration becomes `absl::Time` or
   `absl::Duration` as early as possible after it enters the program: where
   it is pulled out of an incoming struct.
7. Calls that return a new descriptor return the owning `FileDescriptor`.
8. Nullability on every remaining pointer, enforced by a clang-tidy check.

Compile time first (russ): fcntl's operation becomes a template argument,
with a runtime overload only where the value must come from a variable.
Likewise the other operation-selector arguments (ioctl request codes, prctl
options).

Every rule in this step gets a mechanical check: a clang-tidy or
clang-query matcher, or a type that makes the wrong code not compile.

## 25.39 Mechanize every style rule (russ, 2026-10-17; queued, M2; raised in priority 2026-10-18)

Dependencies: after 25.38.

Priority (test-placement analysis 2026-10-18): about 60 % of should and
blocker review findings are style, docs, comment or citation findings that
only readers catch today, so this is the bigger lever on review cost. It
goes first in the M2 queue.

Russ: "remember to add mechanical enforcement for all the style rules we
can!"

Steps:

1. Inventory every rule in `docs/style.md` into a table with the columns
   rule, mechanism today, and why not mechanical. The mechanism is one of:
   clang-tidy, a clang-query matcher in `tools/style_matchers`,
   `banned_symbols`, a type that makes the wrong code not compile, or
   review only.
2. Add a check for each rule that can have one, with good and bad examples
   like the existing matchers. Each new check fails first on a bad example.
3. First candidates:
   - Time values. Ban raw time types (`time_t`, `struct timespec`,
     `struct timeval`, `struct statx_timestamp`) and integer variables or
     fields named `*time`, `*_sec`, `*_ns`, `*_ms` or `*_us`, outside the
     `syscalls::` boundary and the conversion helpers. Code must write
     `absl::Time t = FromStatx(st.stx_mtime)` or similar.
   - Nullability on raw pointers (clang-tidy).
   - No in-band sentinels where detectable: comparisons of IDs, pids or
     fds against 0 or -1 outside the boundary.
   - `std::optional<std::string_view>`, or an optional of a type with an
     empty state: the absence-versus-emptiness rule.
   - Immediately invoked capturing lambdas returning `Status` (1.6a).
   - TLA+ comment length (1.9 applied to `formal/`).
   - Kernel citations naming a version and no host paths: grep for
     `~/Sources` in code comments.
   - Kernel-citation checker (test-placement analysis 2026-10-18): every
     `path:line (Linux vX)` in code and docs is checked against that tag in
     the pinned kernel source. Wrong line numbers were review findings in
     22.5, 17.6 and 25.28b.
   - No production caller (russ, 2026-10-18, asking whether a dead-code check
     would have flagged `syscalls::kill` and `syscalls::waitpid`). Today
     `-Weverything` catches only unused statics, locals, parameters and
     private fields; the coverage gate counts a function as covered when only
     its own test calls it; mutation testing is weekly. The check links every
     shipped binary (`dcfs`, `mount.dcfs`, the umount helper, `fsck.dcfs`, and
     the shipped tools) with `-ffunction-sections -Wl,--gc-sections
     -Wl,--print-gc-sections` for our code, collects the functions of ours
     that the linker discards (no shipped caller), and fails on any not in an
     allowlist with a reason. A test-only caller does not count, and
     `testonly` targets are excluded. The allowlist only shrinks: a new entry
     fails the test. It is a Bazel test, so it runs before review. Scope
     (russ, 2026-10-18: "the linker wouldn't catch it, since it's still
     called from production code. The kernel just never sends the
     message"): this check catches only functions with no production caller,
     the leftovers after a deletion. It does not catch a handler the kernel
     never invokes; that is 12.27's branch-reachability check on the model
     with FUSE passthrough (items 11 and 12 there).
     Guidance (orchestrator, 2026-10-19): where a startup refusal makes a
     runtime-conditional path impossible, make the path unrepresentable by
     type (for example, a passthrough handle type), so the compiler rejects
     the dead branch instead of the check finding it.
4. Items for this step, not done by the clerk:
   - `tools/agents/foreman.py`'s review prompt (`run_review`) takes the
     findings block format from the reviewer agents
     (`.claude/agents/dcfs-reviewer.md`, `dcfs-style-reviewer.md`, the
     "End every report with a findings block" section). That is a code
     change: it lands here, not as a clerk edit.
   - `tools/agents/review_findings.py` and
     `docs/plan/review-findings.tsv` do not exist yet. The orchestrator
     appends findings through `review_findings.py`; the script is a step
     item here.
5. Diff coverage in CI. Every line and branch that a change adds or
   touches is covered by a test, or is listed in `docs/coverage.md` with
   its reason (AGENTS.md: "new code arrives fully covered"). Today's gate
   checks only that the total does not drop below the baseline, so a new
   uncovered branch passes while the total stays above it. This catches
   code that never runs: 25.34's untested `RET_CHECK` in `syscalls::system`
   and 22.5's interrupted-getattr branch. It is a Bazel target with declared
   inputs (AGENTS.md: "Everything goes through Bazel"), so its results cache
   soundly and CI runs it like any test; no script outside the graph.
6. Mutation runs on changed code as a routine merge gate, with
   `tools/mutation/mutate.py` run through Bazel as in item 5. A surviving
   mutant blocks the merge (style 1.10a). This catches code that runs but
   that no test checks: 25.34's `setgroups` never asserted, both-sides-fail
   comparisons, and `InodeOf`'s dropped status. Evidence:
   `docs/plan/review-findings.tsv`, where test-gap is the largest
   substantive category (8 of 54 on 2026-10-17).

**Split into reviewed slices (backlog order, russ, 2026-10-19: "Yes I approve
proposal 2 as a whole."; `process.md` `rule-reviewed-slices`).**
- **25.39a** diff coverage on changed code (item 5). No prerequisite.
- **25.39b** the mutation gate on changed code (item 6). Prerequisite: the
  Bazel pattern of 25.39a.
- **25.39c** the style-rule inventory and the first checks, one rule per
  commit. Prerequisite: 25.38 (the DI interfaces exist).
- **25.39d** the "no production caller" check. Prerequisite: 25.38.
  Kernel citations live in 25.45 item 1.

## 25.40 Support FUSE_STATX (russ, 2026-10-17; queued, M2)

Dependencies: none among the style steps; the M2 row lists it after
25.39.

The kernel (FUSE protocol 7.39, Linux 6.6; cite `include/uapi/linux/fuse.h`
and `fs/fuse` by version and path when written) can pass the caller's
statx mask to the daemon. dcfs requests from the backing by the rule
"fetch the minimum needed, measured in backing operations and their
cost": request what the reply needs, do not trim fields that come free,
and request costly fields (e.g. STATX_DIOALIGN) only when asked. dcfs
caches everything statx returned (`stx_mask` says which fields are valid).

Steps:
1. Check libfuse support first (the lowlevel op and the capability). If it
   is missing, say what it takes; a libfuse patch joins the planned
   patches list, in `docs/plan/notes/kernel-patches.md` or its libfuse
   counterpart `docs/plan/notes/libfuse-passthrough.md`.
2. Where the kernel sends FUSE_STATX, dcfs requests the fields the reply
   needs, and caches every field the reply returns. Where the kernel does
   not send FUSE_STATX, dcfs falls back to GETATTR.
3. Tests run with and without FUSE_STATX, so both paths are covered.
4. Compatibility (russ, 2026-10-17): support runs at least back to Linux
   7.0, his Ubuntu 24.04 LTS HWE kernel. State the tested kernel floor in
   `README.md`.

## 25.41 Replace link-time `-Wl,--wrap` fakes with dependency injection

Status: approved 2026-10-19 (drafted 2026-10-18 by Gemini 4 Argon, high).
russ approved this step on 2026-10-19 ("Yes, and yes.", to the orchestrator's
question), on the strength of his 2026-10-18 rulings quoted below. It unblocks
25.35, 25.36 and 25.38, which go to M2 module by module, one library per
commit and about 800 changed lines per review round (`process.md`
`rule-reviewed-slices`). Order: `BackingSyscalls`, `LocalSyscalls`,
`ProcessSyscalls`, then the fault tests. The chain rewrites call sites that
protocol steps also edit in `dir_cache_fs.cc`; pause it during 22.7 if the
conflicts bite.

The full draft, with its inventories of the `-Wl,--wrap` uses and the
`syscalls::` layer (Appendices A and B), is in
`notes/dependency-injection-2026-10-18.md`. The step text below is the
draft's step, numbered 25.41 (the draft's working number was 25.34b, and
25.34 is guest tests).

**Relation to prior decision ([`docs/design.md:3292-3312`]).** On 2026-10-16 `docs/design.md` recorded keeping `-Wl,--wrap` after reverting `attic/virtual-syscalls` because that branch (1) collapsed [`//dcfs:syscalls`], [`//dcfs:syscalls_backing`], and [`//dcfs:syscalls_process`] into one library, weakening [`//tools:syscalls_backing_users_test`] ([`dcfs/syscalls_backing_users.txt:1-64`]); (2) left two ways to call a syscall (`RealSyscalls::Default()` and free functions); and (3) used virtual-inheritance mixins (`public virtual Syscalls`). This step executes the deliberate design sketched at [`docs/design.md:3307-3312`] ahead of steps 25.35–25.38 ([`docs/plan/phases/25-style-convergence.md:1363-1437`]) so strong types and span buffers land once on the final interfaces.

**Why.** Link-time wrapping relies on glibc symbol names (`openat64`, `fstatfs64` at [`dcfs/BUILD.bazel:1126-1134`]), fragile IA-64 C++ mangled names ([`dcfs/BUILD.bazel:1100`], [`dcfs/BUILD.bazel:1341-1348`], [`dcfs/BUILD.bazel:1429-1432`]), inlining avoidance across translation units ([`dcfs/BUILD.bazel:1439-1440`]), global mutable state in tests ([`dcfs/dir_cache_fs_test.cc:134-203`]), and reserved-identifier exceptions ([`.clang-tidy:35-37`], [`tools/raw_syscalls_test.py:15-17`]).

**Design options evaluated.**
1. **Three narrow virtual interfaces mirroring the three Bazel libraries, injected by reference (Recommended).**
   - `BackingSyscalls` in [`//dcfs:syscalls_backing`], `LocalSyscalls` in [`//dcfs:syscalls`], and `ProcessSyscalls` in [`//dcfs:syscalls_process`]. Each library provides its `Real*Syscalls` implementation; `testonly/` provides `Delegating*Syscalls` wrappers holding `inner_` by reference (single inheritance, no virtual inheritance).
   - *Rulings (russ, 2026-10-18):* the interfaces are pure virtual (`BackingSyscalls`, `LocalSyscalls`, `ProcessSyscalls`). The `Delegating*Syscalls` test fakes implement the interface and hold the real implementation by reference (composition); they never derive from `Real*Syscalls`. libfuse gets its own small seam (`FuseLowlevelOps`), separate from the syscall interfaces.
   - *Rules check:* No test-only knobs in production; the injected object is the real operating-system boundary of [`Context`] and standalone tools. No `CHECK`/abort in production. Preserves [`dcfs/syscalls_backing_users.txt:1-64`] and [`tools/banned_symbols.txt:1-147`].
   - *Hot-path cost:* Cache hits do zero backing syscalls (`SlopeTest`, [`dcfs/dir_cache_fs_test.cc:8460-8503`]). On cache misses and mutations, one indirect call (~1–2 ns) precedes a kernel transition and filesystem operation (~300–5,000+ ns) — <0.5% overhead, verified by [`//bench:dcfs_bench`].
   - *Nth-call faults:* A `CountdownFault` helper in `testonly/` decrements a counter on matching calls and returns `ErrnoToStatus(err, op)` when `++calls == fail_at` (for [`dcfs/dir_cache_fs_test.cc:288-306`], [`dcfs/dir_cache_fs_test.cc:486-495`]) or while `times-- > 0` (for [`dcfs/fsck_test.cc:52-67`]).
2. **Template parameter (`template <typename Syscalls = RealSyscalls>`) on callers.**
   - *Rejected:* Zero dispatch cost, but [`dcfs/backing.cc`] (2,555 lines), [`dcfs/dir_cache_fs.cc`] (4,072 lines), [`dcfs/file_handle.cc`], [`dcfs/device_id.cc`], and [`dcfs/fsck.cc`] would either move into headers or require explicit instantiations of production `.cc` files for every test fake, breaking `.o`-level symbol enforcement in [`tools/banned_symbols.txt:117`] and bloating compile times. And [`FaultSitesTest`] still needs runtime dispatch across hooks. (Compile-time selection *is* used in step 25.38 for the typed `ioctl<OP>`/`fcntl<OP>` free function templates, which dispatch to per-op members of the interface.)
3. **Per-thread or per-instance fault table inside `syscalls::`.**
   - *Rejected:* Violates [`docs/style.md:505-511`] (step 25.7: no test-only knobs in production code) and adds a TLS/table branch to every production syscall solely for tests.

**Architecture of the seams.**
- **Syscall interfaces:** [`Context`] carries `BackingSyscalls &backing_syscalls` and `LocalSyscalls &syscalls` (constructed in `main` at [`dcfs/main.cc:322-375`] and in test harnesses; no global `Default()` accessor). Low-level helpers in [`dcfs/backing.cc`], [`dcfs/file_handle.h:35-66`], and [`dcfs/device_id.h:33`] take `BackingSyscalls &` (or `Context &`). [`dcfs/fsck.h:37-49`] and [`dcfs/startup_channel.h:58`] take a small `FsckDeps` / `DaemonDeps` struct holding references to `BackingSyscalls`, `LocalSyscalls`, and `ProcessSyscalls`. [`FileDescriptor`] keeps calling [`syscalls::close`] directly (never wrapped; avoids doubling `sizeof(FileDescriptor)`). Call sites keep calling the `syscalls::` free functions. Russ, 2026-10-18: "for the change to use a dependency-injected syscalls wrapper, keep the global free functions (so call sites look like syscalls::write(...)), but pass in the Context or Syscalls object. Then the wrapper can call syscalls.Write(...) (and note I'm removing the function naming exception for the member methods of Syscalls, since those functions will now only ever be called by the free functions in syscalls::." Then, russ, 2026-10-18: "Actually, let's have the syscalls:: free functions take Context. It can pull Syscalls from inside it, and it provides a uniform surface for the API (syscalls functions look and feel mostly like libc, with an additional Context argument)." So every `syscalls::` free function takes the `Context` as its first argument, pulls the right Syscalls interface out of it, and calls its CamelCase member (`Read`, `Openat`). The typed signatures stay (russ, 2026-10-18: "Be sure not to let that invalidate the other libc deviations we've already done (such as changing pointer+len to std::span, or changing char* to string_view)."): a call looks like `syscalls::read(ctx, fd, buf)` with `buf` a `std::span<std::byte>`, not `(fd, buf, n)`; names are `std::string_view`; errors are `Status`/`StatusOr`. The deviations are [`docs/style.md`] section 1.2 (`char *`), step 25.37 (spans) and step 25.38 (typed signatures). The members are called only by the free functions, and nothing else calls them. The build graph enforces that, not the compiler: the members are public, and the interfaces' BUILD visibility admits only the `syscalls::` free functions and the testonly fakes (the three conditions and the genquery test are in `docs/style.md`, rule-function-names; russ, 2026-10-18: "maybe we keep the Syscalls functions public, but control access via BUILD visibility?", then "Yes"). Fakes override the virtual members, and are still called only through the free functions; no `repo_shape` rule is needed. Naming rule: [`docs/style.md`] section 1.2, as amended by russ on 2026-10-18.
- **Typed ioctl and fcntl:** the template [`syscalls::ioctl`] becomes a typed front-end (anticipating 25.38): `ioctl<OP>` and `fcntl<OP>` stay free function templates, with the operation as a template parameter (`syscalls::ioctl<FS_IOC_GETFSUUID>(ctx, fd, ...)`, `syscalls::fcntl<F_GETFL>(ctx, fd)`). Each specialization calls a per-op CamelCase member (`IoctlGetVersion`, `IoctlGetFsUuid`, `IoctlBtrfsFsInfo`, `IoctlBtrfsSubvolInfo`, `IoctlFlags`, `IoctlRaw`) that implementations and fakes override (russ, 2026-10-18: "Yes, ioctl and fcntl become free function templates."; the templates take `Context` like every `syscalls::` function).
- **Libfuse seam:** Extract a `FuseLowlevelOps` interface on [`FuseSession`] / [`FuseRequest`] for `fuse_passthrough_open`, `fuse_passthrough_close` ([`dcfs/dir_cache_fs.cc:158-221`]), and `fuse_reply_none` ([`dcfs/fuse_request.cc:136`]), replacing those 3 wraps in [`dcfs/fuse_request_test.cc:159`] and [`dcfs/dir_cache_fs_test.cc:320-335`].
- **Trace-validation & recovery fault tests:** The 6 negative tests in [`DIR_CACHE_FS_FAULTS`] (`skip_mark_unknown`, `syscall_before_phase1`, `phase3_before_syscall`, `snapshot_after_syncfs`, `create_syscall_before_phase1`, `swallow_syscall_error`) test [`//formal:trace_validate`], not `dcfs`, by corrupting `dcfs` at link time. Replace `syscall_before_phase1`, `snapshot_after_syncfs`, `create_syscall_before_phase1`, and `swallow_syscall_error` with injected `BackingSyscalls` fakes that emit/alter trace events or run the backing syscall inside the fake before phase 1 commits, and replace `skip_mark_unknown` and `phase3_before_syscall` by either a `sqlite3_authorizer` on the test's [`sqlite::Database`] (`SQLITE_IGNORE` on the `unknown` write) or direct negative trace tests on `trace_validate`. In [`dir_cache_fs_crash_during_recovery_test`], replace wrapping mangled `cache::RecoverDirty` and `backing::BackingNlink` ([`dcfs/testonly/recover_dirty_fails_once.cc:25`], [`dcfs/testonly/probe_fails_once.cc:26`]) by injecting a `BackingSyscalls` fake that fails `statx` / `open_by_handle_at` on the first recovery pass and on the root-nlink probe.
- **`tools/waitexit.c`:** Split `waitexit_run(pid_t pid, const struct waitexit_sys *sys)` in [`tools/waitexit.c:18-62`] with function pointers `.pidfd_open` and `.poll` so [`test/qemu/waitexit_test.cc:56-96`] injects fakes without `-Wl,--wrap`. *Ruling (russ, 2026-10-18, who asked why it needs a seam at all):* first check whether `tools/waitexit.c` is still used once 25.34 ports the guest scripts to C++. If it is not used, delete it. If it stays, it moves to C++ on `LocalSyscalls` instead of getting a function-pointer struct of its own.

**Migration order (small mergeable commits, each tested).**
1. **Define `LocalSyscalls`, `ProcessSyscalls`, and `BackingSyscalls` + `Delegating*Syscalls` fakes.** Keep [`tools/style_matchers/backing_door_guard.query:1-9`] enforcing `BackingDoor()` in `RealBackingSyscalls`. *Test:* unit tests of `Real*Syscalls` and `Delegating*Syscalls` + [`//tools:syscalls_backing_users_test`].
2. **Migrate [`//test/qemu:waitexit_test`] and [`//dcfs:startup_channel_fault_test`].** Inject `waitexit_sys` in `waitexit.c` and `LocalSyscalls &` in [`ForkDaemon`]; drop `--wrap=pidfd_open`, `--wrap=poll`, `--wrap=setsid`. *Test:* `bazel test //test/qemu:waitexit_test //dcfs:startup_channel_fault_test`.
3. **Migrate [`//dcfs:device_id_fault_test`] and [`//dcfs:backing_fault_test`].** Thread `BackingSyscalls &` through [`GetDeviceId`], [`Context`], [`file_handle.cc`], and [`backing.cc`]; drop `--wrap=ioctl`, `--wrap=fstatfs64`, `--wrap=getxattr`, `--wrap=listxattr`, `--wrap=readlinkat`, and the [`tools/style_checks_allow.txt:66`] exception. *Test:* `bazel test //dcfs:device_id_fault_test //dcfs:backing_fault_test`.
4. **Migrate [`//dcfs:fsck_test`].** Thread `FsckDeps` through [`dcfs/fsck.cc:49-393`]; drop `--wrap={fork,waitpid,dup2,flock,unlinkat}`. *Test:* `bazel test //dcfs:fsck_test`.
5. **Migrate [`//dcfs:fuse_request_test`] and libfuse passthrough in `dir_cache_fs`.** Inject `FuseLowlevelOps`; drop `--wrap=fuse_reply_none` and `--wrap=fuse_passthrough_{open,close}`. *Test:* `bazel test //dcfs:fuse_request_test //dcfs:dir_cache_fs_test`.
6. **Migrate [`DIR_CACHE_FS_TEST_WRAPS`].** Move `NoTransactionAt`, `DCFS_INJECT`, `WrappedCalls`, `FakeInodeNumbers`, hooks, and `SetgroupsFault` ([`dcfs/dir_cache_fs_test.cc:134-516`]) into a `HarnessSyscalls` delegating wrapper on the test harness's `Context`; delete `DIR_CACHE_FS_TEST_WRAPS`. *Test:* `bazel test //dcfs:dir_cache_fs_test //dcfs:dir_cache_fs_poisoned_test //dcfs:dir_cache_fs_slope_test //dcfs:dir_cache_fs_fault_sites_test`.
7. **Migrate [`DIR_CACHE_FS_FAULTS`] and [`dir_cache_fs_crash_during_recovery_test`].** Replace the 8 `dcfs/testonly/` `--wrap` libraries ([`dcfs/testonly/BUILD.bazel:184-237`], [`dcfs/testonly/BUILD.bazel:310-334`]) with injected fakes/authorizer. *Test:* `bazel test //dcfs:dir_cache_fs_fault_... //dcfs:dir_cache_fs_crash_during_recovery_test`.
8. **Delete `--wrap` allow-lists, ban `-Wl,--wrap`, and update docs.** Ban calls to the interface members outside `syscalls*.cc` (the `syscalls::` free functions, which take `Context`, are their only callers; the mechanism is BUILD visibility, with `//dcfs:syscalls` the only admitted user, russ 2026-10-18); remove `__wrap_`/`__real_` exceptions from [`.clang-tidy:35-37,68-70`] and [`tools/raw_syscalls_test.py:15-17,141`]; add a check in [`tools/style_checks_test.py`] banning `--wrap` in `BUILD.bazel`/`*.bzl`; update [`AGENTS.md:22-23`], [`docs/style.md:474-475,516,532,1491-1493,1559,1757-1758`], and [`docs/design.md:3292-3312`]. *Test:* `bazel test --config=presubmit //...`.
9. **Migrate the `InChild` reap test (25.34 follow-ups).** The reap test,
   written with a `--wrap` fake before this step, takes `LocalSyscalls` here:
   `InChild` takes `LocalSyscalls` and the test injects a failing `read`;
   `InChild`'s early-return Cleanup reaps the child, and the test asserts no
   zombie (`waitid` reports it gone). *Test:* `bazel test
   //test/qemu:guest_test`.

**Risks.**
- *Bypassing the injected interface:* A caller invoking an interface member directly would silently skip `NoTransactionAt` and `FaultSitesTest`. Mitigation: the members are reachable only through the `syscalls::` free functions, enforced by BUILD visibility and a genquery test (`docs/style.md`, rule-function-names) in sub-step 8 (`FileDescriptor::Close`, which calls `syscalls::close` without an interface argument, needs the same treatment).
- *Variadic `openat`/`ioctl`/`fcntl`:* Virtual methods cannot be variadic or templates. Mitigation: split `openat(dirfd, path, flags)` and `openat(dirfd, path, flags, mode)` and use the `ioctl<OP>`/`fcntl<OP>` free function templates over per-op CamelCase members, aligning directly with step 25.38 ([`docs/plan/phases/25-style-convergence.md:1405-1437`]).
- *Coverage Baseline:* Every `Real*Syscalls` method and error branch must stay covered so total coverage never drops ([`AGENTS.md:24-29`]).

**Orchestrator review notes (2026-10-18).**
- (a) The hot-path claim (<0.5%, "verified by `dcfs_bench`") is unmeasured. Sub-step 1 measures `dcfs_bench` before and after the interface (miss-heavy and mutation-heavy cases). The step stops for russ if the cost is not negligible.
- (b) It executes the design `docs/design.md:3307-3312` sketched when `--wrap` was kept on 2026-10-16, and answers that decision's three objections: separate libraries kept, one way to call a syscall, and no virtual inheritance.
- (c) Order: it lands before 25.35–25.38, so strong types and spans land once on the final interfaces.
- (d) Owner: dcfs-protocol for sub-steps 6 and 7 (the fault and trace-validation tests touch the protocol's negative tests); dcfs-implementer otherwise.

**Split into reviewed slices (backlog order, from backlog proposal 2,
2026-10-18, approved by russ 2026-10-19 as a whole: "Yes I approve proposal 2
as a whole."; `process.md` `rule-reviewed-slices`).** By library: 25.41a
`BackingSyscalls`, 25.41b `LocalSyscalls`, 25.41c `ProcessSyscalls`, then the
fault tests per the 6-test list in this section.

## 25.42 Pidfds over pids (russ, 2026-10-18; queued after the 2026-10-23 quota reset)

Russ's rule (2026-10-18): "Prefer pidfds over plain pids"
([`docs/style.md`] section 1.5). A child or daemon is held as an owned pidfd,
signalled with `pidfd_send_signal`, and waited on with `waitid(P_PIDFD)` or
`poll`. The sources and their limits are in section 1.5: `clone3` with
`CLONE_PIDFD` or `pidfd_open` for a child or daemon whose pid this process
learns from the kernel, and `open("/proc/<pid>")` only where a pid arrives
from outside. A `/proc/<pid>` fd can be signalled but not waited on
(`kernel/exit.c:1786` (Linux 7.3-rc7)).

Scope: `DcfsMount`, `RunCommand`, fsck's and the startup channel's children,
[`bench/process.cc`] and the tools hold owned pidfds. `pid_t` holders across
`dcfs/`, `tools/`, `bench/` and `test/qemu/` become pidfds; a `pid_t` stays
only for printing or for an API that takes nothing else. `DcfsMount` holds
its daemon as a pidfd here, not as the `std::optional<pid_t>` that 25.34
keeps. Wrappers the steps need (`clone3`, `pidfd_open`, `pidfd_send_signal`,
`waitid`) are added to the syscall libraries with tests.

The deletion (russ, 2026-10-18, superseding the earlier "flag calls to
pid-based wrappers" wording): once every caller holds a pidfd, delete
`syscalls::kill`, `syscalls::waitpid` and any `tgkill` wrapper. The compiler
then enforces the rule, and `//tools:raw_syscalls_test` and `banned_symbols`
keep raw calls out, so no new check is needed. A wrapper survives only if a
caller truly has nothing but a pid (for example a pid read from a file, where
opening `/proc/<pid>` is the alternative to try first), and each survivor is
recorded with its reason beside the wrapper.

Owner: dcfs-implementer. Dependencies: none stated by russ.

## 25.43 Log a SQLite rollback only when it fails (russ, 2026-10-18; queued, not started)

The warning "rolling back a failed transaction failed: ... no transaction is
active" is misleading when SQLite has already rolled the transaction back by
itself, as after SQLITE_FULL. Found by the CI investigation of run
39138000104.

Fix: `UnwindFailedTransaction` (`dcfs/sqlite.cc`, near line 466) checks
`Connection::InTransaction()` and skips the ROLLBACK when the transaction has
already ended. A real rollback failure still logs.

Steps:

1. Test first: a unit test makes a commit fail with SQLITE_FULL, using the
   existing "cache full" abort-trigger pattern in `dcfs/dir_cache_fs_test.cc`,
   and asserts no warning with `ScopedMockLog`. Quote the failing run. A second
   test makes a real rollback fail and asserts that it still logs.
2. Fix `UnwindFailedTransaction` as above.

Test: the two new tests, then `bazel test --config=fast //dcfs/...`.

Owner: dcfs-implementer. Dependencies: none.

## 25.44 Correct the false statements in docs/style.md and AGENTS.md (russ, 2026-10-18; queued, not started; after the rules-index branch merges)

Found by the 2026-10-18 maintainability fact-check. Several statements in
`docs/style.md` and `AGENTS.md` are false about today's tree. Each one either
becomes true (the code or the check is implemented) or is reworded to say it is
planned, with the step that will make it true.

`docs/style.md` (line numbers as of 2026-10-18):

- `layering_check` is said to be enforced (87-91); it is off in
  `tools/style_checks.bzl:74`.
- `format_test` is said to enforce BUILD layout and to run shfmt (1631-1633,
  1677, 1622, 14); `tools/format.py` takes only `.c`, `.cc` and `.h`.
- The matchers `const_local_value`, `char_pointer_string` and the
  function-pointer matcher are described as existing (187-242).
- The `ABSL_FLAG`-outside-`main` `repo_shape` rule (220-222).
- The confession check (1315-1318).
- The injected syscall interfaces (124-131); 25.41 has not started.
- "0 annotations" (165); seven exist.
- `FATAL` (`CHECK`) is listed as a log level (1093).
- `happy_path_nested` is listed as report-only (1074-1076).
- `status_dropped` is missing from the report-only list (77-78).
- The checked directories (70).
- Stale shell and Python numbers (1653-1655).
- The thin-glue ruling is superseded by `no_new_guest_shell` (1708-1713).
- Appendix A rows F1, F2, F6, C3, C13, D2 and N1.
- `-Weverything` is written in the present tense (158-162),
  `test/qemu/README.md` (1744) and `docs/plan/phases/25-style-convergence.md`
  (1671); it becomes true with the `.bazelrc` change or is reworded as planned.

`AGENTS.md`:

- "every test declares `size` and `timeout` explicitly" is false for 72 of 663
  tests: 39 in `formal/`, 14 in `tools/`, 7 in `test/qemu/`, and the rest under
  `third_party/`. Add the missing timeouts, then add a check: the QEMU macros
  already refuse a missing one, so extend that to the `tlc_test` and `py_test`
  macros, or add a `tools/repo_shape.py` rule.

Steps:

1. Count the false statements again against the tree on the day the step
   starts; the line numbers above are from 2026-10-18 and will have moved.
2. For each statement, either implement it (with a test or a check that fails
   first, as the rule requires) or reword it as planned, naming this step or
   the step that will make it true.
3. Add the `size` and `timeout` declarations and the check for the 72 tests.
4. Split `docs/style.md` into the rules (what agents read) and an archive of
   reasons and rulings, and move orchestrator-only text out of
   `docs/plan/process.md`, to cut the fixed reading cost of every dispatch.
   Clerk batch 28, 2026-10-18 (from an outside AI review russ shared that day).

Test: a check that fails on a test that lacks `size` or `timeout`, shown
failing first; the implementer names the commands when the step starts.

Owner: not yet assigned (the orchestrator chooses). Dependencies: the
rules-index branch merges first, because it restructures `docs/style.md` and
`AGENTS.md`.

Also folded in (the same fact-check's stale text outside `docs/testing.md`,
2026-10-18; a small fix job, clerk batch 24): `.github/workflows/ci.yml:316`
says "No threshold gate yet", but the coverage gate exists;
`.github/ci/coverage.sh:9` says "not 0.1 or more above it", but the gate passes
a rise; `tools/mutation/README.md:182` says "survivor ... 1 per push", but
survivors are findings and only exit 2 fails; and `AGENTS.md` and `docs/rules`
still mention `formal/findings/`, which was removed with 12.6b (5dcbdca). Each
becomes true or is reworded as planned, as above.

## 25.45 Mechanize what reviewers keep finding (russ, 2026-10-18; in progress: items 2 and 3 merged 2026-10-19; the rest queued after the 2026-10-23 reset unless russ says otherwise)

Status (2026-10-19): item 2 merged b1349d6e (the 80-column rule and the
split-span rule in `repo_shape`), item 3 merged e716d5dc
(`commit_subjects.sh` width and trailer checks), with the follow-up wrap
acff136c. The 80-column rule landed before two commits that added long lines,
so `repo_shape_test` failed briefly on main until acff136c.

Russ, 2026-10-18: "are there things the reviewers are finding that we can
automate mechanically?" The ledger analysis (311 review-findings rows, 106 of
them marked "could" be mechanical) says yes. Build these in order, each a
`tools/` check with its own failing-first test:

1. Kernel-citation checker (about 34 findings, M). Every `path:line` citation of
   the kernel carries the tested kernel's version and exists in it; the cited
   line is anchored to the named symbol within +-N lines; a bare kernel
   function name next to a kernel path is refused. This is 25.39 item 3,
   pulled forward. Source decided (russ, 2026-10-19: "Pinned tarball sounds
   fine. We can even pin it to the exact kernel we're using for the tests."):
   a Bazel-pinned kernel.org stable tarball of exactly the test kernel's
   version (after 24.2, Alpine v3.22's `linux-virt` 6.12.x); one version
   setting drives both the test kernel and the citation source, so they bump
   together. The checker runs when docs change. Doc kernel citations cite the
   tested kernel's `file:line`; citations to upstream master (for example in
   `notes/kernel-patches.md`) carry a skip marker (the orchestrator's
   proposal; russ may ask for a second pinned source instead). Prerequisite:
   24.2.
2. `repo_shape` 80-column rule for `*.md`, `*.py`, `*.sh` and `BUILD` files,
   outside fences, tables and URLs, with a shrink-only allowlist; plus a rule
   refusing a backtick span split across lines (about 18 findings, S).
3. `.github/ci/commit_subjects.sh`: subject at most 72 columns; body lines at
   most 72 columns outside quotes and URLs; `Co-Authored-By` present; "Failing
   first" in the body of a test-first commit (about 11 findings, S).
4. `repo_shape` comment rules: no step numbers in code comments; no comment
   block longer than 10 lines above a declaration; no first person in
   `docs/style.md` or the reference docs (about 8 findings, S).
5. A clang-query matcher for const non-reference scalar locals (about 6, S-M).
6. A PreToolUse hook on Bash for subagents that refuses `bazel ...|` piped
   into `tail`, `head` or `grep` when chained with `&&` (the rule is
   `rule-no-pipe-bazel`, which the hook enforces; `set -o pipefail` or a
   separate command is allowed), with a self-check test. The 17.2e final
   round's Bazel log had about 35 such pipes, once as `bazel test ... | tail
   -1 && git commit`, which commits even when the test fails. With
   `--config=agent` in lanes the pipes are unnecessary anyway. Clerk batch 28,
   2026-10-18, from the 17.2e final round's log (russ: "let's learn from its
   mistakes"); mechanical first, after the 2026-10-23 reset.

Then: pasted UAPI constants (M), and finishing 7.6b (shfmt, shellcheck and
buildifier). Together these should cover about 20% of the should and nit
findings (55-65 of 294). The rest are stale numbers and doc accuracy, which
need judgement and stay with reviewers.

Test: each check fails first on a fixture that carries the finding, and passes
on the tree once the existing findings are fixed or allowlisted.

Owner: not yet assigned (the orchestrator chooses). Dependencies: none for
items 1 to 4; item 1 needs `~/Sources/linux` at the pinned tag.

## 25.46 Audit the kernel invariants dcfs relies on, and pin each one (russ, 2026-10-18; queued, not started)

Why the miss, for the record (russ, 2026-10-18: "why did we miss
fadvise(WILLNEED) until now? Incomplete model?"): the TLA+ model covers the
metadata write-through protocol, not which kernel requests reach dcfs on which
handles (passthrough is not modelled); the differential tests compare results,
and a hint changes no result, only caching; and no inventory of entry points
existed. Step 5 is that inventory.

Russ, 2026-10-18: "For this and everything else that we're relying on kernel
invariants like this, add a test." A kernel invariant that lets dcfs skip work
or decide a result needs a native `kernel_facts_test` case citing `path:line`
(Linux v7.3-rc7) ([`docs/style.md`] section 2, `rule-kernel-invariants-pinned`).

Steps:

1. Find the reliances. Grep the citations of "Linux v7.3-rc7" in `dcfs/` and
   list each one that lets dcfs skip work or decide a result.
2. For each reliance with no `kernel_facts_test` case, add the case and cite
   the kernel path and line in it. A reliance whose test cannot run natively is
   listed with its reason in the step's "As built" notes.
3. Check that the weekly noisy job runs every case (`kernel_facts_test` runs
   only in the weekly workflow, `rule-kernel-facts-weekly`).
4. Recording FUSE server fixture (russ, 2026-10-18: "Yes. Queue the recording
   FUSE server fixture."): a `testonly` recording FUSE server (libfuse
   passthrough that logs every request: opcode, valid and flags bits, pid,
   uid, gid), used by `kernel_facts_test` to pin, with dcfs out of the loop,
   every FUSE-module behaviour dcfs relies on:
   - `FATTR_KILL_SUIDGID` on truncate only for a caller without `CAP_FSETID`,
     and always on an id-changing chown (fs/fuse/dir.c:2231-2243);
   - under `KILLPRIV_V2`, a non-`CAP_FSETID` write arriving as an empty SETATTR
     with the kill bits and mode dropped (fs/fuse/dir.c:2340-2342, 2248);
   - `REMOVEXATTR` of `security.capability` before that SETATTR;
   - `SB_NOSEC` under V2;
   - `kiocb_modified` on fallback writes (fs/fuse/file.c:1518);
   - a caller outside the server's pid namespace arriving as pid 0
     (fs/fuse/req.c:13);
   - passthrough writes using the daemon's credentials.
   Citations as `path:line (Linux vX)` at the tested kernel.
5. "What the backing does" entry-point table (russ, 2026-10-18: "why did we
   miss fadvise(WILLNEED) until now?"): every VFS entry point a program can use
   on a dcfs file, with, for each one:
   - the `struct file_operations`, `inode_operations` and
     `address_space_operations` of FUSE at the tested kernel, plus fadvise,
     readahead, madvise, sync_file_range, the FS_IOC_* ioctls, mmap flags,
     O_DIRECT, and the copy, clone and dedupe ranges;
   - how it reaches the backing under dcfs: passthrough, fallback served by
     dcfs, or not forwarded;
   - whether the backing's behaviour is reproduced, including side effects
     such as cache warming and readahead;
   - the test or kernel-facts case that pins it.
   Gaps become steps. Differential tests for hint syscalls compare side
   effects (cachestat residency), not only results.

   The side-effect checks take no sleeps, timers or waiting for I/O to settle
   (russ, 2026-10-18: "a non-timer / more deterministic way to measure the
   side effects is needed in the differential tests."). They observe side
   effects only as events or as state reached after an ordering barrier: dcfs's
   own backing-syscall trace (`syscall_traces` / `BackingCall`); a
   `cachestat(2)` snapshot taken after a synchronous, always-sent FUSE
   request that orders the background READ; guest ftrace tracepoint counts
   filtered by inode where the kernel's side must be seen. Never cgroup
   `io.stat` byte or request counters, which move with load (the 26.41
   lesson). The same rule as 23.19.

Test: each new case fails first on a deliberately wrong expectation, then passes
on the pinned kernel; the step names the commands when it starts.

Owner: not yet assigned (the orchestrator chooses). Dependencies: none.

**Split into reviewed slices (backlog order, russ, 2026-10-19: "Yes I approve
proposal 2 as a whole."; `process.md` `rule-reviewed-slices`).**
- **25.46a** steps 1-3: the reliances, the pins and the inventory. No
  prerequisite.
- **25.46b** the recording FUSE server. Prerequisite: 25.46a; feeds 12.27
  items 11-12 and 23.19.
- **25.46c** the entry-point table. Prerequisite: 25.46a.

## 25.47 Procedures as skills and scripts (russ, 2026-10-18; queued, not started; scheduled after the Friday 2026-10-23 01:00 PT quota reset)

russ, 2026-10-18: "Do the dashboard-upload skill, and schedule the rest for
after the Friday reset." The dashboard-upload skill is done separately and is
not part of this step.

Status (2026-10-19): the Stop hook merged 5632b3ae (2026-10-18). It is silent
for dashboard news, blocks only for the clerk queue, and the PostToolUse hook
carries both reminders; uploads do not mark lanes. The rest of the step is
still queued.

Skills under `.claude/skills/` link rule anchors and never restate rules (the
attribution check covers them). Scripts under `tools/agents/` come with tests.

1. Skill: clerk batch. Drain the queue; files a branch is editing are
   off-limits; rule anchors and one index entry for each rule changed; one log
   entry per batch; commit pending ledgers; subject format; the rule-index
   check.
2. Skill: recording review findings. Line format; `review_findings.py add`;
   step labels; outcomes at merge.
3. Skill: review and fact-check briefs. The correctness reviewer, the style
   reviewer and the document fact-checker: what each checks, the findings
   block, the verified-correct list, and which indexes each reads.
4. Script: merge a lane branch. Fetch from the lane clone; fast-forward when
   possible, and always when `.git-blame-ignore-revs` hashes must survive, else
   cherry-pick; subject check; clear the lane state.
5. Script: set up a lane for an agent. Fetch main into the separate clone,
   `checkout -B`, `user.bazelrc` present.
6. Script: one quota reading that prints Claude (`claude_usage.py`) and agy
   (`agy -p '/quota'`) together, folded with the queued `dashboard_lanes.py`
   quota fix.
7. Script: the plan census. `dashboard_lanes.py` regenerates
   `~/.cache/dcfs-dashboard/plan.json` (step, phase, title, state, evidence)
   from `docs/plan/phases/*.md` and `log.md`, and adds `plan` to `DATASETS`
   so the hook tracks it and `mark-uploaded` accepts it. It replaces
   process.md's hand-maintained plan.json, which went stale from 2026-10-17
   22:00Z because the orchestrator never updated it on merges; that stale
   dataset makes the Dashboard type's toolbar read "Updated 1d ago". (The
   dashboard page, files/index.html v10, now shows its own "Last updated"
   line from the newest datasets `updated.at`.) Clerk batch 27, 2026-10-18.
8. Stop-hook nudge: `dashboard_lanes.py` counts dispatch-ledger rows by
   `task_class` per day and, on the second manual run of the same class with
   no script or skill for it, prints "second manual <class> today: mechanize
   it" (the mechanical-first rule, process.md). Clerk batch 27, 2026-10-18.
9. `review_findings.py`: a `checks-owed` subcommand lists every row with
   `mechanizable=could`; the clerk adds each new kind to 25.45's list, so no
   mechanizable finding is lost. Clerk batch 27, 2026-10-18.
10. Per-step spend metric. At merge the clerk sums every dispatch-ledger row
    for the step (implementer, fix rounds, reviews, clerk work, and the
    orchestrator's share apportioned by time) and converts the sum to "% of
    week per merged step" using the quota readings; a weekly dashboard chart
    shows it. Needs the token capture on branch ledger-token-capture. Clerk
    batch 28, 2026-10-18 (outside AI review, 2026-10-18).
11. Stop hook nags instead of blocking: a blocking hook forced 30 or more
    orchestrator turns on 2026-10-18. Dashboard refreshes are batched, and the
    quota read happens inside the hook. Clerk batch 28, 2026-10-18.
12. A measured trial of the style reviewer at medium effort for routine
    steps, once tokens are recorded (item 10's data), compared with high on
    the same steps. Clerk batch 28, 2026-10-18.
13. Scoped style check for the inner loop: `bazel run //tools:style_check --
    <changed files or targets>` runs clang-tidy and the matchers only on the
    given translation units. The repo-wide `//tools:style_checks_test` (a
    repo-wide clang-tidy aspect and matchers) cost 8 to 10 minutes per
    uncached run, and the 17.2e final round ran it about 12 times in its edit
    loop (about 35 of 52 Bazel minutes). Add to `rule-quiet-output` that the
    repo-wide style test runs once before reporting. Clerk batch 28,
    2026-10-18, from the 17.2e final round's log.

*Test:* the scripts come with tests under `tools/agents/`.

**Split into reviewed slices (backlog order, russ, 2026-10-19: "Yes I approve
proposal 2 as a whole."; `process.md` `rule-reviewed-slices`).**
- **25.47a** hooks and the scoped style check.
- **25.47b** skills.
- **25.47c** scripts (merge lane, quota, census).
- **25.47d** metrics.

## 25.48 Foreman single mode (russ, 2026-10-19; done, fa037f4a..1565b72d)

russ, 2026-10-19: "foreman's purpose was to have an orchestrator in Gemini
as well. Maybe we should have both an in-gemini orchestrator and a
non-orchestrator mode? foreman.py also was extended to not be subject to the
30 minute timeout, so we do need to keep using it."

This replaces the research mode this section described earlier on 2026-10-19
(a `--read-only` flag alone). `tools/agents/foreman.py` gets two modes:

1. Orchestrator mode, the default: today's worker, plus review and fix rounds.
2. `--single`: one held-open worker session, with no review and no fix
   rounds.
3. `--read-only` combines with `--single` only: agy plan mode, no branch
   setup, and its text, minus the JOB-COMPLETE line, is saved as `report.md`.
4. Foreman records its session in `tokens.json` and adds the ledger row
   (25.49d).

Owner: dcfs-implementer. Dependencies: none.

*Test:* `//tools/agents:foreman_test` covers both modes and the
`--single --read-only` combination.

Process note (2026-10-19, from the slice 2 review): the implementer wrote
slice 2's tests and code in one pass, so the failing-first evidence had to be
recovered by running the tests against slice 1's code. Queued as 25.52.

## 25.50 `clerk-queue add` rejects flags (done 2026-10-19, cef83075..626595ad)

Done 2026-10-19 by Gemini 3.8 Flash through foreman orchestrator mode, the
first step done on Gemini under rule 12; review by Sonnet, two nits fixed in
75addafd before merge (cef83075, 626595ad on main).

`clerk-queue add --help` queued the literal item `--help`, which reached
clerk batch 34 (orchestrator's queue, 2026-10-19).

1. `clerk-queue add` in `tools/agents/dashboard_lanes.py` rejects any
   argument that starts with `-` and prints its usage line,
   `clerk-queue add TEXT...`.
2. A test in `tools/agents/dashboard_lanes_test.py` fails on the current code
   first, then passes.

Owner: not yet assigned (the orchestrator chooses). Dependencies: none.

*Test:* `dashboard_lanes_test.py` covers the refused flag.

## 25.51 `repo_shape` CLI runs its test's checks (batch 35, 2026-10-19; queued)

`python3 -I tools/repo_shape.py` does not run `tracked_problems` (long_lines,
split_code_spans), which `//tools:repo_shape_test` runs. Clerk batch 34
passed the CLI with 84 long lines and turned main's tools tier red
(2026-10-19).

1. The CLI runs the same checks as `//tools:repo_shape_test`.
2. A test in `tools/repo_shape_test.py` fails on the current code first: a
   long line makes the CLI exit nonzero.

Owner: not yet assigned (the orchestrator chooses). Dependencies: 25.45 item 2
(merged b1349d6e).

*Test:* a long line fails the CLI.

Status (2026-10-19): before this step `tools/repo_shape.py` had no `main()` and
checked nothing when run, so every "repo_shape clean" report from the CLI
before 2026-10-19 was meaningless; only `//tools:repo_shape_test` checked. The
slice review of 2026-10-19 is in `review-findings.tsv`.

## 25.49 Subagents delegate down (russ, 2026-10-19; done, 1887e3ea..fe4f9953)

Done 2026-10-19: 25.49a merged 8779ff6d; 25.49d merged 17e1a466 and da80d74e;
items 2 to 4 merged 1887e3ea..fe4f9953. russ, 2026-10-19: "I'd like it if
our subagents started doing this too, and themselves leveraging dumber
agents for more of the menial work"

1. A rule in `docs/plan/process.md` with an index line in
   `docs/rules/index-process.md`. Each `.claude/agents/*.md` names the
   delegation tool.
2. foreman `--parent` and `--min-weekly-pct` (default 1, refuses on an
   empty or unreadable weekly pool; a refusal creates nothing and is final).
3. Briefs carry a decomposition section; step reports carry a `Delegated` line.
4. Measure in the ledger: parent tokens with and without delegation.

Status: item 1 (the rule, `rule-delegate-down`, and the agent definitions)
and the `dcfs-foreman` skill are done by 25.49a. Items 2 and 3 are done by
1887e3ea..fe4f9953: the `--parent` token and the weekly-pool check in foreman,
and the decomposition section in `process.md` dispatch rule 2.

Item 4 (measure) is the `dashboard_lanes.py ledger by-parent` report, which
totals a step's own tokens against its delegated ones. 25.49d writes
`tokens.json` and one ledger row per foreman run, with tokens = total +
cache_read, as Claude rows count them. Gemini subagent tokens are inside the
session total and not attributable to a subagent.

Process note (2026-10-19, from the slice 2 review): slice 2's four added tests
pinned existing behaviour, so none fails on the old code first. Slice 1's
fixes: all of their tests other than the 14.5% case also passed on the old
code.

Owner: not yet assigned (the orchestrator chooses). Dependencies: 25.48.

*Test:* `tools/agents/foreman_test.py` and `dashboard_lanes_test.py` cover
the `--parent` token, the weekly-pool refusal and the by-parent report.

### 25.34 as built (dcfs-implementer, 2026-10-17)

- Helpers: `test/qemu/guest/guest.h` (`DcfsMount::{Start,Unmount,Crash,Log}`,
  `InChild` with an optional `Identity`, `Snapshot`, `CompareTrees`,
  `ExpectSameAsBacking`, `Mkdir`, `WriteFile`); a command runs through
  `testonly::RunCommand` (argv, no shell, `dcfs/testonly/process.h`, built on
  the `syscalls::` fork/execv/waitpid wrappers). `InChild` carries the child's
  Status whole (payloads included) with `testonly::SerializeStatus` and
  `ParseStatus` (`dcfs/testonly/status_wire`, a protobuf). Tested by
  `//test/qemu:guest_test`.
- `qemu_cc_test` gained `dcfs = True` (the dcfs binary at `/test/data/dcfs`)
  and a disk's filesystem type (`/test/disk0` holds `vdb:xfs`, device and type;
  guest/init mounts that type), so a port can run on ext4, xfs and btrfs.
- First port: `guest/removed.sh` (197 lines) is
  `//test/qemu:removed_test_{ext4,xfs,btrfs}` (`removed_test` aliases
  ext4). The testutil subcommands only it used were deleted; `removed-link`
  and `tmpfile-link` stay in testutil for `copy.sh` and are duplicated in the
  C++ until `copy.sh` ports.
- The rule: `docs/style.md` section 4, `AGENTS.md`, and
  `tools/repo_shape.py` `no_new_guest_shell` with
  `tools/repo_shape_guest_shell.txt` (shrinks only).
- russ (2026-10-17): no separate directory; the sources are in
  `test/qemu/guest/` beside the scripts and the targets in
  `test/qemu/BUILD.bazel` (a BUILD file in `guest/` would move every `guest/`
  label of that package, `guest/init`, the globs, into another package). The
  raw-syscall scan and `style_checks.py` cover `test/qemu/guest/*.cc`; the
  scenarios call the `syscalls::` wrappers, and `truncate`, `poll` of several
  descriptors, `pidfd_open`, `setresuid` and `setresgid` were added.

Follow-ups, in order of value (edits in git history, then size; every one
needs only the helpers above plus what is named):

1. 25.34a `lib.sh` (446, 32 edits): goes as the scripts leave it; its
   `snapshot`, `list_names`, `start_daemon` are `guest.h` today.
2. 25.34b `write.sh` (809), `rename.sh` (563), `lifecycle.sh` (481),
   `create.sh` (460), `setattr.sh` (426): `DcfsMount`, `ExpectSameAsBacking`;
   rename and lifecycle also `Crash`; setattr needs a `Stat`-comparing helper.
3. 25.34c `nfs.sh` (735), `nfs_backing.sh` (376): a Debian chroot and an NFS
   server; need a chroot (`testonly::RunCommand`).
4. 25.34d `mount_dcfs.sh` (905), `mount_dcfs_systemd.sh` (961),
   `systemd_install.sh`/`systemd_run.sh`: the mount helper with options, a
   systemd image; `testonly::RunCommand` throughout.
5. 25.34e `fault_power.sh` (708), `fault_ace.sh` (724), `fault_recover.sh`
   (714), `fault_shutdown.sh` (675), `fault_freeze.sh` (462), `fault_lib.sh`
   (257), `fault_dcfs_lib.sh` (220), `fault_backing.sh`, `fault_cache.sh`,
   `fault_selftest.sh`, `crash.sh` (305), `power.sh` (256): `Crash`, a power
   cut helper (the harness), dm-flakey/dm-log-writes, `Snapshot`/
   `CompareTrees`; 26.36 is editing some of these.
6. 25.34f `credentials.sh` (570, after 17.2e), `cache_permissions.sh` (473):
   `InChild` with `Identity`.
7. 25.34g `sqlite_durability.sh` (941) and its lib, `handles.sh` (446),
   `release_leak.sh` (193), `destroy.sh` (165), `boot.sh` (212),
   `readonly.sh` (315), `syscall_traces.sh` (407), `strace_lib.sh` (383),
   `xfstests*.sh`/`pjdfstest*.sh`/`sibylfs*.sh` (suite runners: thin glue that
   `exec`s the suite may stay shell), `stress*.sh`, `bench*.sh`, `idle*.sh`,
   `memory.sh`, `atime.sh` (213), `copy.sh` (259), `passthrough.sh` (247),
   `readdir_boundary.sh` (238), `boundary.sh` (268), `names*.sh`,
   `enospc_*.sh`, `cancel*.sh`, `request_counts.sh`, `quiet_kernel.sh`,
   `noisy_kernel.sh`, `help.sh`, `invariant_checks_on.sh`, `ram_disks*.sh`,
   `kernel_bug_repro.sh`, `casefold_tune_oops.sh`.
8. Host-side `test/qemu/*_test.sh` (run_qemu_verdict_test 599,
   run_qemu_cost_test 396, strace_lib_test 263, xfstests_gate_test 243, ...):
   Python or C++, not part of this family.

## 25.52 Mechanical test-first evidence (done 2026-10-19, 81299351..d82f8168)

Orchestrator's proposal, not russ's words. Source: 25.48 on 2026-10-19, where
the implementer wrote tests and code in one pass twice in one step, and the
failing-first evidence had to be recovered by hand.

1. Each slice's first commit adds or changes only tests.
2. `tools/test_first_tool` (C++, `//tools:test_first_tool`, logic in
   `tools/test_first.{h,cc}` and `tools/test_first_cli.{h,cc}`, process runner
   in `tools/process_runner.{h,cc}`) is run by the orchestrator per slice at
   merge and checks that the slice's test targets FAIL at its first commit and
   PASS at its last, printing the failing test names for the merge record. Exit
   code 0 is accepted and 3 rejected. Any other code means no verdict: 2 when
   the check could not be made or the usage is wrong, and 1 for a flag Abseil
   does not know.
3. Commits on main stay green: the orchestrator cherry-picks each slice squashed
   after the check, one commit per slice (process.md rule-reviewed-slices 4a).

Design decisions:

- **Slice boundaries are explicit.** `--first=REV --last=REV`, one slice per
  run. A `Slice: N` trailer was rejected because it adds a convention every
  agent must remember, and the slice report already names the commits.
- **Test-file table.** One table in `tools/test_first.cc`: `_test.cc`,
  `_test.py`, `_test.sh`, directories `testdata/` and `testonly/`, the trees
  `test/`, `formal/known_bugs/`, `formal/limitations/`, and `BUILD.bazel`. Any
  other file in the first commit rejects the slice.
- **Targets.** Found with
  `bazel query 'kind("(cc|py|sh)_test", rdeps(//..., set(labels)))' --keep_going`
  at the first commit. Exit 3 (partial: a rule of the first commit does not
  load) means the targets are found at the last commit instead and counted as a
  build failure at the first.
- **Passing targets.** A target that passes at the first commit is reported
  (`passed at first`). The slice is rejected only if that target's own test
  source changed in the commit, or if nothing failed.
- **Build failure at the first commit.** Accepted and labelled `build failed`,
  with the first five `ERROR:` lines quoted for the reviewer to confirm. Refused
  when no `ERROR:` line names a changed test file or an owning target.
- **BUILD files.** A touched `BUILD.bazel` is read as rules
  (`bazel query //pkg:all --output=build` at the first commit and at its parent,
  compared by rule name, comments and `generator_location` ignored). Every new,
  changed or removed rule must be a `*_test`, `testonly`, or generated by a
  test; a rule that was production before may not become testonly; no visibility
  may change.
- **Safety.** It refuses a dirty tree and a first commit that is not an ancestor
  of the last. It always checks the starting branch out again, also when Bazel
  breaks.
- **Injection.** git and Bazel run through an injected `CommandRunner`, so the
  logic is unit-tested with a scripted runner (`//tools:test_first_test`,
  `//tools:test_first_cli_test`), the real runner is tested over busybox in the
  guest (`//tools:process_runner_test`), and `main` as a program
  (`//tools:test_first_main_test`).
- **Main stays green.** The orchestrator squashes each slice when cherry-picking
  and quotes the report in the commit body (process.md rule-reviewed-slices 4a).

Accepted by hand on the real branch: `--first=7ff2e36f --last=dd5e3b38` gives
ACCEPTED (build failed at the first commit with
`target '//tools:test_first' does not exist`, passed at the last), and
`--first=dd5e3b38 --last=110658d9` gives REJECTED (the first commit touches
`tools/test_first.cc`).

Placed in M0 right after 25.49 items 2-4 (`README.md`, Queue item 2a).

Owner: not yet assigned (the orchestrator chooses). Dependencies: 25.49d
merged (da80d74e).

*Test:* the tool fails a slice whose test targets pass at its first commit, and
accepts a slice whose targets fail there and pass at its last commit.

## 25.53 One run-a-program helper (orchestrator's proposal, 2026-10-19; queued)

Orchestrator's proposal, not russ's words. Source: 25.52's review of
2026-10-19 (`review-findings.tsv`, slice 2), which found `tools/process_runner`
reinventing `RunProgram` and `ReadAll` from `dcfs/fsck.cc`. This is the
reinvented-helper class the backlog analysis named as the main leak.

1. One shared helper runs a program (fork, socketpair, dup2, execv, wait) and
   reads its output with a Status, used by `dcfs/fsck.cc` and
   `tools/process_runner`.
2. The duplicate is removed from both callers; no behaviour changes.

Placed in M0 after 25.52 (`README.md`, Queue item 2b). Owner: not yet
assigned (the orchestrator chooses). Dependencies: 25.52 merged.

*Test:* the existing `dcfs/fsck` and `process_runner` tests pass unchanged
on the shared helper, and `process_runner_test.cc` covers the helper's error
paths.

