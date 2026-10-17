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
