# Index of process rules

Read this when you dispatch, review, merge or do plan bookkeeping, or run Bazel
and agents on russ's machine: how agents and the orchestrator work, quota,
review, commits and what never to do. Each line is a ratified rule, in russ's
words with the date where there is one, and links to the section that holds its
reasoning. `AGENTS.md` says which indexes to read for which task.

- Run Bazel with access to `/dev/kvm`. The login session has the `kvm` group
  directly, so `sg kvm` is no longer needed anywhere (russ, 2026-10-11). A Bazel
  server started without KVM access makes every guest fall back to slow
  emulation: run `bazel shutdown` first when switching.
  [process](../plan/process.md#rule-bazel-kvm)
- **Everything goes through Bazel.** Every build step, tool, image and test is a
  Bazel target with declared inputs; nothing is built or run by a script outside
  the graph. That is what makes Bazel's caching of builds and test results
  sound, locally and in CI (the restored disk cache lets unchanged tests count
  as passed, and there is no periodic forced rerun: flake hunting is a targeted
  `--runs_per_test` on the changed tests).
  [style 3](../style.md#rule-everything-through-bazel)
- Never pipe Bazel through `tail` or `head` in an `&&` chain: the pipe hides
  Bazel's exit status. [process](../plan/process.md#rule-no-pipe-bazel)
- Shut down Bazel servers you started in temporary worktrees when you are done
  with them. [style 3](../style.md#rule-everything-through-bazel)
- **The orchestrator and the style reviewer decide style questions** from the
  guides; russ is asked rarely (russ, 2026-10-10).
  [process](../plan/process.md#rule-style-review-decides)
- **Run the style checks on your own diff before you report** (russ, 2026-10-10:
  "Make sure coders run clang-tidy before being shut down. Don't want to lose
  their context!"): `bazel test --config=fast //tools/...`.
  [process](../plan/process.md#rule-agent-runs-style-checks)
- **Review and report**: every step is reviewed before it merges; the agent's
  report lists the diff, the failing-first run, the tests and any deviation.
  [process](../plan/process.md#rule-step-review),
  [process](../plan/process.md#rule-agent-report),
  [process](../plan/process.md#rule-dispatch-prompt),
  [process](../plan/process.md#rule-merge-procedure)
- **Lanes**: steps run in the two long-lived lane clones, never in the main
  checkout, and never with the agent tool's worktree isolation.
  [process](../plan/process.md#rule-lane-clones),
  [CLAUDE.md](../../CLAUDE.md#rule-no-worktree-isolation),
  [CLAUDE.md](../../CLAUDE.md#rule-cd-into-lane)
- **Stop and report on any permission denial; do not work around it** (russ,
  2026-10-05). [process](../plan/process.md#rule-stop-on-denial)
- **Never push, open pull requests or send email**; russ does that.
  [process](../plan/process.md#rule-never-push). Confirm destructive actions
  first: [process](../plan/process.md#rule-confirm-destructive). Do not modify
  `~/Sources/linux`, `~/Sources/libfuse` or `~/Sources/fuse-generation-qemu`:
  [process](../plan/process.md#rule-dont-modify-linux)
- **Docker on this machine is production** (russ, 2026-10-06): never stop,
  restart, remove, prune or modify what you did not create.
  [CLAUDE.md](../../CLAUDE.md#rule-docker-is-production)
- **No background polling loops**; wait in the foreground (2026-10-07, about 90
  orphans found). [CLAUDE.md](../../CLAUDE.md#rule-no-polling-loops); never
  `pkill -f` a pattern in your own command line:
  [CLAUDE.md](../../CLAUDE.md#rule-no-pkill-self)
- **zsh with `noclobber`**: overwrite files with `>|`; `make` and `diff` are
  broken shell functions (use `command make`, `command diff`).
  [process](../plan/process.md#rule-zsh-noclobber)
- **Plan notes live in the repository**, not in session scratch space.
  [CLAUDE.md](../../CLAUDE.md#rule-repo-is-the-notebook)
- **Agents may set timers for themselves** (russ, 2026-10-17: "The timer ban
  only applies inside our codebase. Not to agents themselves.").
  [process](../plan/process.md#rule-agent-timers)
- **Questions for russ and rulings** (russ, 2026-10-17): the orchestrator rules
  on obvious cases and makes provisional rulings on the rest; when a ratified
  principle settles a question, rule, do not ask (russ, 2026-10-17: "Why isn't
  this obvious and therefore something you automatically answer?").
  [process](../plan/process.md#rule-questions-and-rulings),
  [process](../plan/process.md#rule-principle-settles-question)
- **The order of work is `README.md`'s Queue section** (russ, 2026-10-11).
  [process](../plan/process.md#rule-order-of-work)
- **Parallelism and budget**: at most two building agents plus two non-building
  ones; if a step needs more than about three agent runs, pause and tell russ.
  [process](../plan/process.md#rule-parallelism-and-budget),
  [process](../plan/process.md#rule-pause-after-three-runs); push points:
  [process](../plan/process.md#rule-push-points)
- **Red CI is not a problem until we ship** (russ, 2026-10-17); review is the
  bottleneck (russ, 2026-10-18).
  [process](../plan/process.md#rule-red-ci-until-ship),
  [process](../plan/process.md#rule-review-is-bottleneck)
- **Keep a list of failures and open questions for russ** in `log.md`, and keep
  going with what does not need him.
  [process](../plan/process.md#rule-needs-russ-list),
  [process](../plan/process.md#rule-keep-going-when-away)
- **The orchestrator oversees; it does not do the work** (russ, 2026-10-06): it
  decides, dispatches, reviews, merges and keeps the plan current.
  [CLAUDE.md](../../CLAUDE.md#rule-orchestrator-delegates),
  [process](../plan/process.md#rule-roles); commit to `main` at sensible points:
  [CLAUDE.md](../../CLAUDE.md#rule-commit-to-main)
- **The orchestrator's context is the expensive one** (russ, 2026-10-17):
  bookkeeping goes to a clerk in one batch, multi-command lookups to one
  subagent, status updates come from known state.
  [process](../plan/process.md#rule-orchestrator-context),
  [process](../plan/process.md#rule-clerk-batches),
  [process](../plan/process.md#rule-explore-for-lookups),
  [process](../plan/process.md#rule-status-from-known-state),
  [process](../plan/process.md#rule-chores-to-subagents)
- **File changes go through the Edit or Write tool, not a script** (russ,
  2026-10-09). [process](../plan/process.md#rule-edit-tool-not-scripts)
- **Propose formal-methods work unprompted** (russ, 2026-10-09).
  [process](../plan/process.md#rule-formal-methods-proposals)
- **Track review findings** (russ, 2026-10-17/18).
  [process](../plan/process.md#rule-track-review-findings)
- **Choose the effort on each dispatch** (russ, 2026-10-17).
  [process](../plan/process.md#rule-effort-per-dispatch)
- **The Co-Authored-By trailer names the model that wrote the commit** (russ,
  2026-10-18: "Whichever model is the author should be listed as co-author.").
  [process](../plan/process.md#rule-coauthor-trailer),
  [style 7](../style.md#rule-commit-body)
- **Borrowed files are copied in full**, not git-subtree'd (russ, 2026-09-27).
  [process](../plan/process.md#rule-borrowed-files-in-full)
- **Gemini and agy**: route each dispatch
  ([process](../plan/process.md#rule-route-before-dispatch)), invoke agy in the
  background ([process](../plan/process.md#rule-agy-invocation)), size jobs to
  the remaining pool
  ([process](../plan/process.md#rule-dont-start-unfinishable)), write jobs obey
  the same rules as a Claude subagent
  ([process](../plan/process.md#rule-agy-write-jobs)).
- **Quota**: read the pools live before any dispatch that spends quota (russ,
  2026-10-18); pace the Claude plan; read-only jobs go off the Claude plan.
  [process](../plan/process.md#rule-live-quota-readings),
  [process](../plan/process.md#rule-pace-claude-quota),
  [process](../plan/process.md#rule-readonly-jobs-off-claude)
- Design decisions and their reasons go in `docs/design.md`; user-facing
  behavior and limitations in `README.md`.
  [style 7](../style.md#rule-where-things-go)
- Everything in `docs/plan/` except the implementing agent's own "As built"
  notes in its step's phase file is the orchestrator's or its bookkeeping
  clerk's (status, log, other steps, the ledger). When a plan phase changes, the
  orchestrator updates its file in `docs/plan/phases/` and the status table in
  `docs/plan/README.md`, and appends to `docs/plan/log.md`; an agent reports
  what it needs. [style 7](../style.md#rule-where-things-go)
- Commit subjects start with the plan step they belong to, e.g. `3.2:
  third_party/linux: build the pinned kernel with Bazel`.
  [style 7](../style.md#rule-commit-subject)
- Never push, publish or send anything outside the repository unless the
  maintainer asks. [process](../plan/process.md#rule-never-push)
