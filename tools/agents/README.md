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

Keeps the "lanes" table and "Code size" panel of the private claude.ai
dashboard current. Claude Code hooks call it with the hook's JSON on stdin;
it records which subagents run on which lane, computes code size (SLOC) over
the main checkout, writes `lanes.json` and `sloc.json`, and tells the
orchestrator when either file differs from the last uploaded copy. It is driven
by events only: nothing polls and nothing runs on a timer.

Subcommands (all read the hook JSON from stdin, all exit 0):

- `post-agent` (PostToolUse, matcher `Agent`): records a launched background
  agent (`status` `async_launched`) with its lane (the first
  `dircachefs-lanes/lane-N` in the prompt), model (`tool_input.model`, else
  `resolvedModel`, else the `model:` of `.claude/agents/<type>.md`) and effort
  (`tool_input.effort`, else that file's `effort:`); a foreground agent
  records nothing. Calls made by a subagent (the input has `agent_id`) are
  ignored. If the table changed since
  the last upload it prints `additionalContext` telling the orchestrator to
  upload the file.
- `post-message` (PostToolUse, matcher `SendMessage`): when `tool_input.to`
  names a finished agent, moves it back to running (the orchestrator resumed
  it), regenerates and prints the same `additionalContext` as `post-agent` if
  the table changed; any other target prints nothing. Calls made by a
  subagent are ignored.
- `subagent-stop` (SubagentStop): removes the agent (keeping its record in
  `finished.json`, the most recent 50, so `post-message` can resume it),
  regenerates, prints nothing.
- `stop` (the main session's Stop): regenerates lanes and, when main's HEAD
  differs from the last recorded HEAD, regenerates `sloc.json`; if either
  file changed since the last upload it blocks with a reason naming the files,
  unless `stop_hook_active` is true (no loops) or the caller is a subagent.
- `mark-uploaded`: records the current files' sha256 checksums as uploaded
  (`uploaded.sha256` and `sloc.uploaded.sha256`). The orchestrator runs it
  after uploading.
- `show`: prints the table.
- `ledger add [ID] field=value ...`: appends a row to the dispatch ledger
  for an unhooked job (an agy/Gemini job; model suffix sets effort, tokens
  blank, notes for quota delta).
- `ledger set ID field=value ...`: fills or updates outcome fields in the
  dispatch ledger at merge time.

The table has an orchestrator row and one row per `lane-N` under
`~/Sources/dircachefs-lanes/`. A lane with recorded agents shows the step
(the branch name minus a leading `step-`; the first agent's description on a
detached HEAD) and `running`. Its agent type, model and effort are each the
values of every agent on the lane joined by `, ` in launch order (e.g.
`dcfs-reviewer, dcfs-style-reviewer` / `opus, sonnet` / `high, medium`); when
an agent stops, the row shows the rest. An `agy` process whose working
directory is the lane adds `agy, sandboxed` to those lists, with its `--model`
(a trailing `-low|-medium|-high` moved to the effort column). A lane with an
`agy` process and no agent shows just that. Otherwise the lane is `idle`.

The code size panel counts non-blank, non-comment lines over `git ls-files`
of the main checkout (`$DCFS_MAIN_REPO` or `~/Sources/dircachefs`), outputting
`{"area", "language", "sloc"}` rows to `sloc.json`:
- Areas: `prod` (`dcfs/` except `dcfs/testonly/` and `*_test.*`), `test`
  (those exceptions plus `test/`), `model` (`formal/`), and `tools`
  (everything else).
- Languages by extension: `.cc`/`.h` (C++), `.c` (C), `.sh` and
  `test/qemu/guest/init` (shell), `.py` (Python), `.bzl` and
  `BUILD`/`BUILD.*`/`MODULE.bazel` (Starlark), `.tla` (TLA+), `.query`
  (clang-query); other files are skipped.
- Comments: `//` and `/* ... */` blocks for C, C++ and clang-query; `\*`
  and `(* ... *)` blocks for TLA+; `#` for the rest.
- Output order: areas `prod`, `test`, `model`, `tools`, each with languages
  ordered by size descending, followed by four `all` summary rows for the
  areas.

The dispatch ledger (`docs/plan/dispatch-ledger.tsv` in the main checkout)
records every agent job so its model and effort can later be judged
empirically against its outcome, deciding which effort and model suits each
task class. It is tab-separated with the header:

```text
id	dispatched	step	task_class	agent	model	effort	tokens	duration_s	rounds	blockers	shoulds	nits	escapes	verdict	notes
```

- Fields recorded at dispatch:
  - `id`: the agent ID once known, or a generated unique ID.
  - `dispatched`: ISO-8601 UTC timestamp of dispatch.
  - `step`: step name from the lane's branch or agent description.
  - `task_class`: task category derived from agent type (`dcfs-mechanical` ->
    `mechanical`, `dcfs-implementer` -> `implementation`, `dcfs-investigator` ->
    `investigation`, `dcfs-protocol` -> `protocol`, `dcfs-reviewer` ->
    `review`, `dcfs-style-reviewer` -> `style-review`), overridable later.
  - `agent`: agent type name (or `agy` for orchestrator-added jobs).
  - `model`: resolved model name.
  - `effort`: resolved effort level.
- Fields filled on completion:
  - `tokens`: total tokens consumed (from SubagentStop or completion hook input
    if provided; blank otherwise).
  - `duration_s`: run duration in seconds (from completion hook input if
    provided; blank otherwise).
- Outcome fields filled at merge time:
  - `rounds`: number of review rounds or revisions.
  - `blockers`: count of blocking review issues.
  - `shoulds`: count of non-blocking should-fix review issues.
  - `nits`: count of nit comments.
  - `escapes`: defects or missed requirements that escaped into the tree.
  - `verdict`: final outcome (`accepted`, `abandoned`, etc.).
  - `notes`: notes, including quota deltas for agy jobs or merge observations.

Outcome fields are filled at merge time using:

```bash
python3 -I tools/agents/dashboard_lanes.py ledger set ID field=value ...
```

For unhooked jobs (such as Gemini jobs through `agy`), a row is appended with:

```bash
python3 -I tools/agents/dashboard_lanes.py ledger add [ID] field=value ...
```

A failure to write the ledger never fails a hook (exits 0 with at most one
stderr line).

State is in `$DCFS_DASHBOARD_DIR` (default `~/.cache/dcfs-dashboard/`):
`running.json`, `finished.json`, `lanes.json`, `uploaded.sha256`, `sloc.json`,
`sloc.head`, `sloc.uploaded.sha256`, and an optional config.json
(`{"model": ..., "effort": ...}`) for the orchestrator row.
A malformed input or an unreadable state file prints one line to stderr and
exits 0, because a failing hook can wedge the session.

Hook configuration (in `.claude/settings.json`, installed by the
orchestrator):

```json
{"hooks": {
  "PostToolUse": [{"matcher": "Agent", "hooks": [{"type": "command", "command": "python3 -I \"$CLAUDE_PROJECT_DIR\"/tools/agents/dashboard_lanes.py post-agent", "timeout": 10}]},
    {"matcher": "SendMessage", "hooks": [{"type": "command", "command": "python3 -I \"$CLAUDE_PROJECT_DIR\"/tools/agents/dashboard_lanes.py post-message", "timeout": 10}]}],
  "SubagentStop": [{"hooks": [{"type": "command", "command": "python3 -I \"$CLAUDE_PROJECT_DIR\"/tools/agents/dashboard_lanes.py subagent-stop", "timeout": 10}]}],
  "Stop": [{"hooks": [{"type": "command", "command": "python3 -I \"$CLAUDE_PROJECT_DIR\"/tools/agents/dashboard_lanes.py stop", "timeout": 10}]}]
}}
```

It runs directly with `python3 -I`, no Bazel needed; the tests are
`//tools/agents:dashboard_lanes_test`.
