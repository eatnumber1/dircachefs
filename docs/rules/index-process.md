# Index of process rules

Read this when you dispatch, review, merge or do plan bookkeeping, or run
commands on russ's machine (Bazel and KVM, lanes, Docker, polling loops, zsh):
how agents and the orchestrator work, quota, review and what never to do. Each
line is a ratified rule, in russ's words, or marked as an orchestrator ruling,
with the date where there is one and a link to the section that holds its
reasoning. `AGENTS.md` says which indexes to read for which task.

- The orchestrator oversees; it does not do the work (russ, 2026-10-06).
  [CLAUDE.md](/CLAUDE.md#rule-orchestrator-delegates),
  [process](/docs/plan/process.md#rule-roles)
- Lanes: steps run in the two lane clones, with a `cd` into the lane in every
  command, never in the main checkout and never with worktree isolation.
  [process](/docs/plan/process.md#rule-lane-clones),
  [CLAUDE.md](/CLAUDE.md#rule-cd-into-lane),
  [CLAUDE.md](/CLAUDE.md#rule-no-worktree-isolation)
- Dispatch, report, review and merge procedure.
  [process](/docs/plan/process.md#rule-dispatch-prompt),
  [process](/docs/plan/process.md#rule-agent-report),
  [process](/docs/plan/process.md#rule-step-review),
  [process](/docs/plan/process.md#rule-merge-procedure)
- A step is a series of reviewed slices (about 800 changed lines or one
  subsystem each, three review passes per slice), merged once (russ,
  2026-10-19: "I agree, however this means we need to allow a coding agent
  to make multiple commits, with a review for each commit, before eventually
  asking for a PR into main.").
  [process](/docs/plan/process.md#rule-reviewed-slices)
- Rules for every agent: stop after two failures, report only what you observed,
  stay in your lane, report design decisions with their alternative, leave the
  lane's Bazel server running, run commands in the foreground.
  [process](/docs/plan/process.md#rule-two-failures-stop),
  [process](/docs/plan/process.md#rule-report-only-observed),
  [process](/docs/plan/process.md#rule-stay-in-lane),
  [process](/docs/plan/process.md#rule-report-design-decisions),
  [process](/docs/plan/process.md#rule-lane-bazel-server-stays),
  [process](/docs/plan/process.md#rule-foreground-commands)
- Keep build and test output out of the context window: lanes run Bazel with
  `--config=agent`, the inner loop tests only changed targets, logs are read by
  grep, a subagent returns a summary (russ, 2026-10-18: "In general both pieces
  of advice are to keep bazel & test output out of the context window.").
  [process](/docs/plan/process.md#rule-quiet-output)
- Run Bazel with `/dev/kvm`, never pipe it through `tail` or `head` in an `&&`
  chain, and shut down servers you started in temporary worktrees.
  [process](/docs/plan/process.md#rule-bazel-kvm),
  [process](/docs/plan/process.md#rule-no-pipe-bazel)
- Sanitizer runs do not block a step (russ, 2026-10-06).
  [process](/docs/plan/process.md#rule-sanitizers-do-not-block)
- Run the style checks on your own diff before you report (russ, 2026-10-10:
  "Make sure coders run clang-tidy before being shut down. Don't want to lose
  their context!").
  [process](/docs/plan/process.md#rule-agent-runs-style-checks)
- The orchestrator and the style reviewer decide style questions; russ is asked
  rarely (russ, 2026-10-10).
  [process](/docs/plan/process.md#rule-style-review-decides)
- Stop and report on any permission denial (russ, 2026-10-05).
  [process](/docs/plan/process.md#rule-stop-on-denial)
- Never push, open pull requests, publish or send anything outside the
  repository (russ, 2026-10-05); confirm destructive actions first.
  [process](/docs/plan/process.md#rule-never-push),
  [process](/docs/plan/process.md#rule-confirm-destructive)
- Do not modify `~/Sources/linux`, `~/Sources/libfuse` or
  `~/Sources/fuse-generation-qemu`; copy borrowed files in full (russ,
  2026-09-27). [process](/docs/plan/process.md#rule-dont-modify-linux),
  [process](/docs/plan/process.md#rule-borrowed-files-in-full)
- Docker on this machine is production (russ, 2026-10-06).
  [CLAUDE.md](/CLAUDE.md#rule-docker-is-production)
- No background polling loops, and no `pkill -f` of a pattern in your own
  command line. [CLAUDE.md](/CLAUDE.md#rule-no-polling-loops),
  [CLAUDE.md](/CLAUDE.md#rule-no-pkill-self)
- zsh has `noclobber`: overwrite with `>|`; `make` and `diff` are broken
  functions. [process](/docs/plan/process.md#rule-zsh-noclobber)
- Plan notes live in the repository; commit to `main` at sensible points (russ,
  2026-10-05). [CLAUDE.md](/CLAUDE.md#rule-repo-is-the-notebook),
  [CLAUDE.md](/CLAUDE.md#rule-commit-to-main)
- Agents may set timers for themselves (russ, 2026-10-17: "The timer ban only
  applies inside our codebase. Not to agents themselves.").
  [process](/docs/plan/process.md#rule-agent-timers)
- Questions for russ and rulings; when a ratified principle settles a question,
  rule, do not ask (russ, 2026-10-17).
  [process](/docs/plan/process.md#rule-questions-and-rulings),
  [process](/docs/plan/process.md#rule-principle-settles-question)
- The order of work is README.md's Queue section (russ, 2026-10-11).
  [process](/docs/plan/process.md#rule-order-of-work)
- Parallelism and budget: at most two building agents plus two non-building
  ones; pause if a step needs more than about three runs; push points.
  [process](/docs/plan/process.md#rule-parallelism-and-budget),
  [process](/docs/plan/process.md#rule-pause-after-three-runs),
  [process](/docs/plan/process.md#rule-push-points)
- Keep a list of failures and open questions for russ, and keep going with what
  does not need him. [process](/docs/plan/process.md#rule-needs-russ-list),
  [process](/docs/plan/process.md#rule-keep-going-when-away)
- The orchestrator's context is the expensive one (russ, 2026-10-17): clerk
  batches, one Explore agent for multi-lookups, status from known state, chores
  to subagents. [process](/docs/plan/process.md#rule-orchestrator-context),
  [process](/docs/plan/process.md#rule-clerk-batches),
  [process](/docs/plan/process.md#rule-explore-for-lookups),
  [process](/docs/plan/process.md#rule-status-from-known-state),
  [process](/docs/plan/process.md#rule-chores-to-subagents)
- File changes go through the Edit or Write tool, not a script (russ,
  2026-10-09). [process](/docs/plan/process.md#rule-edit-tool-not-scripts)
- Propose formal-methods work unprompted (russ, 2026-10-09).
  [process](/docs/plan/process.md#rule-formal-methods-proposals)
- Track review findings; a review ends with a findings block (russ,
  2026-10-17/18). [process](/docs/plan/process.md#rule-track-review-findings)
- Choose the effort on each dispatch (russ, 2026-10-17).
  [process](/docs/plan/process.md#rule-effort-per-dispatch)
- The trailer names the model that wrote the commit (russ, 2026-10-18).
  [process](/docs/plan/process.md#rule-coauthor-trailer)
- Gemini and agy: route each dispatch, run jobs through foreman (direct agy
  only for usage reads), size jobs to the pool, write jobs obey the same
  rules.
  [process](/docs/plan/process.md#rule-route-before-dispatch),
  [process](/docs/plan/process.md#rule-agy-invocation),
  [process](/docs/plan/process.md#rule-dont-start-unfinishable),
  [process](/docs/plan/process.md#rule-agy-write-jobs)
- Delegate menial chunks to Gemini through foreman, by the `dcfs-foreman`
  skill, not a bare `agy -p` (russ, 2026-10-19: "Make use of foreman a skill
  and put it in AGENTS.md."; "Use the foreman script instead of calling agy
  directly."). [process](/docs/plan/process.md#rule-delegate-down)
- Quota: read the pools live, pace the Claude plan, move read-only jobs off it
  (russ, 2026-10-18). [process](/docs/plan/process.md#rule-live-quota-readings),
  [process](/docs/plan/process.md#rule-pace-claude-quota),
  [process](/docs/plan/process.md#rule-readonly-jobs-off-claude)
- Mechanical first: for every incident, review finding, chore or own mistake,
  the first question is which check, script, type or skill stops it recurring
  (russ, 2026-10-18, "prioritize mechanical first"; the orchestrator's reading,
  not yet ratified). [process](/docs/plan/process.md#rule-mechanical-first)
