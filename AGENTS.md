# Working on dcfs

Rules for anyone changing this repository, human or agent. `README.md` explains
what dcfs is; `docs/design.md` explains how it works; the plan is
`docs/plan/README.md` (one file per phase in `docs/plan/phases/`). Rules marked
"(from phase N)" take effect when that plan phase lands.

This file is the index of every ratified rule (russ, 2026-10-18: "make AGENTS.md
the complete index"). Each rule is one line, in russ's words where possible,
with the date and a link to the section that holds its reasoning:
[style.md](docs/style.md), [design.md](docs/design.md),
[process.md](docs/plan/process.md) or [CLAUDE.md](CLAUDE.md).
`tools/repo_shape.py` fails when a rule marked in those files has no entry here,
or an entry links to a marker that no longer exists (the marker is the `rule-`
anchor above the rule's heading or at the start of its bullet). To add a rule:
write it and its reasoning where it belongs, put an anchor `<a
id="rule-NAME"></a>` on it, and add a line here.

## Tests

- **Test first.** Every bug fix and every behavior change starts with a test
  that fails on the current code. Show the failure (quote it in the commit
  message or the review), then fix. [style 2](docs/style.md#rule-test-first),
  [design](docs/design.md#rule-design-test-first)
- **Tests that need root or control over the kernel run in QEMU guests**, and
  that is every test of dcfs itself, unit tests included: no code paths exist
  only so such a test can run outside the guest. The guests boot a minimal
  kernel in about a second. Checks that need neither (clang-tidy, formatting,
  model checking a specification) may run on the host.
  [style 2](docs/style.md#rule-tests-in-qemu),
  [design](docs/design.md#rule-design-everything-in-qemu)
- **No new shell for guest tests** (from step 25.34): a new test that mounts
  dcfs, crashes it or compares trees is a `qemu_cc_test` in `test/qemu/guest/`
  using the helpers of `guest.h`; shell under `test/qemu/` is thin glue only,
  and `tools/repo_shape.py` refuses a new `*.sh` there that
  `tools/repo_shape_guest_shell.txt` does not list.
  [style 4](docs/style.md#rule-new-guest-script-thin-glue)
- **Fakes, not mocks.** Inject dependencies and use small fake implementations.
  Test-only code never lives in production files: it goes in `*_test.cc`, or in
  a `testonly/` directory whose BUILD targets set `testonly = 1`. Syscall fault
  injection uses link-time `-Wl,--wrap` fakes.
  [style 2](docs/style.md#rule-fakes-not-mocks),
  [design (russ, 2026-10-16)](docs/design.md#rule-link-time-fault-injection)
- **No test-only things in production code** (russ, 2026-10-09): "If a test
  needs a knob in the prod code, make that knob a real feature of the class /
  method. There should be zero `// only use in tests` style comments." A seam a
  test needs is designed and documented as a feature with its production
  default. [style 8](docs/style.md#rule-no-test-only-things); no test peers
  (russ, 2026-10-09): [style 8](docs/style.md#rule-no-test-peers)
- **Coverage** (from phase 7/8): new code arrives fully covered by tests, error
  paths included. CI fails if total coverage drops. Gaps that cannot be covered
  are listed with a reason in `docs/coverage.md`. You must not lower the
  coverage baseline. That's why it exists. Only ever increase it (russ,
  2026-10-11). [style 2](docs/style.md#rule-never-lower-coverage),
  [process](docs/plan/process.md#rule-coverage-baseline)
- **Test sizes are tiers**: Bazel `size` is the tier, and every test declares
  `size` and `timeout` explicitly (the QEMU test macros refuse a missing one).
  `small` tests run constantly during development (`bazel test --config=fast
  //...`), `medium` before submitting (`--config=presubmit`), `large`/`enormous`
  in CI (plain `bazel test //...`). The soak test is tagged `manual` and only
  runs when asked for by name. See `test/qemu/README.md` "Test tiers".
  [style 2](docs/style.md#rule-test-tiers)
- **A test's verdict does not depend on host load or speed** (russ, 2026-10-17:
  "in general for all our tests I'm interested in them being load-agnostic"):
  count events and wait on events; time is only a hang guard sized by the
  harness. [style 2](docs/style.md#rule-load-agnostic-tests)
- **Shard with Bazel's `shard_count`** (russ, 2026-10-18, step 5.12).
  [style 2](docs/style.md#rule-shard-count)
- **The kernel-facts pins test the kernel, not dcfs** (russ, 2026-10-18): they
  run only in the weekly workflow.
  [style 2](docs/style.md#rule-kernel-facts-weekly)
- Never weaken a test to make it faster or to make it pass.
  [style 2](docs/style.md#rule-test-first)
- **Test conventions**: googletest fixtures ([style
  2](docs/style.md#rule-googletest)), extend an existing test or write a new one
  by the cost model ([style 2](docs/style.md#rule-extend-or-new-test)), matchers
  over booleans ([style 2](docs/style.md#rule-matchers-over-booleans)), a test
  helper does not reuse a production struct ([style
  2](docs/style.md#rule-test-helper-structs)), a test comment does not start
  with its step number (russ, 2026-10-17:
  [style 2](docs/style.md#rule-test-comments)), and a test ported from a script
  is named by a behavior sentence ([style
  2](docs/style.md#rule-ported-test-names)).
- **Tests may crash on what is not under test** (russ, 2026-10-09/11): setup
  failures use Abseil `CHECK`/`CHECK_OK`; googletest `ASSERT`/`EXPECT` are for
  the property under test. [style 1.6b](docs/style.md#rule-tests-may-crash)

## Running Bazel

- Run Bazel with access to `/dev/kvm`. The login session has the `kvm` group
  directly, so `sg kvm` is no longer needed anywhere (russ, 2026-10-11). A Bazel
  server started without KVM access makes every guest fall back to slow
  emulation: run `bazel shutdown` first when switching.
  [process](docs/plan/process.md#rule-bazel-kvm)
- **Everything goes through Bazel.** Every build step, tool, image and test is a
  Bazel target with declared inputs; nothing is built or run by a script outside
  the graph. That is what makes Bazel's caching of builds and test results
  sound, locally and in CI (the restored disk cache lets unchanged tests count
  as passed, and there is no periodic forced rerun: flake hunting is a targeted
  `--runs_per_test` on the changed tests).
  [style 3](docs/style.md#rule-everything-through-bazel)
- Never pipe Bazel through `tail` or `head` in an `&&` chain: the pipe hides
  Bazel's exit status. [process](docs/plan/process.md#rule-no-pipe-bazel)
- Shut down Bazel servers you started in temporary worktrees when you are done
  with them. [style 3](docs/style.md#rule-everything-through-bazel)
- **Bazel files** follow Bazel's BUILD style guide as buildifier enforces it.
  [style 3](docs/style.md#rule-bazel-style)
- **Third-party code is fetched and built by Bazel** (from phase 4), not checked
  in unless there is no other way. Prefer the Bazel Central Registry (in
  `MODULE.bazel`); otherwise `http_archive`/`http_file` with a sha256 and Bazel
  rules that build it. `third_party/<name>/` holds only what we write for it
  (BUILD overlays, rules, patches, config, lock files) and a README saying how
  to update the pin. Every tool the build or the tests use is pinned this way.
  The exception is Alpine's packages (from phase 24: the test kernel, QEMU, the
  mkfs tools, busybox): they are pinned to a stable release branch, not to a
  hash, and verified by Alpine's package signatures
  (`third_party/alpine/README.md`).
  [style 3](docs/style.md#rule-third-party-by-bazel)
- **Sanitizer runs do not block a step** (russ, 2026-10-06): a step's agent runs
  its own tests and the plain suite; the full ASan suite runs on the merged
  commit. [process](docs/plan/process.md#rule-sanitizers-do-not-block)

## Code

- **Root is required.** dcfs runs as root; there are no unprivileged fallbacks.
  [design, Assumptions](docs/design.md#rule-root-required)
- **Warnings are errors.** Today: `-Werror`. From phase 7: clang's `-Weverything
  -Werror` for our code, with every disabled warning listed in `.bazelrc` with a
  reason. [style 1.1](docs/style.md#rule-warnings-are-errors)
- **Tri-state records.** Every database record that mirrors something on the
  backing filesystem is present, absent or unknown, and a mutation sets it
  unknown before the backing syscall and present/absent after.
  [style 1.8](docs/style.md#rule-tri-state-records)
- **The protocol has a model.** A change to the write-through protocol (mutation
  phases, fills and their guards, completeness, the dirty set, sync points,
  recovery) updates the TLA+ model in `formal/` in the same change, and `bazel
  test //formal/...` passes (see `formal/README.md`, "Changing the model"). A
  protocol bug the model can express gets a `formal/known_bugs/` variant whose
  test expects its counterexample. Fixing a gap listed in `formal/findings/`
  moves its configuration into the real model. From phase 12.2, trace validation
  fails if the code does something the model does not allow.
  [style 1.8](docs/style.md#rule-protocol-change-updates-model),
  [design](docs/design.md#rule-design-protocol-model),
  [style 6](docs/style.md#rule-tla-conventions)
- **Requests are cancellable** (from phase 22): any operation that can take more
  than about 100 ms on a slow backing checks for interruption at interruption
  points (`dcfs/interruption_point.h`), each just before a backing syscall and
  never between a mutation's syscall and its phase 3, and has a cancellation
  test; cancellation never breaks the tri-state rule.
  [style 1.8](docs/style.md#rule-requests-cancellable),
  [design](docs/design.md#rule-design-cancellation)
- **Behave like the backing, only faster.** With dcfs out of the loop, what does
  the backing do for this operation, over time? dcfs does that
  (`docs/design.md`, "Behave like the backing, only faster"): the same results,
  incremental where it is, interruptible where it is, no stronger and no weaker
  guarantees. A claim about what the backing, the kernel or POSIX promises cites
  its source (POSIX text, a man page, or the kernel code path) where it is
  written, and where it can be measured, a native differential test (the same
  operations with dcfs out of the loop) backs it. russ, 2026-10-17.
  [design](docs/design.md#rule-behave-like-backing)
- **Where the backing's contract allows several answers, give the most useful
  one** (russ, 2026-10-17: "When there's ambiguity, you should be choosing the
  most _useful_ result."). [style 1.12](docs/style.md#rule-most-useful-result)
- **Each FUSE operation keeps the contract the backing syscall gives its
  caller** (russ, 2026-10-10), races included.
  [style 1.12](docs/style.md#rule-fuse-op-keeps-backing-contract)
- **Bounded memory.** A request's working memory is O(1) in the size of the
  files and directories it touches; the cache database grows with the
  filesystem, the working memory does not (`docs/design.md`, "Bounded memory").
  Any exception is a recorded decision with its reason.
  [design](docs/design.md#rule-bounded-memory)
- **File names are bytes.** Never treat a name, symlink target or xattr name as
  text; escape it whenever it is printed (from phase 9).
  [style 1.8](docs/style.md#rule-names-are-bytes),
  [design](docs/design.md#rule-design-names-are-bytes)
- **Descriptors and handles, never paths**, after startup.
  [style 1.8](docs/style.md#rule-no-paths-after-startup),
  [design](docs/design.md#rule-fds-never-paths)
- **`Context &ctx` first** in every function that reads or writes the cache or
  backing; **no globals, singletons or `thread_local`**; **no database
  transaction spans a backing syscall**.
  [style 1.8](docs/style.md#rule-context-first),
  [style 1.8](docs/style.md#rule-no-globals),
  [style 1.8](docs/style.md#rule-no-txn-across-syscall)
- **No raw syscalls anywhere** (russ, 2026-10-07: a firm rule): syscalls go
  through `dcfs/syscalls*.h`, tests included.
  [style 1.5](docs/style.md#rule-no-raw-syscalls); one thin wrapper per syscall,
  named for its manpage: [style 1.5](docs/style.md#rule-syscall-wrappers);
  **prefer pidfds over plain pids** (russ, 2026-10-18):
  [style 1.5](docs/style.md#rule-pidfds-over-pids)
- **Syscall fault injection stays link-time** (russ, 2026-10-16: "I agree.
  Revert it."). [design](docs/design.md#rule-link-time-fault-injection)
- **Mechanize every rule you can** (russ, 2026-10-09: "Mechanical invariant
  enforcement is more valuable."; 2026-10-17: "Remember to mechanize all of the
  style rules you can."). [style 1.1](docs/style.md#rule-mechanize-rules); move
  logic to compile time (russ, 2026-10-17):
  [style 1.1](docs/style.md#rule-compile-time-logic)
- **No intentional crashes** (russ, 2026-10-09): no `CHECK`, `LOG(FATAL)`,
  `abort()` in production; `RET_CHECK` is fine; a crash needs russ's
  case-by-case approval. [style 1.6b](docs/style.md#rule-no-intentional-crashes)
- **No timers in the codebase** (russ, 2026-10-09: "Timers are inherently
  brittle. If things are slow, the system breaks."): wait on the event; a
  timeout only where the awaited thing cannot signal.
  [style 1.11](docs/style.md#rule-no-timers); while working, anything goes, but
  what is submitted obeys the rules (russ, 2026-10-11):
  [style 1.11](docs/style.md#rule-anything-goes-while-working); host-side tools
  get no blanket exemption: [style 1.11](docs/style.md#rule-timers-host-tools)
- **No retry loops** (russ, 2026-10-10: "if you think one is necessary, come to
  me"). [style 1.12](docs/style.md#rule-no-retry-loops)
- **Every branch names its reachable cause** (russ, 2026-10-10): "writing
  unnecessary code causes unnecessary cognitive overhead"; no code for the
  impossible, a surviving mutant blocks the merge.
  [style 1.10a](docs/style.md#rule-reachable-cause)
- **One abstraction over variants; branching is discouraged, not forbidden**
  (russ, 2026-10-08).
  [style 1.10](docs/style.md#rule-one-abstraction-over-variants),
  [style 1.10](docs/style.md#rule-branching-discouraged)
- **Express invariants in the type system, not in comments** (russ, 2026-10-17:
  "It's easy to ignore a comment. The compiler enforces types.").
  [style 1.4](docs/style.md#rule-types-over-comments),
  [style 1.4](docs/style.md#rule-enum-class)
- **Errors are `absl::Status`/`StatusOr`** and nothing else; built with
  `StatusBuilder` and `ErrnoToStatus`, propagated with Abseil's macros; a
  function that does several things is guards, then one straight line of
  effects. [style 1.6](docs/style.md#rule-error-handling-one-page),
  [style 1.6](docs/style.md#rule-status-only-error-channel),
  [style 1.6](docs/style.md#rule-statusbuilder),
  [style 1.6](docs/style.md#rule-propagate-with-macros),
  [style 1.6](docs/style.md#rule-status-messages),
  [style 1.6](docs/style.md#rule-status-never-dropped) (russ, 2026-10-17:
  "*ALWAYS* include the Status"),
  [style 1.6a](docs/style.md#rule-function-shape),
  [style 1.6a](docs/style.md#rule-signature-says-can-fail),
  [style 1.6a](docs/style.md#rule-program-output) (russ, 2026-10-10:
  `absl::PrintF`, never iostreams or C printf)
- **Return a failed Status or log, never both**; log levels (russ, 2026-10-08).
  [style 1.7](docs/style.md#rule-status-or-log),
  [style 1.7](docs/style.md#rule-logging-levels)
- **Sync-point terms** (russ, 2026-10-18: "fsync _is_ a sync point, just not a
  syncfs point."): a file's sync point runs no syncfs; "the periodic sync
  point"; an interruption point is named by where it is ("an fsync's
  syscall"). [style 1.9](docs/style.md#rule-sync-point-terms)
- **Comments** are facts at the line that needs them:
  [style 1.9](docs/style.md#rule-comments-self-explanatory),
  [style 1.9](docs/style.md#rule-comments-invariants),
  [style 1.9](docs/style.md#rule-no-echo-docstrings),
  [style 1.9](docs/style.md#rule-comments-lower-layer),
  [style 1.9](docs/style.md#rule-comments-name-source),
  [style 1.9](docs/style.md#rule-comments-length).
- **Rely on library defaults, and test the ones we rely on** (russ, 2026-10-18:
  "Rely on defaults, but test that our relied-upon defaults are true. That way
  prod code is minimized while still having protections.").
  [style 1.13](docs/style.md#rule-rely-on-defaults)
- **Follow Abseil's Tips of the Week as design guidance** (russ, 2026-10-09:
  "It's a 'rule', but not as strong of a rule as the Google style guide"): below
  `AGENTS.md`, `docs/style.md` and the Google guides.
  [style](docs/style.md#rule-precedence-abseil-tips), and reach for Abseil
  before writing a helper: [style 1.2](docs/style.md#rule-reach-for-abseil)
- **Format C++ with the repository's `.clang-format`.** From phase 7, clang-tidy
  (`.clang-tidy`) runs on our code and its findings are errors.
  [style 1.1](docs/style.md#rule-formatting-enforced),
  [style 1.1](docs/style.md#rule-structural-style-checks), a mechanical reformat
  commit uses the `style:` prefix:
  [style 1.1](docs/style.md#rule-reformat-commit); include what you use:
  [style 1.1](docs/style.md#rule-include-what-you-use); no copies of UAPI
  definitions: [style 1.1](docs/style.md#rule-no-uapi-copies)
- **Narrowing Google's C++ rules**:
  [optional and string_view](docs/style.md#rule-std-optional-string-view),
  [function names](docs/style.md#rule-function-names),
  [locals non-const (russ, 2026-10-11)](docs/style.md#rule-locals-non-const),
  [flags only in main (russ, 2026-10-11)](docs/style.md#rule-flags-in-main-only),
  [no function pointers for callables (russ, 2026-10-11)](docs/style.md#rule-no-function-pointers),
  [Abseil time types (russ, 2026-10-17)](docs/style.md#rule-absl-time),
  [container algorithms (russ, 2026-10-10)](docs/style.md#rule-absl-container-algorithms),
  [composition over inheritance (russ, 2026-10-18)](docs/style.md#rule-composition-over-inheritance),
  [`[[nodiscard]]` (russ, 2026-10-10)](docs/style.md#rule-nodiscard),
  [ownership](docs/style.md#rule-ownership),
  [module layout](docs/style.md#rule-module-layout),
  [ASCII only](docs/style.md#rule-ascii-only),
  [no exceptions](docs/style.md#rule-no-exceptions),
  [`ReadOne`](docs/style.md#rule-readone-unique-lookups); namespaces:
  [flat](docs/style.md#rule-flat-namespaces) (russ, 2026-10-07),
  [unqualified `dcfs` names](docs/style.md#rule-unqualified-dcfs-names) (russ,
  2026-10-10).
- **Shell, Python, TLA+ and host tools**: minimize shell, new tools are C++
  (russ, 2026-10-11/17: "we can do it in C++ with only a few helper functions
  just fine"): [style 4](docs/style.md#rule-cpp-over-shell),
  [host scripts are bash](docs/style.md#rule-host-scripts-bash),
  [guest scripts are busybox sh](docs/style.md#rule-guest-scripts-busybox-sh);
  host tooling has a style and is reviewed (russ, 2026-10-17: "I want *some*
  style. I just don't care what it is."):
  [style 5](docs/style.md#rule-host-tooling-has-style),
  [Python](docs/style.md#rule-python-style).
- **The orchestrator and the style reviewer decide style questions** from the
  guides; russ is asked rarely (russ, 2026-10-10).
  [process](docs/plan/process.md#rule-style-review-decides)
- **Follow `docs/style.md`**: naming, includes, how errors are built
  (`StatusBuilder`, `ErrnoToStatus`) and worded, logging, comments, tests,
  Bazel, shell, Python, docs and commit messages.
- **No backward compatibility until the first deployment** (russ, 2026-10-10/11:
  "we've never shipped dcfs"): a removed option, format or behaviour is simply
  gone; no migration of the cache schema.
  [process](docs/plan/process.md#rule-no-backcompat)
- **FUSE-only machinery is a fallback with an expiry** (russ, 2026-10-11):
  workaround first, kernel patch after everything else, the workaround written
  to be deleted. [design](docs/design.md#rule-fuse-only-fallback)
- **No interim compromises toward the coroutine design** (russ, 2026-09-27):
  simple, synchronous, single-threaded, reviewable in small chunks; the rules
  that hold now are listed in
  [design](docs/design.md#rule-coroutine-ready-rules);
  [design](docs/design.md#rule-no-interim-compromises)
- **Runtime invariant checks**: the rules the design states in prose are also
  checked while tests run.
  [design](docs/design.md#rule-runtime-invariant-checks)

## Process: how agents work

- **Run the style checks on your own diff before you report** (russ, 2026-10-10:
  "Make sure coders run clang-tidy before being shut down. Don't want to lose
  their context!"): `bazel test --config=fast //tools/...`.
  [process](docs/plan/process.md#rule-agent-runs-style-checks)
- **Review and report**: every step is reviewed before it merges; the agent's
  report lists the diff, the failing-first run, the tests and any deviation.
  [process](docs/plan/process.md#rule-step-review),
  [process](docs/plan/process.md#rule-agent-report),
  [process](docs/plan/process.md#rule-dispatch-prompt),
  [process](docs/plan/process.md#rule-merge-procedure)
- **Lanes**: steps run in the two long-lived lane clones, never in the main
  checkout, and never with the agent tool's worktree isolation.
  [process](docs/plan/process.md#rule-lane-clones),
  [CLAUDE.md](CLAUDE.md#rule-no-worktree-isolation),
  [CLAUDE.md](CLAUDE.md#rule-cd-into-lane)
- **Stop and report on any permission denial; do not work around it** (russ,
  2026-10-05). [process](docs/plan/process.md#rule-stop-on-denial)
- **Never push, open pull requests or send email**; russ does that.
  [process](docs/plan/process.md#rule-never-push). Confirm destructive actions
  first: [process](docs/plan/process.md#rule-confirm-destructive). Do not modify
  `~/Sources/linux`, `~/Sources/libfuse` or `~/Sources/fuse-generation-qemu`:
  [process](docs/plan/process.md#rule-dont-modify-linux)
- **Docker on this machine is production** (russ, 2026-10-06): never stop,
  restart, remove, prune or modify what you did not create.
  [CLAUDE.md](CLAUDE.md#rule-docker-is-production)
- **No background polling loops**; wait in the foreground (2026-10-07, about 90
  orphans found). [CLAUDE.md](CLAUDE.md#rule-no-polling-loops); never `pkill -f`
  a pattern in your own command line: [CLAUDE.md](CLAUDE.md#rule-no-pkill-self)
- **zsh with `noclobber`**: overwrite files with `>|`; `make` and `diff` are
  broken shell functions (use `command make`, `command diff`).
  [process](docs/plan/process.md#rule-zsh-noclobber)
- **Plan notes live in the repository**, not in session scratch space.
  [CLAUDE.md](CLAUDE.md#rule-repo-is-the-notebook)
- **Agents may set timers for themselves** (russ, 2026-10-17: "The timer ban
  only applies inside our codebase. Not to agents themselves.").
  [process](docs/plan/process.md#rule-agent-timers)
- **Questions for russ and rulings** (russ, 2026-10-17): the orchestrator rules
  on obvious cases and makes provisional rulings on the rest; when a ratified
  principle settles a question, rule, do not ask (russ, 2026-10-17: "Why isn't
  this obvious and therefore something you automatically answer?").
  [process](docs/plan/process.md#rule-questions-and-rulings),
  [process](docs/plan/process.md#rule-principle-settles-question)
- **The order of work is `README.md`'s Queue section** (russ, 2026-10-11).
  [process](docs/plan/process.md#rule-order-of-work)
- **Parallelism and budget**: at most two building agents plus two non-building
  ones; if a step needs more than about three agent runs, pause and tell russ.
  [process](docs/plan/process.md#rule-parallelism-and-budget),
  [process](docs/plan/process.md#rule-pause-after-three-runs); push points:
  [process](docs/plan/process.md#rule-push-points)
- **Red CI is not a problem until we ship** (russ, 2026-10-17); review is the
  bottleneck (russ, 2026-10-18).
  [process](docs/plan/process.md#rule-red-ci-until-ship),
  [process](docs/plan/process.md#rule-review-is-bottleneck)
- **Keep a list of failures and open questions for russ** in `log.md`, and keep
  going with what does not need him.
  [process](docs/plan/process.md#rule-needs-russ-list),
  [process](docs/plan/process.md#rule-keep-going-when-away)
- **The orchestrator oversees; it does not do the work** (russ, 2026-10-06): it
  decides, dispatches, reviews, merges and keeps the plan current.
  [CLAUDE.md](CLAUDE.md#rule-orchestrator-delegates),
  [process](docs/plan/process.md#rule-roles); commit to `main` at sensible
  points: [CLAUDE.md](CLAUDE.md#rule-commit-to-main)
- **The orchestrator's context is the expensive one** (russ, 2026-10-17):
  bookkeeping goes to a clerk in one batch, multi-command lookups to one
  subagent, status updates come from known state.
  [process](docs/plan/process.md#rule-orchestrator-context),
  [process](docs/plan/process.md#rule-clerk-batches),
  [process](docs/plan/process.md#rule-explore-for-lookups),
  [process](docs/plan/process.md#rule-status-from-known-state),
  [process](docs/plan/process.md#rule-chores-to-subagents)
- **File changes go through the Edit or Write tool, not a script** (russ,
  2026-10-09). [process](docs/plan/process.md#rule-edit-tool-not-scripts)
- **Propose formal-methods work unprompted** (russ, 2026-10-09).
  [process](docs/plan/process.md#rule-formal-methods-proposals)
- **Track review findings** (russ, 2026-10-17/18).
  [process](docs/plan/process.md#rule-track-review-findings)
- **Choose the effort on each dispatch** (russ, 2026-10-17).
  [process](docs/plan/process.md#rule-effort-per-dispatch)
- **The Co-Authored-By trailer names the model that wrote the commit** (russ,
  2026-10-18: "Whichever model is the author should be listed as co-author.").
  [process](docs/plan/process.md#rule-coauthor-trailer),
  [style 7](docs/style.md#rule-commit-body)
- **Borrowed files are copied in full**, not git-subtree'd (russ, 2026-09-27).
  [process](docs/plan/process.md#rule-borrowed-files-in-full)
- **Gemini and agy**: route each dispatch
  ([process](docs/plan/process.md#rule-route-before-dispatch)), invoke agy in
  the background ([process](docs/plan/process.md#rule-agy-invocation)), size
  jobs to the remaining pool
  ([process](docs/plan/process.md#rule-dont-start-unfinishable)), write jobs
  obey the same rules as a Claude subagent
  ([process](docs/plan/process.md#rule-agy-write-jobs)).
- **Quota**: read the pools live before any dispatch that spends quota (russ,
  2026-10-18); pace the Claude plan; read-only jobs go off the Claude plan.
  [process](docs/plan/process.md#rule-live-quota-readings),
  [process](docs/plan/process.md#rule-pace-claude-quota),
  [process](docs/plan/process.md#rule-readonly-jobs-off-claude)

## Docs and commits

- Design decisions and their reasons go in `docs/design.md`; user-facing
  behavior and limitations in `README.md`.
  [style 7](docs/style.md#rule-where-things-go)
- Everything in `docs/plan/` except the implementing agent's own "As built"
  notes in its step's phase file is the orchestrator's or its bookkeeping
  clerk's (status, log, other steps, the ledger). When a plan phase changes, the
  orchestrator updates its file in `docs/plan/phases/` and the status table in
  `docs/plan/README.md`, and appends to `docs/plan/log.md`; an agent reports
  what it needs. [style 7](docs/style.md#rule-where-things-go)
- Commit subjects start with the plan step they belong to, e.g. `3.2:
  third_party/linux: build the pinned kernel with Bazel`.
  [style 7](docs/style.md#rule-commit-subject)
- Prose and Markdown follow the style guide: full sentences, plain and specific.
  [style 7](docs/style.md#rule-prose-style),
  [style 7](docs/style.md#rule-markdown-style)
- Never push, publish or send anything outside the repository unless the
  maintainer asks. [process](docs/plan/process.md#rule-never-push)

