@AGENTS.md

# Claude Code notes

Claude-specific additions to `AGENTS.md` (which applies to every agent and
contributor).

## Orchestration

Plan execution follows `docs/plan/process.md` (one step's life cycle) and
`docs/plan/execution.md` (waves, lanes, sync points). The orchestrator
session dispatches each step to a subagent of one of the types below and
does not pass a `model` override, so the type's model and effort apply.

| Agent type | Model | Effort | Use for |
|---|---|---|---|
| `dcfs-mechanical` | Haiku | low | precise, pattern-following batches (warning and clang-tidy fixes, simple tests) |
| `dcfs-implementer` | Sonnet | medium | the default for plan steps |
| `dcfs-investigator` | Sonnet | high | measurement and investigation (emulation speed, hermetic kernel/QEMU/busybox builds, puzzling failures) |
| `dcfs-protocol` | Opus | high | the write-through protocol, dirty set and recovery, identity, crash and failure handling, mount namespaces, the TLA+ model |
| `dcfs-reviewer` | Opus | xhigh | read-only review of hard steps and of the plan |
| `dcfs-style-reviewer` | Sonnet (Opus for judgement-heavy reshaping steps) | high | read-only style review of every step's diff before merge, against `docs/style.md` incl. 1.9's comment rules (russ, 2026-10-12/16) |

<a id="rule-orchestrator-delegates"></a>
**The orchestrator oversees; it does not do the work** (russ, 2026-10-06).
It decides what to do, dispatches subagents, reviews their work, merges,
keeps the plan current and assists russ. Hands-on work (investigation,
fixes, long test runs, doc rewrites) goes to a subagent, even when small.
When the orchestrator runs on Fable, Fable may be used for a subagent
for the hardest tasks, where judgement rather than labour is the
bottleneck (a review of crash safety across several paths, a refinement
mapping's design): pass `model: fable` explicitly and say why in
`docs/plan/log.md`. russ (2026-10-09): "You can use it a little more if
you think it will be helpful (but you don't have to!). Also remember to
leverage different effort levels too." Choose the effort per task: low
for mechanical batches, medium by default, high or xhigh for protocol
work and reviews. This conflicts with `docs/plan/process.md`
rule 6, "Vary the effort knob per job" (russ, 2026-10-17), whose table says
"Protocol, crash safety, a puzzling failure | not Gemini | Opus high or
xhigh" and "Review needing judgement | Argon medium | Sonnet high, Opus
high", and "Start a job one notch lower than seems needed and raise it only
when the answer comes back shallow": here reviews are "high or xhigh", there
a judgement review is "Opus high" (conflict raised with russ 2026-10-18).

Definitions are in `.claude/agents/`. A step that fails review twice moves
up one row (implementer to protocol, mechanical to implementer).

## Working on russ's machine

- <a id="rule-repo-is-the-notebook"></a>
  Do the work in this repository: plan, notes and logs go under
  `docs/plan/`, not in session scratch space or personal memory.
- <a id="rule-no-worktree-isolation"></a>
  Lane checkouts for agents: `~/Sources/dircachefs-lanes/lane-{1,2}`
  (`docs/plan/process.md`). Do not use the agent tool's `isolation:
  "worktree"`: it resolved to the home-directory dotfiles repository on
  this machine and triggered ssh prompts.
- Run Bazel with access to `/dev/kvm` and `bazel shutdown` first when
  switching: see the process index (`docs/rules/index-process.md`, "Running
  Bazel").
- <a id="rule-no-polling-loops"></a>
  Never leave a background polling loop (`while pgrep -f ...; do sleep`,
  `until grep -q MARKER log; do sleep`): the pattern matches the loop's
  own command line, or the marker never arrives once the run it waits for
  dies. About 90 such orphans were found on 2026-10-07. Wait in the
  foreground with the tool's timeout and rerun instead.
- <a id="rule-no-pkill-self"></a>
  Never `pkill -f` / `pgrep -f` with a pattern that also appears in your
  own command line: it matches (and kills) your own shell. Prefer
  `pgrep -f '^bazel\(name\)'` anchored patterns or kill by PID.
- <a id="rule-cd-into-lane"></a>
  Agents' shells start in the main checkout. An agent must `cd` into its
  lane at the start of every command; a forgotten `cd` once left a
  downloaded fakeroot source tree in the main checkout (2026-10-05). A Bazel
  command run from the main checkout starts a server there and builds the
  wrong tree (2026-10-06).
- <a id="rule-docker-is-production"></a>
  **Docker on this machine is production** (russ, 2026-10-06). Never stop,
  restart, remove, prune or modify containers, images, volumes or networks
  you did not create; never run `docker system prune`, `docker container
  prune`, `docker image prune`, `docker volume prune`, `docker network
  prune`, `docker rm/rmi` on anything not ours, or change the daemon's
  configuration. Containers we start (e.g. `act` jobs) carry a label
  (`--label dcfs.owner=act`) and `--rm`; clean up only by that label. Run
  Docker commands through `sg docker -c '...'` (the login session predates
  joining the `docker` group).
- The shell is zsh with `noclobber` (`docs/plan/process.md`, "zsh with
  `noclobber`").
- <a id="rule-commit-to-main"></a>
  Commit to `main` at sensible points without asking (russ, 2026-10-05);
  subagents commit on their step branches. Never push.
- Never lower the coverage baseline (`AGENTS.md`, "Always").
