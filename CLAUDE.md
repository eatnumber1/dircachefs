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

Definitions are in `.claude/agents/`. A step that fails review twice moves
up one row (implementer to protocol, mechanical to implementer).

## Working on russ's machine

- Do the work in this repository: plan, notes and logs go under
  `docs/plan/`, not in session scratch space or personal memory.
- Lane checkouts for agents: `~/Sources/dircachefs-lanes/lane-{1,2}`
  (`docs/plan/process.md`). Do not use the agent tool's `isolation:
  "worktree"`: it resolved to the home-directory dotfiles repository on
  this machine and triggered ssh prompts.
- Every Bazel command in a lane or worktree, including `bazel help`,
  `info`, `query` and `cquery`, goes through `sg kvm -c '...'`: any of
  them starts that checkout's server, and a server started without the
  `kvm` group makes every later test fall back to slow emulation
  (2026-10-05: a stray `bazel help` cost a full-suite run).
- Never `pkill -f` / `pgrep -f` with a pattern that also appears in your
  own command line: it matches (and kills) your own shell. Prefer
  `pgrep -f '^bazel\(name\)'` anchored patterns or kill by PID.
- **Docker on this machine is production** (russ, 2026-10-06). Never stop,
  restart, remove, prune or modify containers, images, volumes or networks
  you did not create; never run `docker system prune`, `docker container
  prune`, `docker image prune`, `docker volume prune`, `docker network
  prune`, `docker rm/rmi` on anything not ours, or change the daemon's
  configuration. Containers we start (e.g. `act` jobs) carry a label
  (`--label dcfs.owner=act`) and `--rm`; clean up only by that label. Run
  Docker commands through `sg docker -c '...'` (the login session predates
  joining the `docker` group).
- The shell is zsh with `noclobber` (overwrite with `>|`); `make` and
  `diff` are broken shell functions on the command line (use `command
  make`, `command diff`).
- Commit to `main` at sensible points without asking (russ, 2026-10-05);
  subagents commit on their step branches. Never push.
