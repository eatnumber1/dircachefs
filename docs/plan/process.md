# Process: how phases are executed

How the phases in `phases/` get done. `/AGENTS.md` holds the rules every
change follows; this file is about orchestration. The original plan's
process (its waves and file ownership table) is in `history.md`.

## Roles

- **Orchestrator**: the strongest available model, in a long-running
  session with russ. It does not write step code. It prepares each step,
  dispatches it to a subagent, reviews every diff, runs the tests, merges,
  keeps `README.md`'s status table and `log.md` current, and talks to
  russ.
- **Subagents**: one step at a time, as one of the agent types in
  `/CLAUDE.md` (each fixes a model and an effort level), chosen from the
  phase's "Owner" line and `execution.md`:
  - Haiku for boilerplate with a precise spec and patterns to copy;
  - Sonnet by default;
  - Opus where subtle invariants live (the write-through protocol,
    identity, crash and failure handling, the TLA+ model).
  A step that fails review twice escalates one tier.
- **russ**: decides open questions, reviews the plan and anything marked
  for russ's review (e.g. the TLA+ model's README), sends anything outside
  the repository (LKML mail, pushes).

## A step, start to finish

1. **Prepare.** Steps run in one of two long-lived lane clones,
   `~/Sources/dircachefs-lanes/lane-1` and `lane-2` (at most two agents
   run at once). Each is a separate `git clone` of `~/Sources/dircachefs`
   (its `origin` is that local path, so fetching never touches the
   network), with its own refs: an agent's git mistakes cannot touch
   `main` or the other lane. For a new step the orchestrator runs, in the
   lane: `git fetch origin && git checkout -B step-N.M origin/main`.
   Reusing the lane keeps its Bazel output directory and server, so a
   step rebuilds only what changed; all checkouts share the disk cache
   (`.bazelrc`) and Bazel's download cache. Each lane has a git-ignored
   `user.bazelrc` with `startup --max_idle_secs=10800`, so its server
   stays warm between steps but exits after three idle hours. Do not
   create a new checkout per step. (Not the agent tool's automatic
   worktree isolation: on russ's machine it picked the home-directory
   dotfiles repository and triggered ssh prompts.)
2. **Dispatch.** The subagent's prompt contains: the worktree path and
   branch; the commit-subject prefix (the plan step, `N.M:`; never a
   review-finding label: eight `N4:` commits reached main on 2026-10-08
   and the CI subject gate rightly failed the push); the step's text
   from its phase file; `/AGENTS.md`; the files it
   should read first; the step's "done" criteria; and the instructions
   below. Agents are told not to explore the tree beyond that, not to touch
   files outside the step, to stop and report after two failed attempts at
   the same build or test error, and to leave the lane's Bazel server
   running (it is reused by the next step).
3. **Test first.** The agent writes the step's tests, runs them, and
   records them failing on the unchanged code (quoted output), then makes
   them pass.
4. **Report.** The agent ends with: a diff summary, the test commands it
   ran with results (including the failing-first run), coverage of the
   code it added (from Phase 7), and any deviation from the plan.
5. **Review** (orchestrator, in the lane): read the full diff and run the step's tests
   and the tier russ expects for the phase (see "Running tests"). Check:
   - the rules in `/AGENTS.md` (test first, fakes not mocks, test code
     out of production files, warnings, names as bytes, `third_party/`)
     and `/docs/style.md` (StatusBuilder helpers, flat namespaces,
     `syscalls::` call form, manpage-named wrappers only in `syscalls.h`,
     `enum class`, message wording, comment style);
   - the dcfs invariants: every cache/backing function takes `Context&`;
     no paths after startup (objects are reached by fd or handle; Phase 13
     opens names relative to a parent fd, which is still not a path); no
     database transaction spans a backing syscall; every syscall that can
     reach the backing filesystem (a backing fd, handle or name) is made
     in `backing.cc` (process-local syscalls may use `syscalls::` anywhere;
     russ, 2026-10-07); no globals or `thread_local`; every record that
     mirrors the backing filesystem follows the present/absent/unknown
     rule (from 26.2 these are also checked at run time by the checking
     build the small and medium tiers boot; the precise forms are in
     `docs/design.md` "Runtime invariant checks": the dirty set is NOT
     "equal to the unknown rows", and a held fd may coexist with a later
     writable open);
   - coverage of the new code (from Phase 7) and the TLA+ model updated if
     the protocol changed (from Phase 12);
   - docs updated (`docs/design.md`, `README.md`) and no drift from the
     plan; if the plan was wrong, the phase file is corrected in the same
     merge;
   - a change to start-up or shutdown is tested in `main.cc`'s order
     (the harness fixture's order hid 12.4b's probe running before the
     mount fds existed; one shared `backing::Startup()` for both);
   - every gate the step adds (a test or check whose job is to reject
     something) has a committed self-check with a known-bad fixture
     (Phase 26.1; `test/qemu/README.md` "Gates and their self-checks");
     a gate without one is a review finding.
   Fixes go back to the same agent with `SendMessage` (it keeps its
   context, which is cheaper than a new agent).
6. **Merge.** In the lane: rebase the step branch on `origin/main` and
   rerun the tests. Run `.github/ci/commit_subjects.sh origin/main..<branch>`
   in the lane (the CI subject gate; two pushed runs were lost to a
   `N4:` and a `12.5 review:` subject before this line existed). Then, in
   `~/Sources/dircachefs`: `git fetch
   ~/Sources/dircachefs-lanes/lane-K step-N.M` and `git merge --ff-only
   FETCH_HEAD`. Update the status table and `log.md` and commit them.
   The lane stays for the next step.
7. **Sync-point suites** run in a lane checked out at `main` (already
   built), not in `~/Sources/dircachefs`, whose Bazel server is russ's.

## Running tests

- Run Bazel with `/dev/kvm` access: `sg kvm -c 'bazel test //...'` when
  the login session predates joining the `kvm` group. A Bazel server
  started without KVM silently falls back to emulation: `bazel shutdown`
  first when switching.
- Never pipe Bazel through `tail`/`head` in an `&&` chain.
- Until Phase 6 lands: `bazel test //...` runs everything, about 30
  minutes for the full ext4/xfs/btrfs matrix; `bazel test //dcfs:...` or
  single targets during development; `--config=asan` at the end of each
  phase.
- From Phase 6: `--config=fast` while developing, `--config=presubmit`
  before review, everything (CI) before a phase is marked done.
- From Phase 5, CI must be green on the merged commit.
- Coverage baseline (russ, 2026-10-08): the gate fails on drops only; a
  rise prints the suggested bump. The orchestrator bumps
  `dcfs/coverage_baseline.txt` to the last CI run's reported value as part
  of declaring the next push point, so the floor stays within one push
  cycle of reality without red runs.
- Push points (russ, 2026-10-08): while development is fast, a push point
  needs every merged branch green on its tiers and nothing more; CI catches
  the sanitizer-only, coverage and large-tier failures. After launch, when
  development slows, russ wants green pushes: a push point becomes a sync
  point, and the candidate commit gets CI's checks locally first (plain
  full suite, asan, ubsan, the coverage gate, the mutation tool's changed
  mode) via one script that calls the same `.github/ci/*.sh` scripts.

## Sanitizer runs do not block steps (russ, 2026-10-06)

ASan (and later UBSan) suites are slow, so they are not part of a step's
"done" criteria:
- A step's agent runs the step's own tests (plain and, if the step
  touches C++, those targets under `--config=asan`) and the plain full
  suite, then reports. It does not wait for a full ASan suite.
- The orchestrator merges on a green plain suite and starts the full ASan
  suite on the merged commit in the background (in a lane checked out at
  `main`), while lanes move on to their next steps.
- A sanitizer failure becomes the next step for that area, test first,
  ahead of new work. Every sync point requires the full ASan suite green
  on the commit being synced.
- While a long Bazel command runs, an agent may do non-Bazel work (reading,
  writing code or docs for the same step); one checkout runs one Bazel
  command at a time.

## Parallelism and budget

- Phases run in order; the status table in `README.md` is the source of
  truth. Steps inside a phase may run in parallel only if they touch
  disjoint files.
- At most two building agents (Bazel builds or QEMU tests) plus two
  non-building agents (reviews, docs, drafting) at a time: the dev
  machine (4 CPUs, 11 GB of RAM) is the limit, not tokens (Claude Max
  since 2026-10-05). Amended 2026-10-09 after six lanes ran for a day:
  russ's weekly all-models limit was 65% used with the reset a week out,
  so tokens bind again. From then: about three lanes once the running fix
  rounds land; Opus reviews only for protocol, identity and oracle steps
  (mechanical and docs steps merge on the orchestrator's read plus CI);
  rules front-loaded into dispatch prompts so a review is a check, not a
  second design pass; low effort for mechanical work; the protocol-heavy
  queue sequenced rather than parallel. Amended 2026-10-10 (russ: "You
  don't need to avoid the quota limit at all costs. In fact, if you
  _don't_ use it all it's basically wasted money for credits. Just pace
  yourself."): the weekly budget is spent, not hoarded, at a pace that
  lands near 100% at the reset: divide what is left by the days left and
  run as many lanes as that rate buys (at 16% with six days left, about one
  busy Sonnet lane); near the end of the week, an unused remainder goes to
  queued work rather than being left. A red push still gets fixed whatever
  the pace. Each lane's `user.bazelrc` caps its tests (since 2026-10-10:
  `--jobs=4`, `--local_test_jobs=4`, 20 GB; the machine has 8 cores and
  62 GB). Prefer `SendMessage` follow-ups to
  new agents. Shut down agents, background loops and Bazel servers that
  are no longer needed.
- If a step needs more than about three agent runs to land, pause and tell
  russ: something is probably wrong with the step.
- **The agent that wrote the code runs the style checks on it** (russ,
  2026-10-10: "Make sure coders run clang-tidy before being shut down.
  Don't want to lose their context!"). Every coding agent's last act
  before its report is `bazel test --config=fast //tools/...` (which holds
  repo_shape, banned symbols, raw syscalls and, from 25.21, clang-tidy and
  the clang-query matchers) and fixing what it finds in its own diff;
  allowlist entries are for pre-existing code only. The orchestrator
  sends review and CI findings back to the same agent with
  `SendMessage`, not to a fresh one, for the same reason.
- **Style review: the orchestrator decides, russ is asked rarely** (russ,
  2026-10-10, after the first batch of six: "Use the style guidance you
  have, and if something's really unclear, I need more context than these
  questions are giving me. Use your judgement, but you're bringing me too
  many style questions at the moment"). Every phase-25 step, and any step
  whose diff is mostly shape rather than behaviour, gets a
  `dcfs-style-reviewer` pass before merge. Its report separates findings
  (violations of written rules) from numbered questions. The orchestrator
  answers the questions itself from `docs/style.md`, the catalogue and
  the rulings russ has already given, writes each answer into
  `docs/style.md` as a dated sentence (marked "orchestrator's ruling"
  rather than russ's words), and has the building agent apply it. A
  question goes to russ only when the guide and his prior rulings cannot
  settle it, it will recur, and the two answers lead to materially
  different code; then it goes with full context (the whole function,
  both versions as code, what each costs, where else the choice shows
  up), one at a time, not in batches. russ reads the dated rulings in
  `docs/style.md` at his leisure and overrides any he disagrees with.
- Since 7.1b (2026-10-08) the first Bazel command in an output base
  extracts `@dcfs_llvm` (a streaming extract of the 11.6 GB LLVM release
  to 1.9 GB): 4 CPU minutes, 10-15 minutes of wall on a loaded host. An
  agent's foreground command limit (10 minutes) kills it, and an
  interrupted repository fetch leaves the server wedged until `bazel
  shutdown`, after which the extraction starts over. So the first command
  in a lane after it rebases onto 7.1b (and again onto 7.1c, whose sysroot change re-extracts it) is `bazel fetch --repo=@dcfs_llvm`
  run as a background command (the Bash tool's `run_in_background`, not a
  polling loop), and nothing else runs in that lane until it returns.
- zsh with `noclobber`: overwrite files with `>|`. `make` and `diff` are
  broken shell functions on the command line: use `command make`,
  `command diff` (scripts with a shebang are unaffected).
- Until Phase 3 lands, the test kernel is built from `~/Sources/linux`
  into `~/.cache/dcfs/kernel-build`; afterwards Bazel fetches and builds
  the pinned release.

## Standing instructions from russ (as of 2026-10-05)

- Stop and report on any permission denial; do not work around it.
- Do not modify `~/Sources/linux`, `~/Sources/libfuse` or
  `~/Sources/fuse-generation-qemu` (the kernel patch's test suite, shared
  with reviewers) unless asked.
- Never push, open pull requests or send email; russ does that.
- Confirm destructive actions first.
- Keep a list of failures and open questions for russ in `log.md`
  ("Needs russ"), so russ can catch up after being away.
- When russ is away, keep going with whatever does not need russ, and
  collect what does.
