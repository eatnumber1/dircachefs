# Process: how phases are executed

How the phases in `phases/` get done. `/AGENTS.md` holds the rules every
change follows; this file is about orchestration. The original plan's
process (its waves and file ownership table) is in `history.md`.

## Roles

- <a id="rule-roles"></a>
  **Orchestrator**: the strongest available model, in a long-running
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

## Mechanical first (russ, 2026-10-18)

- <a id="rule-mechanical-first"></a>
  **Mechanical first.** russ, 2026-10-18: "in general, you seem to be
  forgetting to prioritize mechanical first in our process. How can we
  better avoid you forgetting to do that?" The rule as the orchestrator
  reads it (labelled as its reading; russ has not yet ratified this
  wording): for every incident, review finding, chore or own mistake, the
  first question is which check, script, type or skill stops it recurring;
  that is queued (or built) before or instead of a manual fix; a chore done
  by hand a second time becomes a script or skill; a manual fix without a
  queued mechanism says why none is possible. The queued mechanisms are in
  25.47 items 7 to 9 and 25.45.

## A step, start to finish

1. <a id="rule-lane-clones"></a>
   **Prepare.** Steps run in one of two long-lived lane clones,
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
2. <a id="rule-dispatch-prompt"></a>
   **Dispatch.** The subagent's prompt contains: the worktree path and
   branch; the commit-subject prefix (the plan step, `N.M:`; never a
   review-finding label: eight `N4:` commits reached main on 2026-10-08
   and the CI subject gate rightly failed the push); the step's text
   from its phase file; `/AGENTS.md`; the files it
   should read first; the step's "done" criteria; and the instructions
   below. Agents are told not to explore the tree beyond that, not to touch
   files outside the step, to stop and report after two failed attempts at
   the same build or test error, and to leave the lane's Bazel server
   running (it is reused by the next step). After dispatching, refresh
   the dashboard's lanes and steps (step 6 says how; a Haiku subagent does
   it), and do the same
   when a lane's agent finishes or is stopped, or a Gemini job ends.
3. **Test first.** The agent writes the step's tests, runs them, and
   records them failing on the unchanged code (quoted output), then makes
   them pass.
4. <a id="rule-agent-report"></a>
   **Report.** The agent ends with: a diff summary, the test commands it
   ran with results (including the failing-first run), coverage of the
   code it added (from Phase 7), any deviation from the plan, and any
   design decision with a real alternative (see "Rules for every agent").
5. <a id="rule-step-review"></a>
   **Review** (the `dcfs-style-reviewer` subagent plus the orchestrator,
   in the lane; and the Opus `dcfs-reviewer` too for protocol, identity
   and oracle steps): before a step merges, its full diff is reviewed by
   `dcfs-style-reviewer` (russ, 2026-10-16: the per-step review he asked
   for on 2026-10-12; `dcfs-code-reviewer` existed only in the temporary
   orchestrator's harness), which enforces `AGENTS.md`, `docs/style.md` (including section 1.9's
   Comments & Documentation Standards), Abseil Tips of the Week, and the
   project invariants before giving an APPROVAL. The orchestrator runs the
   step's tests and the tier russ expects for the phase (see "Running tests").
   Check:
   - the rules in `/AGENTS.md` (test first, fakes not mocks, test code
     out of production files, warnings, names as bytes, `third_party/`)
     and `/docs/style.md` (StatusBuilder helpers, flat namespaces,
     `syscalls::` call form, manpage-named wrappers only in `syscalls.h`,
     `enum class`, message wording, and the new Comments & Documentation
     Standards: Self-Explanatory Code First, Document Timeless Invariants /
     Deliberate Omissions / Subtle Caveats, and Ban Redundant Method-Name-Echoing
     Docstrings);
   - behaves like the backing, only faster (russ, 2026-10-17;
     `docs/design.md`, "Behave like the backing, only faster"): for each
     operation the diff touches ask "With dcfs not in the loop, what does
     the backing do for this operation, over time? Where does dcfs block,
     snapshot, retry or decide by its own rule where the backing would
     not? Does any buffer grow with directory or file size?"; a claim about what the backing, the kernel or POSIX promises
     cites its source (POSIX text, a man page, or the kernel code path)
     where it is written, and a native differential test backs it where it
     can be measured; a design decision with a real alternative is in the
     step's As built notes, and on the questions table if it changes
     user-visible behavior or a guarantee;
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
6. <a id="rule-merge-procedure"></a>
   **Merge.** In the lane: rebase the step branch on `origin/main` and
   rerun the tests. Run `.github/ci/commit_subjects.sh origin/main..<branch>`
   in the lane (the CI subject gate; two pushed runs were lost to a
   `N4:` and a `12.5 review:` subject before this line existed). Then, in
   `~/Sources/dircachefs`: `git fetch
   ~/Sources/dircachefs-lanes/lane-K step-N.M` and `git merge --ff-only
   FETCH_HEAD`. Update the status table and `log.md` and commit them.
   Then a Haiku subagent refreshes russ's private dashboard, "dcfs Mission
   Control" (https://claude.ai/artifact/EVypNNcogbNcxzRjBYp8rK). The
   uploads (asset publish, `ArtifactData` update with `if_version`, then
   `mark-uploaded`) are the subagent's, not the orchestrator's (russ,
   2026-10-18: "Use subagents!", sent while the orchestrator was
   hand-uploading dashboard datasets; the update's `data` holds `source` and
   `updated` at the top level; the subagent follows the
   `dcfs-dashboard-upload` skill). The refresh rewrites its four
   snapshot files (the steps in flight and queued, in the Queue's order;
   each lane's step, agent type, model and effort, which the
   `dashboard_lanes` hooks write to `~/.cache/dcfs-dashboard/lanes.json`
   on every agent launch and finish (`tools/agents/README.md`; after an
   upload, `mark-uploaded`, or the PostToolUse reminder; the Stop hook
   is silent about it); the
   quota readings
   from "Quota management" below; `gh run list --limit 12`; and
   `~/.cache/dcfs-dashboard/plan.json`, every plan step, its phase, title,
   state and evidence. plan.json is maintained by hand until 25.47 item 7
   adds its generator (`dashboard_lanes.py`, from `phases/*.md` and
   `log.md`); until then the dashboard's plan census is stale from
   2026-10-17,
   upload each as an asset and point its dataset at the new file with a
   fresh `updated` time, all datasets in one batch write. At most one
   refresh per turn, at its end, covering every change in it; old
   uploads are not deleted one by one (each refresh's calls re-read the
   orchestrator's whole context; russ agreed 2026-10-16). Its datasets go stale after a day or two and
   say so on the page, which is the reminder when a refresh is missed.
   Before any refresh, list what each lane's checkout is on (`git -C
   <lane> branch --show-current`) against the running agents, so the
   lanes table reflects the lanes, not memory.
   The lane stays for the next step.
7. **Sync-point suites** run in a lane checked out at `main` (already
   built), not in `~/Sources/dircachefs`, whose Bazel server is russ's.

## Running tests

- <a id="rule-bazel-kvm"></a>
  Run Bazel with access to `/dev/kvm`. The login session has the `kvm` group
  directly, so `sg kvm` is no longer needed anywhere (russ, 2026-10-11). A
  Bazel server started without KVM silently falls back to emulation:
  `bazel shutdown` first when switching.
- <a id="rule-no-pipe-bazel"></a>
  Never pipe Bazel through `tail`/`head` in an `&&` chain.
- Until Phase 6 lands: `bazel test //...` runs everything, about 30
  minutes for the full ext4/xfs/btrfs matrix; `bazel test //dcfs:...` or
  single targets during development; `--config=asan` at the end of each
  phase.
- From Phase 6: `--config=fast` while developing, `--config=presubmit`
  before review, everything (CI) before a phase is marked done.
- From Phase 5, CI must be green on the merged commit (superseded for
  large-tier failures by [red CI until ship](#rule-red-ci-until-ship), russ,
  2026-10-17: "Until we ship, a red CI is not a problem.").
- <a id="rule-coverage-baseline"></a>
  Coverage baseline (russ, 2026-10-08, 2026-10-11): the gate fails on drops
  only; a rise prints the suggested bump. The orchestrator bumps
  `dcfs/coverage_baseline.txt` to a higher value when coverage rises. **Never
  lower the coverage baseline** (russ, 2026-10-11: "You must not lower the
  coverage baseline. That's why it exists. Only ever increase it."). If a change
  would drop coverage, write tests to keep or raise the floor; never lower
  `dcfs/coverage_baseline.txt`.
- <a id="rule-push-points"></a>
  Push points (russ, 2026-10-08): while development is fast, a push point
  needs every merged branch green on its tiers and nothing more; CI catches
  the sanitizer-only, coverage and large-tier failures. After launch, when
  development slows, russ wants green pushes: a push point becomes a sync
  point, and the candidate commit gets CI's checks locally first (plain
  full suite, asan, ubsan, the coverage gate, the mutation tool's changed
  mode) via one script that calls the same `.github/ci/*.sh` scripts.

<a id="rule-sanitizers-do-not-block"></a>
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
- <a id="rule-parallelism-and-budget"></a>
  At most two building agents (Bazel builds or QEMU tests) plus two
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
  the pace. 2026-10-11: the pace was broken (91% to 98% in one day) by
  running three Sonnet-class agents beside a long interactive trial; from
  here the count of concurrently running Sonnet or Opus agents is the thing
  the orchestrator limits, not the lane count, and an interactive session
  with russ (a trial, a review conversation) counts as one. Each lane's `user.bazelrc` caps its tests (since 2026-10-10:
  `--jobs=4`, `--local_test_jobs=2` (since 2026-10-17; it was 4), 20 GB; the machine has 8 cores and
  62 GB). Every lane's `user.bazelrc` now also has `build --local_resources=cpu=4` (orchestrator, 2026-10-18)
  (time analysis 2026-10-18: lane 8 had no `user.bazelrc` and ran uncapped;
  with the cap a lane cannot start more than one 4-worker TLC beside a guest;
  agents spent 40 % of their active hours in Bazel on the oversubscribed host,
  `docs/plan/notes/time-analysis-2026-10-18.md` section 7). Prefer `SendMessage` follow-ups to
  new agents. Shut down agents, background loops and Bazel servers that
  are no longer needed.
- <a id="rule-pause-after-three-runs"></a>
  If a step needs more than about three agent runs to land, pause and tell
  russ: something is probably wrong with the step.
- **No backward compatibility until the first deployment** (russ,
  2026-10-10/11): moved to `docs/design.md`, "Assumptions".
- <a id="rule-order-of-work"></a>
  **The order of work is `README.md`'s Queue section** (russ, 2026-10-11),
  not the phase numbers: the spare-disk trial first, then budgets, the
  slowness, the atomicity contract, then the rest by what the trial
  teaches. New phases are frozen; a new idea goes into the parked list.
- <a id="rule-agent-runs-style-checks"></a>
  **The agent that wrote the code runs the style checks on it** (russ,
  2026-10-10: "Make sure coders run clang-tidy before being shut down.
  Don't want to lose their context!"). Every coding agent's last act
  before its report is `bazel test --config=fast //tools/...` (which holds
  repo_shape, banned symbols, raw syscalls and, from 25.21, clang-tidy and
  the clang-query matchers) and fixing what it finds in its own diff;
  allowlist entries are for pre-existing code only. The orchestrator
  sends review and CI findings back to the same agent with
  `SendMessage`, not to a fresh one, for the same reason.
- <a id="rule-style-review-decides"></a>
  **Style review: the orchestrator decides, russ is asked rarely** (russ,
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
- <a id="rule-zsh-noclobber"></a>
  zsh with `noclobber`: overwrite files with `>|`. `make` and `diff` are
  broken shell functions on the command line: use `command make`,
  `command diff` (scripts with a shebang are unaffected).
- Until Phase 3 lands, the test kernel is built from `~/Sources/linux`
  into `~/.cache/dcfs/kernel-build`; afterwards Bazel fetches and builds
  the pinned release.

## Standing instructions from russ (as of 2026-10-05)

- <a id="rule-stop-on-denial"></a>
  Stop and report on any permission denial; do not work around it.
- <a id="rule-dont-modify-linux"></a>
  Do not modify `~/Sources/linux`, `~/Sources/libfuse` or
  `~/Sources/fuse-generation-qemu` (the kernel patch's test suite, shared
  with reviewers) unless asked.
- <a id="rule-never-push"></a>
  Never push, open pull requests or send email; russ does that.
- <a id="rule-confirm-destructive"></a>Confirm destructive actions first.
- <a id="rule-needs-russ-list"></a>
  Keep a list of failures and open questions for russ in `log.md`
  ("Needs russ"), so russ can catch up after being away.
- <a id="rule-keep-going-when-away"></a>
  When russ is away, keep going with whatever does not need russ, and
  collect what does.

## Rules for every agent

What every agent that works a step does, wherever its brief is silent:
- <a id="rule-two-failures-stop"></a>
  If the same build or test error defeats you twice, stop and report.
- <a id="rule-report-only-observed"></a>
  Never report a result you did not observe: quote what you ran and what it
  printed.
- <a id="rule-lane-bazel-server-stays"></a>
  Leave the lane's Bazel server running at the end of a step; the next step
  reuses it (servers started in temporary worktrees are shut down: see
  `docs/style.md` 3, "Everything goes through Bazel").
- <a id="rule-stay-in-lane"></a>
  Never create, download or edit files outside your lane.
- <a id="rule-report-design-decisions"></a>
  A design decision you make inside a step that has a real alternative (one
  russ might choose differently) goes in your report for the step's As built
  notes, with its alternative; if it changes user-visible behaviour or a
  guarantee, it also goes on the questions table before the merge (see
  "Questions for russ, and rulings").
- <a id="rule-foreground-commands"></a>
  An agent working a step waits for a command in the foreground (600000 ms
  timeout; rerun if it needs longer) and never backgrounds it. The
  orchestrator's exceptions are the background `bazel fetch` of a lane's
  first command (see above) and agy jobs (`run_in_background`, see "Using
  Gemini through `agy`").
- <a id="rule-quiet-output"></a>
  **Keep build and test output out of the context window** (russ, 2026-10-18:
  "In general both pieces of advice are to keep bazel & test output out of the
  context window."). The two suggestions below are from an outside review russ
  shared on 2026-10-18.
  - Lanes run Bazel with `--config=agent`, so agents see only warnings,
    errors, failing tests and the exit status
    ([`docs/style.md`](/docs/style.md#rule-everything-through-bazel)).
  - The edit-compile-debug loop builds and tests only the targets it changes
    (`bazel test --config=agent //dcfs:foo_test`, or the one guest test), never
    `bazel test //...` or a repo-wide clang-tidy or style-checks run. Once before
    reporting, it runs the tiers its brief names (usually `--config=fast` for
    the affected packages plus `//tools/...`, and `--config=presubmit` for the
    affected targets); the full `//...` and the coverage run are CI's unless
    the brief asks. The README test-tier rows describe the tiers, not this loop
    ([`test/qemu/README.md`](/test/qemu/README.md#test-tiers)).
  - Read a test log or a CI log with `grep` or `tail` on the failing target,
    never `cat` a whole `test.log`, the `bazel-testlogs` tree or a `gh run log`
    (`gh run view --log-failed | grep ...`).
  - Never pipe Bazel through `tail` or `head` in an `&&` chain
    ([`rule-no-pipe-bazel`](#rule-no-pipe-bazel)).
  - A subagent returns a summary, not raw output.

<a id="rule-orchestrator-context"></a>
## The orchestrator's context is the expensive one (russ, 2026-10-17)

Every orchestrator tool call re-sends its whole conversation. So: the
orchestrator keeps judgement (reviewing diffs, deciding, talking to russ)
and hands mechanical bookkeeping (merge records in log.md and the phase
files, ledger rows, the census, plan text edits) to a dcfs-mechanical
clerk (Haiku, effort low) in one batched dispatch with exact instructions
(russ: "Agreed. But make it so you won't forget to do that." Also a memory
note). A self-handoff note in `docs/plan/notes/` lets russ compress the
orchestrator's context without losing the working state.

How that works in practice (russ, 2026-10-17 and 2026-10-18; each was a
slip he caught):
- <a id="rule-clerk-batches"></a>
  **Queue small plan and doc edits; send one clerk batch at a natural pause**
  ("Are you remembering to use the clerk to keep your token use low?",
  2026-10-17). (Orchestrator's reading:) only an edit that unblocks a running
  agent goes out at once. `dashboard_lanes.py clerk-queue add "<file>:
  <change>"` queues an item; the Stop hook asks for a batch when no subagent
  runs or at five items; `clerk-queue drain` prints the brief for one
  dcfs-mechanical dispatch.
- <a id="rule-explore-for-lookups"></a>
  **A question that needs more than one lookup goes to one Explore agent
  with all the sub-questions at once** ("Remember to use subagents to keep
  tool calls from wasting lots of tokens copying context!", 2026-10-18),
  (Orchestrator's reading:) even when each grep looks quick, the
  orchestrator only composes the answer from its report; CI log digging and
  any multi-command lookup go to one Haiku or Explore agent.
- <a id="rule-status-from-known-state"></a>
  **A status update is answered from known state**. (Orchestrator's reading:) no
  new tool calls unless something changed (a background `gh run watch` already
  reports CI), and when the orchestrator's context is very large it proposes a
  handoff and compaction at the next natural pause (russ, 2026-10-18: "Are you
  still using subagents for work, to save on your expensive context
  transfers?").
- <a id="rule-chores-to-subagents"></a>
  **Any chore of more than one tool call goes to a Haiku subagent** (russ,
  2026-10-18: "Use subagents!"). (Orchestrator's reading:
  one cheap command, such as a single quota read before a dispatch, stays
  inline, since delegating it costs a dispatch turn plus the clerk's tokens;
  better still is no orchestrator turn at all, a quota check in a PreToolUse
  hook on agent dispatch or as the dispatched agent's first step.)

## How the orchestrator works with russ

Standing preferences russ gave in conversation, collected here from the
orchestrator's notes (2026-10-18) so that they are in the repository:
- <a id="rule-edit-tool-not-scripts"></a>
  **File changes go through the Edit or Write tool, not a script** (russ,
  2026-10-09: "In the future, can you use your edit tool rather than a
  python script to edit files? It's prettier and easier for me to review in
  the Claude webui."). Every file the orchestrator changes itself (plan
  bookkeeping, memory notes) is one Edit or Write call
  per file; Bash is for commands (git, gh, bazel, grep). (Orchestrator's
  reading: this overrides any harness hint to prefer Bash for edits.)
- <a id="rule-red-ci-until-ship"></a>
  **Until we ship, a red CI is not a problem** (russ, 2026-10-17, declining
  a pre-push run of the large tests a change reaches: "No. Until we ship, a
  red CI is not a problem."). (Orchestrator's reading: the large and
  enormous tiers run in CI and failures there are fixed forward; do not
  propose pre-push or pre-merge gates for them, do not hedge a "push or not"
  answer on untested large tests: say push, watch CI and dispatch fixes for
  any failure. Revisit when dcfs ships.)
- <a id="rule-review-is-bottleneck"></a>
  **Review is the bottleneck, and tests are placed accordingly** (russ,
  2026-10-18: "Code review is expensive in tokens, and that's our real
  bottleneck"): (Orchestrator's decision under that rule, step 5.13:) a test
  moves to a later CI wave only if it is not catching bugs before review. The CI
  cache is large and not duplicated (russ, 2026-10-18: "I'd prefer a large CI
  cache to no cache hits, as long as there's no duplication."; step 5.6).
- <a id="rule-principle-settles-question"></a>
  **When a ratified principle settles a question, rule; do not ask** (russ,
  2026-10-17, on Q-19 (atomic `O_TRUNC`), which "behave like the backing"
  settles: "Yeah! Why isn't this obvious and therefore something you
  automatically answer?"). (Orchestrator's reading:) before putting a question
  to russ, check the plan, `docs/design.md`'s principles, the style rulings and
  the earlier rulings. If they settle it, rule, record it with the principle
  cited, and report it as decided. A recorded code comment giving a convenience
  reason is not a reason to ask. Ask only when principles conflict, none applies
  or the cost is out of proportion, and say which.
- <a id="rule-formal-methods-proposals"></a>
  **Propose formal-methods work unprompted** (russ, 2026-10-09: "The TLA+
  validates itself many times over. I want to expand use of formal methods",
  and "If you come up at any point with ideas for how to do even more with
  formal methods, let me know."). (Orchestrator's reading:) when a design
  question, a bug or a review turns up something a model could judge,
  propose the model step first and tell russ without being asked.
- <a id="rule-track-review-findings"></a>
  **Track review findings so trends show** (russ, 2026-10-17 and 2026-10-18:
  "I'd like to start collecting data on issues found during code review, so we
  can look for trends." and "Remember to track reviewer findings so we can look
  for patterns."). (Orchestrator's reading of the mechanism:) when any review
  reports (Claude reviewer, style reviewer, Gemini, Argon, a foreman Flash
  review), queue a clerk item to append its findings with
  `tools/agents/review_findings.py add --step S --round R --reviewer X --date D`
  to `docs/plan/review-findings.tsv` (a review with no findings gets a `none`
  row); at merge, fill each finding's outcome (fixed with a commit, declined, or
  question). `review_findings.py summary` reports patterns, which drive which
  style rules to mechanize (25.39).

  **A review ends with a findings block**, one finding per line, fields
  separated by " | ": severity (blocker|should|nit) | category (correctness,
  test-gap, model-gap, dead-code, comment-accuracy, contract-claim,
  status-handling, naming, test-shape, commit-hygiene, docs, style-other) |
  rule (a style.md section or AGENTS.md rule, or empty) | file:line | one-line
  summary | mechanizable (checked|could|no) | open. A review with no
  findings ends with the single line `none`.
- <a id="rule-effort-per-dispatch"></a>
  **Choose the effort on each dispatch** (russ, 2026-10-17, asking why
  every lane ran at high effort: the agent type's default had decided it).
  Pass `effort` explicitly, by rule 6 of "Using Gemini through `agy`", "Vary
  the effort knob per job" (russ, 2026-10-17): start one notch lower than
  seems needed and raise it only when the answer comes back shallow; its
  table gives judgement reviews Opus high and protocol or crash-safety work
  Opus high or xhigh.
- Recommended, not a rule (russ, 2026-10-18: "The fresh-agent rule can get
  adopted as a soft recommendation, not a rule. You should be allowed to make
  judgement calls on a case-by-case basis."): prefer a fresh agent with a full
  brief over resuming one whose context is very large (17.2e's implementer
  reached about 450k tokens on 2026-10-18; every resumed turn re-reads it). A
  judgement call case by case.
- <a id="rule-coauthor-trailer"></a>
  **The trailer names the model that wrote the commit** (russ, 2026-10-18:
  "Whichever model is the author should be listed as co-author.").
  (Orchestrator's reading:) a brief says "end with a Co-Authored-By trailer
  naming your model" (plus the session line), and never pastes a fixed trailer.
  The orchestrator's own commits carry its model (style 7).
- <a id="rule-borrowed-files-in-full"></a>
  **When borrowing files from another of russ's repositories, copy the files
  in full; do not use git subtrees** (russ, 2026-09-27). `~/Sources/
  fuse-generation-qemu` stays untouched (see the standing instructions
  above).

<a id="rule-agent-timers"></a>
## Timers (russ, 2026-10-17)

Timers in the codebase stay banned (style 1.11; the dashboard tool's hourly
meter reading is the one approved exception). Agents themselves may set
timers: a self-scheduled wakeup at a quota reset or a check-back on a long
job is fine (russ, 2026-10-17: "The timer ban only applies inside our
codebase. Not to agents themselves.").

<a id="rule-questions-and-rulings"></a>
## Questions for russ, and rulings (russ, 2026-10-17)

russ: "you can make rulings on cases that are very obvious, and also you
can make provisional rulings on non-obvious ones so work can proceed.
Maybe for the future, questions for me go somewhere that I don't have to
scroll way up in our conversation to see? Maybe on the dashboard?"
- The orchestrator rules on an obvious case itself (the guides or an
  earlier ruling settle it) and records it where the rule lives.
- A non-obvious case gets a provisional ruling, applied at once so the
  step proceeds, and goes on the dashboard's "Questions for you" table
  (dataset `questions`: status open, provisional or answered, the
  recommendation, the ruling) for russ to confirm or change; a change is
  then carried into the code. Reviewers propose the ruling with the
  question instead of blocking on it.
- The table is the place russ looks; a reply still names what is new on
  it in one line.

## Using Gemini through `agy` (2026-10-16)

russ is on Google AI Pro: the Antigravity quota is part of a paid
subscription, so the aim is to spend all of it before each weekly reset,
responsibly. Facts (local `agy -p "/usage"`, and Google's developer forum;
Google publishes no per-model numbers):
- Pools: the general Gemini pool (weekly and five-hour), Gemini 4 Argon's
  weekly limit, and a Claude-and-GPT pool. Argon's limit may be on top of
  the general pool, not instead of it (russ, 2026-10-16): an Argon job
  spends from both, so watch both and stop at whichever is lower. One Argon
  review took about 9% of Argon's week and 2% of the general pool.
- The Claude-and-GPT pool on Pro is a small baseline, drains much faster,
  and forum reports describe multi-day lockouts once it is empty; it was
  at 0% on 2026-10-16. Paid top-ups ("AI credits") exist.
Guidelines:
1. Read `agy -p "/usage"` before every dispatch and note the figures in
   log.md when a job starts.
What Gemini is good at (2026-10-16, published figures, mostly Google's own,
so treat them as leads): both Argon and 3.8 Flash take about a million
tokens of input, and Argon's long-context retrieval is reported best in
class (GraphWalks 84% at 256K-1M tokens, against Opus 5.5's 67%), so the
whole dcfs tree (dcfs/, the TLA+ models, the guest scripts, the plan) fits
in one Argon prompt. It is also positioned for defensive security work.
Its general coding and agentic scores are weaker than Opus's, and
independent rankings put it below Opus 5.5. So Gemini's work is
whole-repository reading that our Claude subagents, which read excerpts,
do badly: consistency between code, comments, README, design.md and the
plan; duplicate code across the tree; dead code and stale references
(15.11's leftovers are the kind); spec-to-code tracing (every TLA+ action
to its code site and back; every README promise to a test); and security
reading. Not for writing code, nor for subtle protocol judgement.
2. Gemini 3.8 Flash for volume (censuses, surveys, mechanical reading);
   Gemini 4 Argon for medium work (reviews, investigations), about ten jobs
   a week, stopping at 10% of its limit; the general pool down to 5%.
3. Claude and GPT models through agy are fine, and worth using since that
   pool is separate and unspent quota is wasted (russ, 2026-10-16: "Feel
   free to use Claude or GPT models, especially if it's a separate pool.
   Unspent credits are wasted money. Just be cognizant of the limits."):
   its Claude Opus/Sonnet are the same models as our subagents, so it
   takes read-only reviews that would otherwise spend the Claude plan;
   stop at 10% of that pool. Paid top-ups ("AI credits") are still not
   enabled without russ. Measured 2026-10-17: one Opus (high) review of
   a medium diff spent 56% of that pool's week and all of its five-hour
   window, and was cut off before writing anything; the pool cannot carry
   an Opus review. Use it only for small Sonnet jobs; reviews go to Argon.
4. <a id="rule-agy-write-jobs"></a>
   Safety: agy is a weaker model with shell access. Write work is allowed
   (russ, 2026-10-17: "Gemini isn't so bad it must be only read-only.
   Feel free to use it. But check its work and keep an eye on it."), under
   the same rules as a Claude subagent: its own lane checkout and step
   branch, `--sandbox`, the brief's AGENTS.md/CLAUDE.md rules spelled out
   in the prompt (never push, Docker is production, no timers or retry
   loops, test first), and the orchestrator reviews every diff before a
   merge, as it would a Claude subagent's (russ, the same day: "you can
   even be a little looser than that"): it spot-checks the claimed test
   runs (the quoted output, a rerun of what looks doubtful) rather than
   rerunning everything, and the style review is the usual one. agy's
   print mode cannot ask for approval and `--mode auto` is not on this
   account, so write jobs run with `--dangerously-skip-permissions` in their
   own lane checkout, without `--sandbox` (russ, 2026-10-17: "Maybe run it
   without a sandbox, like the opus models"; the sandbox kept Bazel from
   fetching, and a job hung on its first test run). agy ends a session after
   30 minutes of waiting on its background commands, so a write prompt
   says: no baseline Bazel run before the change, commands in the
   foreground, and the lane is one whose Bazel cache is warm (a fresh
   clone's first build outlasts the limit). Read-only jobs still use
   `--mode plan` and write to the orchestrator's scratchpad. No protocol,
   crash-safety or identity work (rule 6's table).
4a. Cost per job (measured 2026-10-17 from agy's transcripts, which log
   tokens per model call; russ: "Agreed"): a job's cost is roughly its
   number of model calls times its context, since every call re-sends the
   session. The query-plan worker took 355 calls (173 file views, many
   rereading slices; 133 commands), 51.7M cached context tokens and 260k
   output, about 5% of the Gemini week. So a prompt: (1) gives the map
   (files and line ranges) and says to read each file whole, once; (2)
   uses Flash medium for mechanical work, high only where judgement is
   needed; (3) keeps one task per job (a refactor and its test are two
   jobs); (4) prefers the narrow design to a wide refactor. The dispatch
   ledger records each job's calls and token totals from the transcript.
5. Narrow jobs, several of them (russ, 2026-10-17: "The long context is
   good, but be wary of Flash's limitations. Consider using multiple,
   narrowly targeted reviewers."). Give each job the whole tree as
   context but one question: one invariant, one document against the
   code, one directory's dead code. Where Flash breaks down is not
   known (russ: "Maybe it'll work as one big one? idk"), so it is
   measured: once 26.31 has verified the Argon sweep's 35 findings,
   Flash answers the same five questions as one broad job and as five
   narrow ones, and both are scored for precision and recall against
   the verified set. Measured 2026-10-17 (notes/gemini-sweep-2026-10-17.md,
   last section): five narrow Flash jobs found 11 of the 34 verified
   defects, one broad Flash job 4; use narrow jobs for Flash.
6. Vary the effort knob per job, for Gemini and Claude alike (russ,
   2026-10-17), and name the choice in the dispatch:

   | Job | Gemini | Claude subagent |
   |---|---|---|
   | Listing, census, grep-like survey | Flash low | Haiku low |
   | One narrow check (a doc against code, one directory) | Flash medium | Sonnet medium |
   | Verification of findings, an audit | Argon low or Flash high | Sonnet medium |
   | Review needing judgement | Argon medium | Sonnet high, Opus high |
   | Protocol, crash safety, a puzzling failure | not Gemini | Opus high or xhigh |

   Start a job one notch lower than seems needed and raise it only when
   the answer comes back shallow. The orchestrator's own effort is the
   session's, which russ sets.
   Reviewers (russ, 2026-10-18: "Ok to your recommendation. Feel free to
   change the reviewer default now."; the recommendation was the
   orchestrator's): Opus high by default, xhigh only for protocol,
   crash-safety and identity steps or after a shallow high review, until
   data says otherwise.
7. Gemini has no network: a prompt that asks about style tells it to read
   the pinned Google guides in `bazel-bin/third_party/google_styleguide/`
   (`cpp.md`, `python.md`, `shell.md`, `markdown.md`; build them with
   `bazel build //third_party/google_styleguide:guides` first, and name the
   section) and `docs/style.md`, not to recall them.
8. Gemini's findings are treated as leads, not facts: a claim that would
   change the plan is re-verified (12.18 is the precedent: its "not
   usable" verdict was wrong).
9. <a id="rule-route-before-dispatch"></a>
   Route before each dispatch (russ, 2026-10-17: "Also are you remembering
   to use Gemini when appropriate?", and 2026-10-18: "remember to use
   Gemini and 'agy Claude/GPT' too."). Ask "could Gemini Flash do this?" and
   consider the four routes: a Claude Code subagent, agy Gemini, agy Claude
   (`claude-opus-5-5-*`, `claude-sonnet-5-5-*`) and agy GPT-OSS. Read-only
   research, source greps, doc accuracy passes and self-contained tests go to
   Gemini, within its daily share of the remaining weekly pool; Claude keeps
   protocol and model work, reviews (Flash reviews proved too lenient) and
   judgement-heavy design. A job that needs Claude-grade judgement but not
   the session's tools can run through agy instead of a Claude Code subagent.
10. <a id="rule-agy-invocation"></a>
    Invoking agy (learned 2026-10-18). Jobs do not call agy directly: they
   go through `tools/agents/foreman.py` by the `dcfs-foreman` skill (rule
   12). Direct calls are for the usage reads only, `agy -p "/usage"` and
   `agy -p '/quota'`, run with the Bash tool's `run_in_background: true`
   when they might block (russ, 2026-10-18: "I think those agy invocations
   were blocking for you"). Facts that hold for foreman's own calls: the
   prompt is attached to `-p` with `=` (`-p="$(cat prompt.txt)"`, or a bare
   `-p --model` swallows the flag); `agy -p` ends when the model's turn
   ends and kills any background task it started, so foreman holds a
   stream-json session open until JOB-COMPLETE and the model is told to run
   everything in the foreground; `--mode auto` is not available on this
   account.
11. <a id="rule-dont-start-unfinishable"></a>
    Don't start work you can't finish (russ, 2026-10-18, offering the last of
   the week's Gemini quota): size a job to the remaining pool. With a few
   points left, pick a small bounded job (a short review), not a large one
   that could die midway. agy Opus at high effort drains its pool in minutes:
   check headroom before each such job and give it work that is safe to
   abandon.

12. <a id="rule-delegate-down"></a>
    **Delegate menial chunks down** (russ, 2026-10-19: "Make use of foreman
   a skill and put it in AGENTS.md." and "I'd like it if our subagents
   started doing this too, and themselves leveraging dumber agents for
   more of the menial work." and "Use the foreman script instead of
   calling agy directly."). Every agent, orchestrator or subagent, hands a
   bounded chunk that needs labour rather than judgement (surveys, greps
   across trees, doc passes, test boilerplate, warning-fix batches) to
   Gemini through `tools/agents/foreman.py`, by the `dcfs-foreman` skill
   (`.claude/skills/dcfs-foreman/SKILL.md`). It reconciles rule 10: jobs
   go through foreman, not a bare `agy -p`; only `agy -p '/quota'` and
   `/usage` stay direct. A Claude subagent cannot spawn another Claude
   subagent, so foreman is how it delegates down, and the Claude quota it
   would have spent stays for judgement work. The delegating agent reads
   the pool first, reviews the result before using it, records a ledger
   row naming its parent step, and puts a `Delegated:` line in its
   report. Rule 6's table still decides what may be delegated.

## Quota management, Claude and Gemini together (russ, 2026-10-16)

russ: "I'd like you to manage all quota spend, both Gemini and Claude. That
means Claude quota is more important, and you need to make sure we don't run
out of Claude before Gemini. Plan ahead!" and "unspent Claude quota is
wasted money." So both are spent to the end of their weeks, Claude never
first.
- <a id="rule-live-quota-readings"></a>
  **Readings.** Claude: `timeout 90 python3 -I tools/agents/claude_usage.py`
  (the session and weekly panels of `/usage`, through a pseudo-terminal;
  approximate: local sessions only). Gemini, and the Claude/GPT pool inside
  Antigravity: `agy -p "/usage"`. Read both before each dispatch; log a
  snapshot when a day starts. Read the pools live, never from the hourly
  dashboard reading alone (russ, 2026-10-18: "Why do we keep relying on
  stale hourly readings?"): the two commands above are the live read; a
  decision never rests on the hourly reading alone. `dashboard_lanes.py quota`
  is only what the hourly hook records for the dashboard
  (`quota.json`, `budget.json`). A live read is event-driven, not a timer.
- <a id="rule-pace-claude-quota"></a>
  **The Claude plan is the scarce resource** (the orchestrator runs on it and
  every coding agent does). Pace it so the week ends near 95%, never empty
  before its reset. russ's own interactive use outside the project happens
  throughout the week (russ, 2026-10-16: "save a little bit (maybe 5-10%?)
  for unrelated interactive use", then "my interactive use will happen
  throughout the week. That means as we get close to quota reset, you
  should target getting closer to 100% (maybe 95%?)"). So the reserve for
  him shrinks as the week runs out: the week's target is 95% at the reset,
  and on each day the plan leaves (days left / 7) x 10% unspent for him, on
  top of a reserve for red pushes that is released on the last day. Daily
  budget = (95% - used - his reserve - red-push reserve) / days left.
  **Thursday ramp (russ, 2026-10-18: "Since quotas reset on Friday, let's
  ramp up spend on Thursday to try to get as close to 100% spent as possible
  without going over. Don't worry about me doing out-of-band spend starting
  on Thursday.")**, amended (2026-10-18, russ: "I don't actually think you can
  effectively spend that slowly. Some of the items we have to do are big."):
  the Monday to Wednesday daily targets (73.4, 76.8, 80.2) no longer apply.
  The week overran (63% at 00:48 Sun, 83% at 12:50 Sun; the Sunday target was
  70), so spend comes in lumps, one whole job at a time, each measured by a
  live reading before and after it is dispatched: Mon, 17.2e's Opus re-review
  and its fix round; Tue, 5.13's Sonnet review and merge; Wed, nothing new
  (russ's use and red pushes only). The ceiling is about 91 at Wed end, then
  the Thursday ramp to 97 and about 99 by the reset (Fri 01:00 PT). A Thursday
  job starts only if its measured cost class fits what is left. From Thursday
  on no reserve is kept for russ or for red pushes. Cause of the overrun: Opus
  rounds on 22.5, 25.28b and 25.34 ran overnight with no pace check before
  each dispatch.
  russ, 2026-10-18 13:00 PT: 17.2e's Opus re-review runs now (started at 83%),
  not Monday.
  Measured cost class (2026-10-18, 17.2e round 3): an Opus re-review of a
  mid-sized step: <1% of the weekly quota (83% before at 12:55, 83% after at
  13:22). Use it to size the lumps above.
  The orchestrator itself is the largest consumer: lean turns, hands-on work
  delegated, Opus subagents only for protocol, identity, oracle and
  security-relevant work, Sonnet for routine implementation, Haiku for
  mechanical batches.
- <a id="rule-readonly-jobs-off-claude"></a>
  **Move every read-only job off the Claude plan when a pool allows it:**
  reviews, investigations and audits go to agy's Claude/GPT pool first (same
  models, separate quota), then to Gemini 4 Argon (whole-repository reading),
  then to Gemini 3.8 Flash (volume reading). Only writing, and judgement the
  agy models cannot be trusted with, stays on the Claude plan.
- **Spend Gemini to the end of its week too**: if a day's Claude budget is
  used up and Gemini quota remains, queue read-only Gemini work (the
  consistency, duplication, dead-code and spec-tracing sweeps of the
  Gemini section above) rather than leave it idle. If the Claude plan has
  headroom near its reset that would otherwise be lost, pull the next
  writing steps forward.
