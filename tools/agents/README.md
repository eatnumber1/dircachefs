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
orchestrator when either file differs from the last uploaded copy. Everything
is event-driven except the hourly meter reading, an approved timer exception:
nothing polls.

Subcommands (all read the hook JSON from stdin, all exit 0):

- `post-agent` (PostToolUse, matcher `Agent`): records a launched background
  agent (`status` `async_launched`) with its lane (the first
  `dircachefs-lanes/lane-N` in the prompt), model (`tool_input.model`, else
  `resolvedModel`, else the `model:` of `.claude/agents/<type>.md`) and effort
  (`tool_input.effort`, else that file's `effort:`); a foreground agent
  records nothing. Calls made by a subagent (the input has `agent_id`) are
  ignored. If a dataset has news
  since the last upload it prints `additionalContext` telling the
  orchestrator to upload the files, followed by the clerk ask when the queue
  holds 5 items.
- `post-message` (PostToolUse, matcher `SendMessage`): when `tool_input.to`
  names a finished agent, moves it back to running (the orchestrator resumed
  it), regenerates and prints the same `additionalContext` as `post-agent` if
  the table changed; any other target prints nothing. Calls made by a
  subagent are ignored.
- `subagent-stop` (SubagentStop): removes the agent (keeping its record in
  `finished.json`, the most recent 50, so `post-message` can resume it),
  regenerates, prints nothing.
- `stop` (the main session's Stop): regenerates lanes and, when main's HEAD
  differs from the last recorded HEAD, regenerates `sloc.json`; launches the
  detached `quota` reader at most once per hour; launches the detached `ci`
  reader once when `origin/main` differs from `ci.read_sha` or `ci.json` has a
  run that is `in_progress` or `queued` (recording `origin/main` in
  `ci.read_sha`). It prints nothing for dataset news (the PostToolUse
  reminder tells the orchestrator on its next tool call; a Stop block forces
  an extra turn) and blocks only for a clerk ask (below), unless
  `stop_hook_active` is true (no loops) or the caller is a subagent.
  `post-agent` treats a `dcfs-mechanical` agent whose prompt names the
  `dcfs-dashboard-upload` skill as laneless, even if the prompt names a lane
  path, so an upload does not mark
  `lanes.json` changed. When each dataset asks:
  - `lanes`: on material changes compared with the last uploaded table (a
    lane's step changed, an agent was newly dispatched, or an override was set
    or cleared; a lane going to idle by SubagentStop alone asks nothing).
  - `quota` and `budget`: whenever a new meter reading changes them.
  - `ci`: when a run was added or dropped, or a run's `result` differs from
    `ci.uploaded.json`. A reread with the same runs asks nothing.
  - `sloc`: never on its own. When another dataset asks and `sloc` has also
    changed, `sloc` is named in the same ask.
- `mark-uploaded DATASET...`: records the named datasets (`lanes`, `sloc`,
  `quota`, `budget`, `ci`) as uploaded at their current contents
  (`uploaded.sha256`, `lanes.uploaded.json` and `lane_states.uploaded.json`
  for lanes, `sloc.uploaded.sha256`, `quota.uploaded.sha256`,
  `budget.uploaded.sha256`, `ci.uploaded.sha256` and `ci.uploaded.json`
  for ci).
  Datasets not named stay changed. With no names it is a usage error. The
  orchestrator runs it with the names the Stop hook's reason gives, after
  uploading those datasets.
- `show`: prints the table.
- `quota`: runs the usage meters (claude_usage.py and agy /usage), writes
  `quota.json`, appends Claude week to `budget.json`, and refreshes `ci.json`.
- `ci`: runs `gh run list --limit 12` in the main checkout and writes
  `ci.json`: a list of `{"run", "workflow", "commit", "result", "trigger",
  "started"}` (`commit` the first 8 hex digits of `headSha`; `result` the
  conclusion, or the status while a run is unfinished). A failing `gh` prints
  one line on stderr naming the command and keeps the previous `ci.json`.
- `lane-state LANE STATE [NOTE]` / `lane-state LANE clear`: sets or clears a
  manual lane state override (e.g. `lane-state lane-2 paused "quota"` or
  `lane-state lane-1 waiting "review Sunday"`), stored in `lane_states.json`.
  The row shows the override state and note (in the agent column) instead of
  idle or running, until cleared or a new agent is dispatched to that lane.
- `ledger add [ID] field=value ...`: appends a row to the dispatch ledger
  for an unhooked job (an agy/Gemini job; model suffix sets effort, tokens
  blank, notes for quota delta).
- `ledger set ID field=value ...`: fills or updates outcome fields in the
  dispatch ledger at merge time.
- `clerk-queue add TEXT...`: appends TEXT (joined by spaces, a newline counts
  as a space) as one numbered item of the bookkeeping clerk queue,
  `clerk_queue.md`. It prints nothing; empty text is a usage error.
- `clerk-queue drain`: prints the clerk brief for the queue (the header
  `Bookkeeping clerk batch (orchestrator's queue):`, one numbered line per
  item, then the closing line that tells the clerk what to edit, how to commit
  on main, and what to report), removes the queue, and writes
  `clerk_queue.dispatched`. An empty queue prints nothing and changes nothing.
  The orchestrator pastes the brief into one `dcfs-mechanical` dispatch
  (effort low).

The clerk rule: `stop` asks for a clerk when the queue is non-empty and either
no agent is running or the queue holds 5 items (`CLERK_BATCH`), and the ask
names the count and the drain command. It does not ask while
`clerk_queue.dispatched` exists; a `dcfs-mechanical` agent stopping (the
SubagentStop hook) removes that marker. `stop` blocks with the ask (the
`stop_hook_active` guard stops a second block); `post-agent` and
`post-message` add it to their `additionalContext` at 5 items only.

The table has an orchestrator row and one row per `lane-N` under
`~/Sources/dircachefs-lanes/`. A lane with recorded agents shows the step
(the branch name minus a leading `step-`; the first agent's description on a
detached HEAD) and `running`. Its agent type, model and effort are each the
values of every agent on the lane joined by `, ` in launch order (e.g.
`dcfs-reviewer, dcfs-style-reviewer` / `opus, sonnet` / `high, medium`); when
an agent stops, the row shows the rest. An `agy` process whose working
directory is the lane adds `agy` (plus `, read-only` with `--mode plan`,
plus `, sandboxed` only with `--sandbox`) to those lists, with its `--model`
(a trailing `-low|-medium|-high` moved to the effort column). A lane with an
`agy` process and no agent shows just that. An override (`lane-state`) displays
its state and note instead of idle or running. Otherwise the lane is `idle`.

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
id	dispatched	step	task_class	agent	model	effort	tokens	duration_s	rounds	blockers	shoulds	nits	escapes	verdict	notes	input_tokens	output_tokens	cache_read_tokens	cache_creation_tokens
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
- Fields filled on completion (SubagentStop; the PostToolUse hook of a
  background launch fires at dispatch, before the agent has spent anything):
  - `input_tokens`, `output_tokens`, `cache_read_tokens`,
    `cache_creation_tokens` (the last four columns): summed from the
    `message.usage` blocks of the agent's own transcript, whose path the hook
    input gives as `agent_transcript_path`. Streaming writes one transcript
    line per content block with the message's usage repeated, so a message
    counts once, by its last block.
  - `tokens`: the total of those four; blank when the transcript is missing,
    has a malformed line or has no usage block (the reason goes to stderr and
    the hook still succeeds). A `tokens` or `total_tokens` in the hook input,
    if one ever appears, wins.
  - `duration_s`: the run duration, from `duration_s` in the hook input if
    present, else the transcript's last minus first `timestamp`.
  - `dashboard_lanes.py ledger backfill-tokens` fills the same fields for rows
    whose `tokens` is empty, finding `agent-<id>.jsonl` under
    `~/.claude/projects/*/*/subagents/`, and prints how many rows it filled.
  - `dashboard_lanes.py ledger restore-missing [--since ISO]` rebuilds rows
    lost from the ledger (for example by a `git reset --hard` over
    uncommitted hook rows): every `agent-<id>.meta.json` under the same
    directories with no row gets one, from the meta's `agentType` and
    `description` and the transcript's first timestamp and model, token
    columns and duration; `--since` skips agents dispatched earlier. Rows
    are inserted in dispatch-time order, existing rows are untouched, and a
    second run adds nothing.
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

The `quota` command runs the external usage meters for Claude Code and `agy`
(`claude_usage.py` in the main checkout with a 90 s timeout and
`agy -p /usage` with a 120 s timeout), records pool limits and reset
timestamps in `quota.json`, and appends the Claude week usage entry to
`budget.json`. The `stop` hook launches the detached `quota` reader in the
background at most once per hour when checking for dashboard changes. This is
an approved timer exception (approved by russ, 2026-10-17: credits are spent out
of band, so the meters can only be known by reading them). If a meter fails or
times out, previous rows are kept and the failure is reported to stderr.

State is in `$DCFS_DASHBOARD_DIR` (default `~/.cache/dcfs-dashboard/`):
`running.json`, `finished.json`, `lanes.json`, `uploaded.sha256`,
`lanes.uploaded.json`, `lane_states.json`, `lane_states.uploaded.json`,
`sloc.json`, `sloc.head`, `sloc.uploaded.sha256`, `quota.json`,
`quota.uploaded.sha256`, `quota.read_at`, `quota.log`, `budget.json`,
`budget.uploaded.sha256`, `ci.json`, `ci.uploaded.sha256`, `ci.read_sha`,
`ci.log`, `clerk_queue.md`, `clerk_queue.dispatched`, and an optional
config.json (`{"model": ..., "effort": ...}`) for the orchestrator row.
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

## foreman

Drives a queued development step end to end on Gemini in a lane, running worker
and review/fix rounds, and leaving a merge-ready branch plus `summary.md`
(orchestrator mode, the default). `--single` runs one worker session only, for a
bounded chunk; `--read-only` with it runs a survey in plan mode and saves a
report (below). Both hold the session open past agy's 30-minute print timeout.

How to use it as an agent (when, quota, prompt, ledger, report) is the
`dcfs-foreman` skill, `.claude/skills/dcfs-foreman/SKILL.md`.

Usage:

```bash
python3 -I tools/agents/foreman.py STEP LANE BRANCH PROMPT_FILE [--model M] [--review-model M] [--out DIR]
```

Or via Bazel:

```bash
bazel run //tools/agents:foreman -- STEP LANE BRANCH PROMPT_FILE [--model M] [--review-model M] [--out DIR]
```

Single mode and read-only mode:

```bash
python3 -I tools/agents/foreman.py STEP LANE BRANCH PROMPT_FILE --single \
    [--model M] [--out DIR]
python3 -I tools/agents/foreman.py STEP DIR PROMPT_FILE --single --read-only \
    [--model M] [--out DIR]
```

- `STEP` (required): name of the step (e.g. `25.33`).
- `LANE` (required): lane repository directory.
- `BRANCH` (required): branch name to check out from `FETCH_HEAD` of main.
- `PROMPT_FILE` (required): path to the worker's initial prompt file.
- `--model M`: model for worker and fix sessions (default:
  `gemini-3.8-flash-medium`).
- `--review-model M`: model for review sessions (default: same as `--model`).
- `--single`: one worker session, no review or fix rounds. The branch is set up
  as in orchestrator mode. `tokens.json` records the session with role
  `single`; the ledger row says `mode=single` and `rounds=0`; `summary.md` has
  no "last review" section. With `--single`, `--review-model` is refused.
- `--read-only` (requires `--single`; refused without it): agy plan mode, and
  no git at all: nothing is fetched or checked out and `DIR` (the second
  argument) is only the working directory. There is no `BRANCH` argument, so
  the form is `STEP DIR PROMPT_FILE`. The worker's text, minus its
  `JOB-COMPLETE` line, is saved as `report.md` in `--out`. `summary.md` says
  "read-only run in DIR" and has no commit sections; the ledger row says
  `mode=read-only`.
- `--out DIR`: output directory where `.jsonl`, `.out`, usage readings, and
  `summary.md` are saved (default: scratchpad foreman directory or
  `/tmp/foreman/<step>`).

### Execution and protocol

1. Checks out a fresh branch in `LANE` tracking `FETCH_HEAD` of the main
   repository (`$DCFS_MAIN_REPO` or `~/Sources/dircachefs`), and records `base`.
2. Measures quota before the run via `agy -p /usage` filtering for Gemini
   models.
3. Runs the worker session in `stream-json` mode:
   - Starts `agy` with `--input-format stream-json --output-format stream-json`
     and `-p=`.
   - Sends the worker prompt with an instruction appended to print
     `JOB-COMPLETE` when all work, including background commands, has finished
     and reported.
   - Keeps stdin open, reads stdout NDJSON events line by line, writes them to
     `worker.jsonl`, and streams agent response text to `worker.out`.
   - Closes stdin when an `agent_response` contains `JOB-COMPLETE`, then waits
     for `agy` to emit the final `result` event and exit.
   - If `agy` exits early (crash, quota, or an `ERROR` result), records the
     failure in `summary.md` and terminates the job immediately.
4. Up to two review/fix rounds:
   - Read-only review: runs `agy -p ... --mode plan --output-format json`
     inspecting `git diff base HEAD` and uncommitted files against style and
     agent rules. The review text goes to `review<round>.out` and agy's raw
     JSON object (with `usage`) to `review<round>.json`.
   - If review outputs `NONE`, the step is clean and the loop breaks.
   - Otherwise, runs a fix round in `stream-json` mode with the review
     findings, saving events to `fix<round>.jsonl` and text to `fix<round>.out`.
5. Measures quota after the run, and writes `summary.md` reporting the branch,
   base commit, commits made, uncommitted changes, last review, quota delta
   and a token table (role, model, the five counts).
6. Writes `tokens.json` and adds one dispatch-ledger row (below).

### Token counts

Every agy session (worker, each fix, each review) is recorded as one entry of
`tokens.json` in `--out`: `role` (`worker`, `fix`, `review`, `single`), `name`
(`worker`, `fix1`, `review1`), `model`, `usage` (`input_tokens`,
`output_tokens`, `thinking_tokens`, `cache_read_tokens`, `total_tokens`),
`subagent_calls` and `duration_s`; plus `total`. `thinking_tokens` is part of
`output_tokens`, not added to it. Stream sessions take the
counts from the `result` event's `usage` (the sum of the `step_update` usage
fields if agy died before a result), review sessions from the JSON object's
`usage`. The model is `init.model` for stream sessions and `--review-model`
for reviews (the JSON object has no model). A failed run records the sessions
that ran.

Subagents inside a Gemini session are not attributed separately. Found in the
12 worker and fix recordings of 2026-10-17/18: `init.tools` lists
`invoke_subagent`, `define_subagent`, `browser_subagent` and
`manage_subagents`, but no `step_update` has a `tool_name` of
`invoke_subagent` or `browser_subagent`, every event carries the one
`conversation_id`, and `result.usage` equals the sum of the `step_update.usage`
fields exactly. So `result.usage` is the session total and would include any
subagent steps that appeared as ordinary steps; `subagent_calls` counts such
tool calls so a session that used subagents is visible. If a subagent ever
shows as events with another `conversation_id`, attribute it then.

The ledger row (`dashboard_lanes.py ledger add`, run by foreman against
`$DCFS_MAIN_REPO`) has `agent=agy`, the worker model, `tokens` the
`total_tokens + cache_read_tokens` sum (as the Claude rows count their
tokens), `duration_s` the summed session durations, the `input_tokens`,
`output_tokens` and `cache_read_tokens` sums, `rounds` the review rounds run,
and in `notes` `foreman mode=orchestrator;` (`single`, `read-only`) then each
session as `name=total_tokens` (`; failed` when the run failed). Thinking
tokens have no ledger column: they are in `tokens.json`.

Known issue (2026-10-18): foreman hangs on a non-JSON `AGY_ERROR` line, such as
agy's 429 `RESOURCE_EXHAUSTED` ("undecodable agy event"), and can leave the lane
mid-rebase. Until it is fixed, check the lane (`git status`) after a foreman
failure and abort any rebase.

The tests are `//tools/agents:foreman_test`.

## review_findings

Maintains the review findings ledger (`docs/plan/review-findings.tsv`) in the
main repository checkout and provides summary counts across review sessions.

Usage:

```bash
# Add a findings block from stdin
python3 -I tools/agents/review_findings.py add --step S --round R --reviewer X --date D < block

# Print summary counts by category, rule, severity, and mechanizable
python3 -I tools/agents/review_findings.py summary
```

Or via Bazel:

```bash
bazel run //tools/agents:review_findings -- add --step S --round R --reviewer X --date D < block
bazel run //tools/agents:review_findings -- summary
```

The findings block format has one finding per line, separated by ` | `:
```text
severity | category | rule | file | summary | mechanizable | outcome
```

- `severity`: `blocker`, `should`, `nit`, `none` ('none' records a review
  that found nothing).
- `category`: `correctness`, `test-gap`, `model-gap`, `dead-code`,
  `comment-accuracy`, `contract-claim`, `status-handling`, `naming`,
  `test-shape`, `commit-hygiene`, `docs`, `style-other`.
- `rule`: `docs/style.md` section or `AGENTS.md` rule cited, or empty.
- `file`: `path:line` or empty.
- `summary`: one line, no tabs.
- `mechanizable`: `checked`, `could`, `no`.
- `outcome`: `fixed <commit>`, `declined`, `question`, `open`.

The tests are `//tools/agents:review_findings_test`.

