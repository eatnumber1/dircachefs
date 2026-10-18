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
   running (it is reused by the next step). After dispatching, refresh
   the dashboard's lanes and steps (step 6 says how), and do the same
   when a lane's agent finishes or is stopped, or a Gemini job ends.
3. **Test first.** The agent writes the step's tests, runs them, and
   records them failing on the unchanged code (quoted output), then makes
   them pass.
4. **Report.** The agent ends with: a diff summary, the test commands it
   ran with results (including the failing-first run), coverage of the
   code it added (from Phase 7), and any deviation from the plan.
5. **Review** (the `dcfs-style-reviewer` subagent plus the orchestrator,
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
6. **Merge.** In the lane: rebase the step branch on `origin/main` and
   rerun the tests. Run `.github/ci/commit_subjects.sh origin/main..<branch>`
   in the lane (the CI subject gate; two pushed runs were lost to a
   `N4:` and a `12.5 review:` subject before this line existed). Then, in
   `~/Sources/dircachefs`: `git fetch
   ~/Sources/dircachefs-lanes/lane-K step-N.M` and `git merge --ff-only
   FETCH_HEAD`. Update the status table and `log.md` and commit them.
   Then refresh russ's private dashboard, "dcfs Mission Control"
   (https://claude.ai/artifact/EVypNNcogbNcxzRjBYp8rK): rewrite its four
   snapshot files (the steps in flight and queued, in the Queue's order;
   each lane's step, agent type, model and effort, which the
   `dashboard_lanes` hooks write to `~/.cache/dcfs-dashboard/lanes.json`
   on every agent launch and finish (`tools/agents/README.md`; after an
   upload, `mark-uploaded`, or the Stop hook holds the turn open); the
   quota readings
   from "Quota management" below; `gh run list --limit 12`; and
   `~/.cache/dcfs-dashboard/plan.json`, every plan step and its state
   from the 2026-10-17 census, in which a merge flips its step to done
   and a new step is added as queued),
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

- Run Bazel with access to `/dev/kvm`. The login session has the `kvm` group
  directly, so `sg kvm` is no longer needed anywhere (russ, 2026-10-11). A
  Bazel server started without KVM silently falls back to emulation:
  `bazel shutdown` first when switching.
- Never pipe Bazel through `tail`/`head` in an `&&` chain.
- Until Phase 6 lands: `bazel test //...` runs everything, about 30
  minutes for the full ext4/xfs/btrfs matrix; `bazel test //dcfs:...` or
  single targets during development; `--config=asan` at the end of each
  phase.
- From Phase 6: `--config=fast` while developing, `--config=presubmit`
  before review, everything (CI) before a phase is marked done.
- From Phase 5, CI must be green on the merged commit.
- Coverage baseline (russ, 2026-10-08, 2026-10-11): the gate fails on drops only; a
  rise prints the suggested bump. The orchestrator bumps
  `dcfs/coverage_baseline.txt` to a higher value when coverage rises.
  **Never lower the coverage baseline** (russ, 2026-10-11: "You must not lower
  the coverage baseline. That's why it exists. Only ever increase it."). If a
  change would drop coverage, write tests to keep or raise the floor; never
  lower `dcfs/coverage_baseline.txt`.
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
- If a step needs more than about three agent runs to land, pause and tell
  russ: something is probably wrong with the step.
- **No backward compatibility until the first deployment** (russ, several
  times, 2026-10-10/11: "we've never shipped dcfs", "we haven't shipped. No
  need for back-compat"): a removed option, format or behaviour is simply
  gone, with no shim, deprecation message, migration or test of the old
  spelling. That includes the cache database schema: a schema change before
  the first deployment bumps the version and starts fresh, no migration
  (russ, 2026-10-11: "I don't own trial databases. I don't care about
  migration until we ship ... back-compat is *irrelevant* right now"). The
  migration machinery (`migrate.cc`) stays in place for the day it matters
  and stays tested (russ: "The migration machinery should still be
  tested though"): `migrate_test.cc` (18 cases today) keeps exercising it
  against synthetic older-schema databases built by the test, and keeps
  passing under the coverage gate; what pre-ship schema changes skip is
  writing a migration for the real previous version, not the tests of the
  mechanism.
- **The order of work is `README.md`'s Queue section** (russ, 2026-10-11),
  not the phase numbers: the spare-disk trial first, then budgets, the
  slowness, the atomicity contract, then the rest by what the trial
  teaches. New phases are frozen; a new idea goes into the parked list.
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

## The orchestrator's context is the expensive one (russ, 2026-10-17)

Every orchestrator tool call re-sends its whole conversation. So: the
orchestrator keeps judgement (reviewing diffs, deciding, talking to russ)
and hands mechanical bookkeeping (merge records in log.md and the phase
files, ledger rows, the census, plan text edits) to a dcfs-mechanical
clerk (Haiku, effort low) in one batched dispatch with exact instructions
(russ: "Agreed. But make it so you won't forget to do that." Also a memory
note). A self-handoff note in `docs/plan/notes/` lets russ compress the
orchestrator's context without losing the working state.

## Timers (russ, 2026-10-17)

Timers in the codebase stay banned (style 1.11; the dashboard tool's hourly
meter reading is the one approved exception). Agents themselves may set
timers: a self-scheduled wakeup at a quota reset or a check-back on a long
job is fine (russ, 2026-10-17: "The timer ban only applies inside our
codebase. Not to agents themselves.").

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
4. Safety: agy is a weaker model with shell access. Write work is allowed
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
7. Gemini has no network: a prompt that asks about style tells it to read
   the pinned Google guides in `bazel-bin/third_party/google_styleguide/`
   (`cpp.md`, `python.md`, `shell.md`, `markdown.md`; build them with
   `bazel build //third_party/google_styleguide:guides` first, and name the
   section) and `docs/style.md`, not to recall them.
8. Gemini's findings are treated as leads, not facts: a claim that would
   change the plan is re-verified (12.18 is the precedent: its "not
   usable" verdict was wrong).

## Quota management, Claude and Gemini together (russ, 2026-10-16)

russ: "I'd like you to manage all quota spend, both Gemini and Claude. That
means Claude quota is more important, and you need to make sure we don't run
out of Claude before Gemini. Plan ahead!" and "unspent Claude quota is
wasted money." So both are spent to the end of their weeks, Claude never
first.
- **Readings.** Claude: `timeout 90 python3 -I tools/agents/claude_usage.py`
  (the session and weekly panels of `/usage`, through a pseudo-terminal;
  approximate: local sessions only). Gemini, and the Claude/GPT pool inside
  Antigravity: `agy -p "/usage"`. Read both before each dispatch; log a
  snapshot when a day starts.
- **The Claude plan is the scarce resource** (the orchestrator runs on it and
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
  on Thursday.")**: the Monday to Wednesday targets stay (73.4, 76.8, 80.2),
  Thursday ends at 97%, and spend reaches about 99% by the reset (Fri 01:00
  PT). From Thursday on no reserve is kept for russ or for red pushes.
  The orchestrator itself is the largest consumer: lean turns, hands-on work
  delegated, Opus subagents only for protocol, identity, oracle and
  security-relevant work, Sonnet for routine implementation, Haiku for
  mechanical batches.
- **Move every read-only job off the Claude plan when a pool allows it:**
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
