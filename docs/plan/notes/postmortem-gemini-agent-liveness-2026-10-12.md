<!-- SPDX-License-Identifier: MIT -->
> Written by the temporary orchestrator (Gemini) on 2026-10-12 in /tmp; moved
> here 2026-10-16 under russ's "capture everything in the repository" rule.
> Its rules name that harness's tools (`manage_subagents`, `schedule`); the
> lesson that carries over: an unattended orchestrator must keep a worker
> running, and a reviewer waiting for messages is not a worker.

# Root Cause Analysis: Agent Idleness & Future Prevention

## 1. What Went Wrong (Root Cause)

1. **Conflating "Standing-By Reviewers" with "Active Workers":**
   - The Command Reviewer and Code Reviewer were spawned with prompt-and-wait loop designs. Once initialized, their state became `idle`.
   - The orchestrator treated their existence as "agents deployed", but because they were passive listeners awaiting messages, no forward progress on code implementation was being executed by them.

2. **Orchestrator Doing Work Synchronously Instead of Delegating:**
   - Instead of immediately spawning a dedicated worker subagent (`dcfs-implementer`) to execute the refactoring in `lane-1`, the orchestrator started inspecting files and drafting designs synchronously in its own context.
   - To the user monitoring active processes in the UI, zero subagents showed as `running`.

3. **Absence of a Liveness Watchdog / Heartbeat:**
   - There was no automated check enforcing that whenever unattended work is declared, at least one worker subagent is in state `running`.
   - If a subagent completed its prompt or went idle, the orchestrator did not have an automated watchdog alert to wake up and assign the next unit of work.

---

## 2. Concrete Fixes to Prevent Recurrence

### Rule 1: Mandatory Liveness Invariant
Whenever unattended work is ongoing:
- The orchestrator must run `manage_subagents(Action='list')`.
- If no worker subagent is in `running` state (or if all agents are `idle`), the orchestrator must either:
  1. Immediately launch the next task on an implementer subagent via `invoke_subagent`, OR
  2. Take over and actively execute the work in the current turn.
- A state where all agents are `idle` while a task is pending is treated as a defect.

### Rule 2: Active Watchdog Timers via `schedule`
- When launching background subagents for long-running steps, the orchestrator must schedule a watchdog timer:
  `schedule(DurationSeconds=300, Prompt="Watchdog: inspect subagent liveness and verify progress", TimerCondition="any")`
- If the worker subagent finishes early, the message satisfies the condition and wakes the orchestrator.
- If the worker stalls or dies silently, the watchdog timer fires, waking the orchestrator to inspect transcripts, kill hung agents, and respawn work.

### Rule 3: Clear Role Partitioning
- **Passive listeners** (Command Reviewer, Code Reviewer): Acknowledge that they will be `idle` by design until queried.
- **Active workers** (Implementers): Must always be actively `running` with clear, bounded goals. Every active task belongs to an explicit worker subagent in a specific lane.

### Rule 4: Visible Progress Dashboard in `/tmp/dcfs_status.md`
- Track agent conversation IDs, current role, state (`running` vs `idle`), and the specific lane/target they are executing.
