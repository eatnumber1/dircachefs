# Writing the prompt for foreman

Read this when you write a foreman prompt file. Pick the mode first
(SKILL.md, top). Both prompts are self-contained: the Gemini session has
none of your context and has not read `AGENTS.md`.

## Subagents inside a Gemini session

What `agy` offers (checked 2026-10-19 with `agy agents`, `agy models`,
`agy help` and agy's builtin skills): a built-in subagent tool for a
single delegated task and the `run_workflow` skill for fan-out and
aggregate. `agy agents` lists no custom agents here, and no flag or
documented field picks a subagent's model, so you cannot require one. What
you do choose is the session's own model with `--model`; the cheap ids are
`gemini-3.8-flash-low`, `gemini-3.8-flash-medium` and
`gemini-3.8-flash-high` (`agy models` lists the rest). To keep a menial
chunk cheap, run it as its own `--single` job on a cheap model, rather
than hoping the orchestrator session picks one.

## Orchestrator mode (a whole step on Gemini)

Checklist for the prompt file:
1. The step number, its entry in `docs/plan/phases/` (path and heading),
   and the indexes to read first (`AGENTS.md`, then the rule indexes its
   "Read before you start" table names for the task).
2. The lane, the branch (foreman made it) and the commit rules: subject
   prefix `N.M: `, trailer naming the model, never push, Docker is
   production, no timers or retry loops.
3. Tests first: which test fails first, which targets to run
   (`bazel test --config=fast //tools/...` and the step's own), and that
   every command runs in the foreground with no baseline Bazel run before
   the change.
4. Decomposition: "Split the work. Hand the menial pieces (greps, boilerplate,
   warning fixes) to your own subagents, one task each with its paths and
   its expected output, then read and check what they return before you use
   it. Keep design and the final edit yourself."
5. The report it ends with: diff summary, commands run with results, any
   deviation and design decision, then the line `JOB-COMPLETE`.

## Single mode (a bounded chunk)

Run it with `--single`; a survey that edits nothing also takes `--read-only`
(plan mode, no branch), and its text, minus the JOB-COMPLETE line, is saved
as `report.md`.

Template (fill the capitals; keep it under one screen):

```text
Read-only research for dcfs STEP (the dcfs repository is the current
directory). Do not edit any file. Do not use subagents. Run commands in
the foreground only. Your final answer is the report, in Markdown.

Goal: ONE SENTENCE, with the context it needs.
Inputs: exact paths (and git revisions, for example
`git -C TREE show TAG:path`; say which tree is the wrong version).
Report, citing exact paths and line numbers:
1. FIRST QUESTION.
2. SECOND QUESTION. Flag every unknown instead of guessing.
Size: at most N lines. If you run short of quota, print the rows you
have and say what is missing; a partial answer is acceptable.
```

Notes: write "Do not edit any file" only for a read-only job and say what
a write job may touch; untrusted data (a downloaded package) is read, never
executed; ask for a table when the answer is a set of rows, and name its
columns.
