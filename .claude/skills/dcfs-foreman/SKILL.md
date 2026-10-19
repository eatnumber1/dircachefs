---
name: dcfs-foreman
description: >-
  Hand a bounded, menial chunk of work (kernel or source surveys, greps
  across trees, doc passes, test boilerplate, warning-fix batches) to Gemini
  through tools/agents/foreman.py instead of doing it yourself or calling
  agy directly. Use when you are an orchestrator or a subagent and the work
  needs labour rather than judgement. Claude subagents cannot spawn Claude
  subagents, so this is how they delegate down.
---

# Delegate to Gemini with foreman

The rule is `rule-delegate-down` in
[process](/docs/plan/process.md#rule-delegate-down); the invocation
rules are
[agy-invocation](/docs/plan/process.md#rule-agy-invocation),
[dont-start-unfinishable](/docs/plan/process.md#rule-dont-start-unfinishable)
and [agy-write-jobs](/docs/plan/process.md#rule-agy-write-jobs). Flags and
output files are in the foreman section of
[tools/agents/README.md](/tools/agents/README.md). Never call `agy -p` for
a job; `agy -p '/quota'` is the one direct call.

**Use it for**: surveys of the kernel or other trees, greps across them,
doc accuracy passes, test boilerplate to a pattern, warning-fix batches.
**Do not use it for**: the write-through protocol, the model, identity,
crash handling, reviews (Flash is lenient), or design that needs judgement
([process](/docs/plan/process.md#rule-route-before-dispatch)).

1. **Quota first.** `agy -p '/quota'`. Size the job to the pool left;
   with little left, choose a small job that is safe to abandon.
2. **Lane and branch.** Pick a free, clean lane (not yours, not one with a
   job running) and a new branch name, such as `delegate-STEP-WHAT`.
3. **Prompt file.** Self-contained: the task, the paths to read, the
   rules that matter (never push, Docker is production, no timers or retry
   loops, test first), and whether it is read-only. The worker's final
   answer is the report, so say what it must contain.
4. **Run it with the Bash tool in the background**
   (`run_in_background: true`), never in the foreground and never behind a
   polling loop; you are told when it exits:
   `cd YOUR_LANE && python3 -I tools/agents/foreman.py STEP LANE BRANCH
   PROMPT_FILE --out DIR`.
5. **Read the result.** Under `--out`: `worker.out` (the report),
   `review*.out`, `summary.md`, `usage.before` and `usage.after`. Verify
   its claims before you use them: grep or read what it cites.
6. **Check the lane** with `git status`: a non-JSON `AGY_ERROR` or 429
   makes foreman hang and can leave the lane mid-rebase (README, known
   issue); abort any rebase.
7. **Read-only jobs cost extra today**: an empty-diff review round and a
   throwaway branch, until 25.48 adds `--read-only`. Ignore the branch.
8. **Record it.** `python3 -I tools/agents/dashboard_lanes.py ledger add
   step=PARENT_STEP agent=agy model=MODEL notes=QUOTA_DELTA` from the main
   checkout, naming the parent step.
9. **Report it.** Your step report gets a line
   `Delegated: WHAT, to Gemini, job output DIR, verified by HOW` (or
   `Delegated: none`).
