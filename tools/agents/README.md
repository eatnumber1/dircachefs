<!-- SPDX-License-Identifier: MIT -->
# Agent tools

Harness tools for the orchestrators that drive dcfs development. They are not
part of dcfs: nothing in the dcfs build or tests depends on them.

## gemini_watchdog

A liveness monitor for subagents of the Antigravity/Gemini harness. It reads
the harness's transcript directory (default `~/.gemini/antigravity-cli/brain`),
detects subagents that are running, idle, stalled or failed, and writes a
Markdown status dashboard. It is not used with Claude Code: the Claude harness
notifies the orchestrator when an agent finishes, so there is nothing to poll.

Run it with Bazel:

```
bazel run //tools/agents:gemini_watchdog -- --parent-id ID --active-ids A,B
```

- `--parent-id ID` (required): conversation ID of the orchestrator.
- `--active-ids A,B`: comma-separated IDs of the subagents to monitor. When
  omitted, subagents are discovered from the parent transcript.
- `--status-file PATH`: where the dashboard is written. Default
  `$XDG_RUNTIME_DIR/dcfs_agent_status.md`, or the tmp dir if `XDG_RUNTIME_DIR`
  is unset.
- `--interval SEC`: polling interval in daemon mode (default 5).
- `--stall-threshold SEC`: seconds without transcript progress before a
  subagent is reported stalled (default 600).
- `--once`: write one dashboard and exit.
- `--brain-dir PATH`: the harness transcript directory.

The monitor runs only while an orchestrator of that harness is working
unattended. Stop it when that orchestrator stops; a leftover daemon keeps
writing a stale dashboard.

It polls on a timer, as `docs/style.md` 1.11 allows when nothing can signal:
a stall is the absence of activity in the harness's transcript files, which
no event reports. The `--interval` bounds how stale the status file may be.

## claude_usage

Prints the usage panel of Claude Code's `/usage` command as plain text. It
starts an interactive `claude` in a pseudo-terminal, sends `/usage`, keeps the
panel and exits.

Run it from the repository root, with a logged-in `claude` on the `PATH`:

```
timeout 90 python3 -I tools/agents/claude_usage.py
```

`--cwd DIR` picks the checkout claude runs in (default: the current
directory). That checkout must already be one claude trusts, so that no trust
dialog appears.

- It prints the panel's lines, one per line, with runs of spaces collapsed.
- The figures are approximate: they count this machine's local sessions only,
  not the account's usage elsewhere.
- It exits 1 with `usage panel not found` when the panel does not appear within
  its own 60 s deadline. It kills `claude` before exiting; `timeout 90` is the
  outer bound.
- The screen cleaning and the panel extraction are tested
  (`//tools/agents:claude_usage_test`, on a synthetic capture). The pty driver
  is not: it needs a logged-in CLI. The `py_binary` target
  `//tools/agents:claude_usage` runs the tool through Bazel too.

Its waits are bounded deadlines, as `docs/style.md` 1.11 allows: the pty has no
event for "the panel has drawn" or "the prompt is up", so it polls until a
deadline.

## dashboard_lanes

Keeps the "lanes" table of the private claude.ai dashboard current. Claude
Code hooks call it with the hook's JSON on stdin; it records which subagents
run on which lane, writes the table to `lanes.json`, and tells the
orchestrator when that file differs from the last uploaded copy. It is driven
by events only: nothing polls and nothing runs on a timer.

Subcommands (all read the hook JSON from stdin, all exit 0):

- `post-agent` (PostToolUse, matcher `Agent`): records a launched background
  agent (`status` `async_launched`) with its lane (the first
  `dircachefs-lanes/lane-N` in the prompt), model (`tool_input.model`, else
  `resolvedModel`, else the `model:` of `.claude/agents/<type>.md`) and effort
  (that file's `effort:`); a foreground agent records nothing. Calls made by a
  subagent (the input has `agent_id`) are ignored. If the table changed since
  the last upload it prints `additionalContext` telling the orchestrator to
  upload the file.
- `subagent-stop` (SubagentStop): removes the agent, regenerates, prints
  nothing.
- `stop` (the main session's Stop): regenerates; if the table changed since
  the last upload it blocks with a reason naming the file, unless
  `stop_hook_active` is true (no loops) or the caller is a subagent.
- `mark-uploaded`: records the current file's sha256 as uploaded. The
  orchestrator runs it after uploading.
- `show`: prints the table.

The table has an orchestrator row and one row per `lane-N` under
`~/Sources/dircachefs-lanes/`. A lane with a recorded agent shows the step
(from its branch `step-X`, else the agent's description), agent type, model
and effort as `running`; a lane with no agent but an `agy` process whose
working directory is the lane shows `agy, sandboxed` and its `--model`;
otherwise it is `idle`.

State is in `$DCFS_DASHBOARD_DIR` (default `~/.cache/dcfs-dashboard/`):
`running.json`, `lanes.json`, `uploaded.sha256`, and an optional
`config.json` (`{"model": ..., "effort": ...}`) for the orchestrator row.
A malformed input or an unreadable state file prints one line to stderr and
exits 0, because a failing hook can wedge the session.

Hook configuration (in `.claude/settings.json`, installed by the
orchestrator):

```json
{"hooks": {
  "PostToolUse": [{"matcher": "Agent", "hooks": [{"type": "command", "command": "python3 -I $CLAUDE_PROJECT_DIR/tools/agents/dashboard_lanes.py post-agent", "timeout": 10}]}],
  "SubagentStop": [{"hooks": [{"type": "command", "command": "python3 -I $CLAUDE_PROJECT_DIR/tools/agents/dashboard_lanes.py subagent-stop", "timeout": 10}]}],
  "Stop": [{"hooks": [{"type": "command", "command": "python3 -I $CLAUDE_PROJECT_DIR/tools/agents/dashboard_lanes.py stop", "timeout": 10}]}]
}}
```

It runs directly with `python3 -I`, no Bazel needed; the tests are
`//tools/agents:dashboard_lanes_test`.
