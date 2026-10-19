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

Foreman has two modes. **Orchestrator mode** (today's default) runs a
worker plus review/fix rounds and leaves a merge-ready branch: a whole step
on Gemini. **Single mode** (`--single`: one worker session, no review
round; add `--read-only` for plan mode, no git and a saved `report.md`) is
for bounded chunks.
Foreman holds the session past agy's 30-minute print timeout, which is why
every agy job goes through it.

What may be delegated, who may delegate and why are in
[process](/docs/plan/process.md#rule-delegate-down); routing is
[rule 9](/docs/plan/process.md#rule-route-before-dispatch). The invocation
rules are [agy-invocation](/docs/plan/process.md#rule-agy-invocation),
[dont-start-unfinishable](/docs/plan/process.md#rule-dont-start-unfinishable)
and [agy-write-jobs](/docs/plan/process.md#rule-agy-write-jobs). Flags and
output files are in the foreman section of
[tools/agents/README.md](/tools/agents/README.md). A subagent delegates
only into the delegation lane its brief names; with none named, do not
delegate and say so in your report.

1. **Quota first.** `agy -p '/usage'` in the background. Size the job to
   the pool left; with little left, choose a small job that is safe to
   abandon.
2. **Lane and branch.** Use the lane your brief names and a new branch
   name, such as `delegate-STEP-WHAT`. A `--read-only` survey needs no
   branch and changes nothing in the lane.
3. **Prompt file.** Self-contained, per mode: see
   [prompts.md](prompts.md) for the checklist (orchestrator mode) and the
   template (single mode).
4. **Run it with the Bash tool in the background**
   (`run_in_background: true`), never in the foreground and never behind a
   polling loop; you are told when it exits:

   ```bash
   cd YOUR_LANE && python3 -I tools/agents/foreman.py \
       STEP LANE BRANCH PROMPT_FILE --single --out OUT_DIR
   cd YOUR_LANE && python3 -I tools/agents/foreman.py \
       STEP WORKDIR PROMPT_FILE --single --read-only --out OUT_DIR
   ```

   The first is a write chunk; the second a survey (no branch; `WORKDIR` is
   only the working directory). `OUT_DIR` is where the results go: put it
   under your scratchpad. Omit `--single` for a whole step with review
   rounds.
5. **Read the result.** Under `--out`: `worker.out` (the worker's text); in
   read-only mode also `report.md` (that text minus the `JOB-COMPLETE`
   line); `review*.out`, `summary.md` (with a token table), `tokens.json`
   (per-session Gemini token counts), `usage.before` and `usage.after`.
   Foreman adds the run's dispatch-ledger row itself. Verify its claims
   before you use them: grep or read what it cites.
6. **Check the lane** with `git status`: a non-JSON `AGY_ERROR` or 429
   makes foreman hang and can leave the lane mid-rebase (README, known
   issue); abort any rebase.
7. **Bounded chunks use `--single`**: no review round. A write chunk still
   gets a throwaway branch; ignore it.
8. **The ledger row is foreman's.** It adds one row for the run (step as
   you passed it, `agent=agy`, tokens, per-session breakdown in `notes`);
   pass the parent step as `STEP` so it is attributed. Add a second row
   by hand only to record something foreman cannot know, such as the quota
   delta (`ledger add` from the main checkout, `notes=QUOTA_DELTA`).
9. **Report it.** Your step report gets a line
   `Delegated: WHAT, to Gemini, job output DIR, verified by HOW` (or
   `Delegated: none`).
