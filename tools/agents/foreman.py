# SPDX-License-Identifier: MIT
"""Foreman: runs queued steps end to end on Gemini in a lane.

Harness tool, not part of dcfs; see tools/agents/README.md. Drives worker
and review/fix rounds via agy in stream-json mode, holding stdin open until
JOB-COMPLETE is signaled. With --single it runs the one worker session and
nothing else; with --read-only as well it runs in agy plan mode, leaves the
lane's git state alone and saves its text, minus the JOB-COMPLETE line, as
report.md.
"""

import argparse
import dataclasses
import json
import os
import subprocess
import sys
import tempfile
import time

DEFAULT_MODEL = "gemini-3.8-flash-medium"
DEFAULT_FOREMAN_BASE = (
    "/tmp/claude-1000/-home-russ-Sources-dircachefs/"
    "4cb5bd53-9fd3-4845-aa05-5c5668254104/scratchpad/foreman"
)

COMPLETION_INSTRUCTION = (
    "\n\nWhen all work is finished and every background command has reported "
    "back, print the line JOB-COMPLETE as the last thing you say."
)

# thinking_tokens is part of output_tokens, not an addition to it.
COUNT_KEYS = (
    "input_tokens",
    "output_tokens",
    "thinking_tokens",
    "cache_read_tokens",
    "total_tokens",
)

# Tools through which a Gemini session starts a subagent; see README.
SUBAGENT_TOOLS = ("invoke_subagent", "browser_subagent")


@dataclasses.dataclass
class SessionUsage:
  """Token counts and model of one agy session."""

  model: str = ""
  counts: dict = dataclasses.field(
      default_factory=lambda: {k: 0 for k in COUNT_KEYS}
  )
  subagent_calls: int = 0
  duration_s: float = 0.0


def parse_stream_usage(jsonl_path):
  """Reads a stream-json recording and returns its SessionUsage.

  Counts come from the result event's usage; without a result event (agy
  died) they are the sum of the step_update usage fields.
  """
  usage = SessionUsage()
  step_sum = {k: 0 for k in COUNT_KEYS}
  result_usage = None
  with open(jsonl_path, "r", encoding="utf-8") as f:
    for line in f:
      try:
        data = json.loads(line)
      except json.JSONDecodeError:
        continue
      event = data.get("event")
      if event == "init":
        usage.model = data.get("init", {}).get("model", "")
      elif event == "result":
        result = data.get("result", {})
        result_usage = result.get("usage")
        usage.duration_s = float(result.get("duration_seconds") or 0)
      elif event == "step_update":
        su = data.get("step_update", {})
        if su.get("tool_name") in SUBAGENT_TOOLS:
          usage.subagent_calls += 1
        for k in COUNT_KEYS:
          step_sum[k] += (su.get("usage") or {}).get(k, 0)
  source = result_usage if result_usage is not None else step_sum
  usage.counts = {k: int(source.get(k, 0)) for k in COUNT_KEYS}
  return usage


def parse_review_json(text):
  """Returns the JSON object agy printed in text, or None."""
  try:
    parsed = json.loads(text)
  except ValueError:
    return None
  return parsed if isinstance(parsed, dict) else None


def add_session(sessions, role, name, model, usage):
  """Appends one session record; role is worker, fix, review or single."""
  sessions.append({
      "role": role,
      "name": name,
      "model": usage.model or model,
      "usage": dict(usage.counts),
      "subagent_calls": usage.subagent_calls,
      "duration_s": usage.duration_s,
  })


def build_tokens(sessions):
  """Returns the tokens.json document for the recorded sessions."""
  total = {k: 0 for k in COUNT_KEYS}
  for s in sessions:
    for k in COUNT_KEYS:
      total[k] += s["usage"][k]
  return {
      "sessions": sessions,
      "total": total,
  }


def tokens_table(tokens):
  """Returns the markdown table of tokens.json's sessions."""
  lines = [
      "| role | model | input | output | thinking | cache read | total |",
      "|---|---|---|---|---|---|---|",
  ]
  for s in tokens["sessions"]:
    u = s["usage"]
    lines.append(
        f"| {s['role']} | {s['model']} | "
        + " | ".join(str(u[k]) for k in COUNT_KEYS)
        + " |"
    )
  t = tokens["total"]
  lines.append(
      "| total | | " + " | ".join(str(t[k]) for k in COUNT_KEYS) + " |"
  )
  return "\n".join(lines)


def record_ledger_row(step, model, mode, tokens, rounds, error, environ):
  """Adds this run's dispatch-ledger row through dashboard_lanes.py."""
  total = tokens["total"]
  breakdown = "; ".join(
      f"{s['name']}={s['usage']['total_tokens']}" for s in tokens["sessions"]
  )
  notes = f"foreman mode={mode}; {breakdown}"
  if error:
    notes += "; failed"
  cmd = [
      sys.executable,
      "-I",
      os.path.join(os.path.dirname(os.path.abspath(__file__)),
                   "dashboard_lanes.py"),
      "ledger",
      "add",
      f"step={step}",
      "agent=agy",
      f"model={model}",
      # As the Claude rows count: cache reads are part of tokens.
      f"tokens={total['total_tokens'] + total['cache_read_tokens']}",
      f"duration_s={round(sum(s['duration_s'] for s in tokens['sessions']))}",
      f"input_tokens={total['input_tokens']}",
      f"output_tokens={total['output_tokens']}",
      f"cache_read_tokens={total['cache_read_tokens']}",
      f"rounds={rounds}",
      f"notes={notes}",
  ]
  res = subprocess.run(cmd, env=environ, capture_output=True, text=True)
  if res.returncode != 0 or res.stderr:
    print(
        f"foreman: ledger: exit {res.returncode}: {res.stderr.strip()}",
        file=sys.stderr,
    )


def default_out_dir(step, environ=os.environ):
  """Returns the output directory for foreman logs and summary."""
  base = environ.get("DCFS_FOREMAN_DIR")
  if not base:
    if os.path.isdir(DEFAULT_FOREMAN_BASE):
      base = DEFAULT_FOREMAN_BASE
    else:
      base = os.path.join(tempfile.gettempdir(), "foreman")
  return os.path.join(base, step)


def setup_git_branch(
    lane, branch, main_repo=None, git_bin="git", environ=os.environ
):
  """Fetches main and checks out a fresh branch at FETCH_HEAD."""
  if not main_repo:
    main_repo = environ.get(
        "DCFS_MAIN_REPO", os.path.expanduser("~/Sources/dircachefs")
    )
  subprocess.run(
      [git_bin, "fetch", "-q", main_repo, "main"],
      cwd=lane,
      check=True,
  )
  subprocess.run(
      [git_bin, "checkout", "-q", "-B", branch, "FETCH_HEAD"],
      cwd=lane,
      check=True,
  )
  return subprocess.check_output(
      [git_bin, "rev-parse", "HEAD"], cwd=lane, text=True
  ).strip()


def has_job_complete_line(text):
  """Returns True if some line of text is exactly JOB-COMPLETE once stripped."""
  return any(line.strip() == "JOB-COMPLETE" for line in text.split("\n"))


def read_usage(agy_bin, lane, timeout=120, environ=os.environ):
  """Reads usage via agy -p /usage and filters for Gemini Models."""
  cmd = [agy_bin, "-p", "/usage"]
  try:
    res = subprocess.run(
        cmd,
        cwd=lane,
        env=environ,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
        timeout=timeout,
    )
  except (subprocess.SubprocessError, OSError) as e:
    return f"usage meter failed: {e}\n"
  lines = [line for line in res.stdout.splitlines() if "Gemini Models" in line]
  return "\n".join(lines) + ("\n" if lines else "")


def run_stream_session(
    agy_bin,
    model,
    prompt,
    out_jsonl_path,
    out_txt_path,
    lane,
    read_only=False,
    clock=time.time,
    environ=os.environ,
):
  """Runs a worker or fix agy session in stream-json mode.

  Starts agy with --input-format stream-json --output-format stream-json and
  -p=, writes prompt as one input line with JOB-COMPLETE instruction appended,
  keeps stdin open, reads stdout events line by line, writes events to .jsonl
  and agent text to .out, closes stdin when the agent text has a line that is
  exactly JOB-COMPLETE, and waits for agy to exit.

  Returns (success: bool, error_message: str | None).
  """
  del clock  # Clock parameter for consistency with injected harness tools.
  cmd = [
      agy_bin,
      "--model",
      model,
      "--dangerously-skip-permissions",
      "--input-format",
      "stream-json",
      "--output-format",
      "stream-json",
      "-p=",
  ]
  if read_only:
    cmd += ["--mode", "plan"]

  prompt_with_instruction = prompt + COMPLETION_INSTRUCTION
  input_event = (
      json.dumps({
          "event": "user",
          "message": {"content": prompt_with_instruction},
      })
      + "\n"
  )

  proc = subprocess.Popen(
      cmd,
      stdin=subprocess.PIPE,
      stdout=subprocess.PIPE,
      stderr=subprocess.STDOUT,
      text=True,
      cwd=lane,
      env=environ,
      bufsize=1,
  )

  proc.stdin.write(input_event)
  proc.stdin.flush()

  saw_job_complete = False
  error_result = None
  accumulated_response = ""

  try:
    with open(out_jsonl_path, "w", encoding="utf-8") as jsonl_f, open(
        out_txt_path, "w", encoding="utf-8"
    ) as txt_f:
      for line in proc.stdout:
        jsonl_f.write(line)
        jsonl_f.flush()

        line_str = line.strip()
        if not line_str:
          continue

        try:
          data = json.loads(line_str)
        except json.JSONDecodeError:
          print(f"foreman: undecodable agy event: {line_str}", file=sys.stderr)
          continue

        event = data.get("event")
        if event == "step_update":
          su = data.get("step_update", {})
          if su.get("step_type") == "agent_response":
            text = su.get("text_delta", "")
            if text:
              txt_f.write(text)
              txt_f.flush()
              accumulated_response += text
              if not saw_job_complete and has_job_complete_line(
                  accumulated_response
              ):
                saw_job_complete = True
                proc.stdin.close()
        elif event == "result":
          res = data.get("result", {})
          if res.get("status") == "ERROR":
            error_result = res.get("response") or "ERROR status in result"
          if not accumulated_response and res.get("response"):
            txt_f.write(res["response"])
            txt_f.flush()
  finally:
    if proc.stdout:
      proc.stdout.close()

  proc.wait()
  proc.stdin.close()

  if error_result:
    return False, f"agy result ERROR: {error_result}"
  if not saw_job_complete:
    return False, f"agy exited before JOB-COMPLETE (code {proc.returncode})"
  if proc.returncode != 0:
    return False, f"agy exited with code {proc.returncode}"

  return True, None


def run_review(
    agy_bin,
    review_model,
    base,
    worker_prompt,
    out_review_path,
    lane,
    environ=os.environ,
):
  """Runs a read-only review session using -p --mode plan, JSON output.

  Writes the review text to out_review_path and the raw JSON beside it.
  Returns (ok, error, text, is_clean, usage).
  """
  review_prompt = (
      "Read-only review; do not modify anything and do not run builds. "
      f"In the current directory (the dcfs repository), review `git diff {base}"
      " HEAD` and any uncommitted changes against docs/style.md "
      "(sections 1.6, 1.9, 2, 3, 5, 7) and AGENTS.md, "
      "and against the task it was meant to do, which follows. "
      "List only real defects, each as `path:line \u2014 rule \u2014 defect"
      " \u2014 fix`. If there are none and the work is committed, reply with"
      f" the single word NONE.\n\nTASK:\n{worker_prompt}"
  )
  cmd = [
      agy_bin,
      "-p",
      review_prompt,
      "--model",
      review_model,
      "--mode",
      "plan",
      "--output-format",
      "json",
  ]
  usage = SessionUsage(model=review_model)
  try:
    res = subprocess.run(
        cmd,
        cwd=lane,
        env=environ,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
  except (subprocess.SubprocessError, OSError) as e:
    with open(out_review_path, "w", encoding="utf-8") as f:
      f.write(str(e))
    return False, f"review failed to run: {e}", "", False, usage

  with open(
      os.path.splitext(out_review_path)[0] + ".json", "w", encoding="utf-8"
  ) as f:
    f.write(res.stdout)
  parsed = parse_review_json(res.stdout)
  if parsed is None:
    with open(out_review_path, "w", encoding="utf-8") as f:
      f.write(res.stdout + res.stderr)
    return (
        False,
        f"review output is not JSON (code {res.returncode})",
        res.stdout,
        False,
        usage,
    )
  text = parsed["response"]
  with open(out_review_path, "w", encoding="utf-8") as f:
    f.write(text if text.endswith("\n") else text + "\n")
  counts = parsed["usage"]
  usage.counts = {k: int(counts[k]) for k in COUNT_KEYS}
  usage.duration_s = float(parsed["duration_seconds"])
  if parsed["status"] == "ERROR":
    return False, f"review ERROR: {parsed['error']}", text, False, usage
  is_clean = any(line.strip() == "NONE" for line in text.splitlines())
  return True, None, text, is_clean, usage


def build_fix_prompt(branch, review_output, worker_prompt):
  """Builds prompt instructing worker to fix review findings."""
  return (
      "Do NOT run any bazel command before your changes are made, "
      "and run every command in the foreground. "
      f"In the current directory (the dcfs repository, branch {branch}), "
      "fix these review findings, then rerun the tests the task names "
      "in the foreground and commit (subject prefix as the task says, "
      "Co-Authored-By: Gemini <noreply@google.com>; never push, "
      "never touch Docker, no sleeps or retry loops). Findings:\n"
      f"{review_output}\n\n"
      "The task, for reference:\n"
      f"{worker_prompt}"
  )


def generate_summary(
    out_dir, step, lane, branch, base, git_bin="git", error_msg=None,
    tokens=None, single=False, read_only=False,
):
  """Generates summary.md matching run.sh formatting.

  A read-only run has no branch or commits: the summary says read-only and
  names the directory.
  """
  if read_only:
    lines = [f"# {step}", "", f"read-only run in {lane}"]
  else:
    lines = [f"# {step}", "", f"branch {branch} in {lane}, base {base}"]

  if error_msg:
    lines.extend(["", "## error", error_msg])

  if not read_only:
    commits = subprocess.check_output(
        [git_bin, "log", "--oneline", f"{base}..HEAD"],
        cwd=lane,
        text=True,
    )
    lines.extend(["", "## commits", commits.rstrip("\n")])

    uncommitted = subprocess.check_output(
        [git_bin, "status", "--short"],
        cwd=lane,
        text=True,
    )
    lines.extend(["", "## uncommitted", uncommitted.rstrip("\n")])

  if not single:
    review_files = []
    for f in os.listdir(out_dir):
      if f.startswith("review") and f.endswith(".out"):
        review_files.append(f)
    review_files.sort()
    last_review_text = ""
    if review_files:
      latest_review = os.path.join(out_dir, review_files[-1])
      with open(latest_review, "r", encoding="utf-8") as rf:
        rlines = rf.readlines()
        last_review_text = "".join(rlines[-40:]).rstrip("\n")
    lines.extend(["", "## last review", last_review_text])

  with open(os.path.join(out_dir, "usage.before"), "r", encoding="utf-8") as f:
    usage_before = f.read()
  with open(os.path.join(out_dir, "usage.after"), "r", encoding="utf-8") as f:
    usage_after = f.read()
  quota_text = (usage_before + usage_after).rstrip("\n")
  lines.extend(["", "## quota before / after", quota_text])
  if tokens is not None:
    lines.extend(["", "## tokens", tokens_table(tokens)])

  summary_content = "\n".join(lines) + "\n"
  summary_path = os.path.join(out_dir, "summary.md")
  with open(summary_path, "w", encoding="utf-8") as f:
    f.write(summary_content)
  return summary_path


def save_report(out_dir):
  """Saves the worker's text, minus its JOB-COMPLETE line, as report.md."""
  with open(os.path.join(out_dir, "worker.out"), "r", encoding="utf-8") as f:
    kept = [line for line in f if line.strip() != "JOB-COMPLETE"]
  with open(os.path.join(out_dir, "report.md"), "w", encoding="utf-8") as f:
    f.writelines(kept)


def run_foreman(
    step,
    lane,
    branch,
    prompt_file,
    model=DEFAULT_MODEL,
    review_model=None,
    out_dir=None,
    agy_bin="agy",
    git_bin="git",
    single=False,
    read_only=False,
    clock=time.time,
    environ=os.environ,
):
  """Runs one queued step end to end on Gemini in a lane.

  With single, runs one held-open worker session and no review or fix rounds.
  With read_only (requires single), the worker runs in agy plan mode, nothing
  in lane changes (it is only the working directory, and branch is unused),
  and the worker's final text is saved as report.md.
  """
  if single:
    mode = "read-only" if read_only else "single"
  else:
    mode = "orchestrator"
  if not review_model:
    review_model = model
  if not out_dir:
    out_dir = default_out_dir(step, environ=environ)
  os.makedirs(out_dir, exist_ok=True)

  with open(prompt_file, "r", encoding="utf-8") as f:
    worker_prompt = f.read()

  base = None
  if not read_only:
    base = setup_git_branch(
        lane=lane,
        branch=branch,
        git_bin=git_bin,
        environ=environ,
    )

  ub = read_usage(agy_bin, lane, environ=environ)
  with open(os.path.join(out_dir, "usage.before"), "w", encoding="utf-8") as f:
    f.write(ub)

  sessions = []
  rounds = 0

  def finish(error=None):
    ua = read_usage(agy_bin, lane, environ=environ)
    with open(os.path.join(out_dir, "usage.after"), "w", encoding="utf-8") as f:
      f.write(ua)
    tokens = build_tokens(sessions)
    with open(
        os.path.join(out_dir, "tokens.json"), "w", encoding="utf-8"
    ) as f:
      json.dump(tokens, f, indent=2)
      f.write("\n")
    generate_summary(
        out_dir, step, lane, branch, base, git_bin=git_bin, error_msg=error,
        tokens=tokens, single=single, read_only=read_only,
    )
    record_ledger_row(step, model, mode, tokens, rounds, error, environ)
    return 1 if error else 0

  def run_session(role, name, prompt):
    ok, err = run_stream_session(
        agy_bin=agy_bin,
        model=model,
        prompt=prompt,
        out_jsonl_path=os.path.join(out_dir, f"{name}.jsonl"),
        out_txt_path=os.path.join(out_dir, f"{name}.out"),
        lane=lane,
        read_only=read_only,
        clock=clock,
        environ=environ,
    )
    add_session(
        sessions, role, name, model,
        parse_stream_usage(os.path.join(out_dir, f"{name}.jsonl")),
    )
    return ok, err

  ok, err = run_session(
      "worker" if mode == "orchestrator" else "single", "worker", worker_prompt
  )
  if ok and read_only:
    save_report(out_dir)
  if not ok or single:
    return finish(err)

  for r in (1, 2):
    rev_ok, rev_err, rev_text, is_clean, rev_usage = run_review(
        agy_bin=agy_bin,
        review_model=review_model,
        base=base,
        worker_prompt=worker_prompt,
        out_review_path=os.path.join(out_dir, f"review{r}.out"),
        lane=lane,
        environ=environ,
    )
    rounds = r
    add_session(sessions, "review", f"review{r}", review_model, rev_usage)
    if not rev_ok:
      return finish(rev_err)

    if is_clean:
      break

    fix_ok, fix_err = run_session(
        "fix", f"fix{r}", build_fix_prompt(branch, rev_text, worker_prompt)
    )
    if not fix_ok:
      return finish(fix_err)

  return finish()


def main(argv):
  parser = argparse.ArgumentParser(
      description=__doc__,
      formatter_class=argparse.RawDescriptionHelpFormatter,
  )
  parser.add_argument("step", help="Step name (e.g. 25.33)")
  parser.add_argument("lane", help="Path to lane directory")
  parser.add_argument(
      "rest",
      nargs="+",
      metavar="branch prompt_file",
      help="Target branch name and path to the worker prompt file; with "
      "--read-only only the prompt file (there is no branch)",
  )
  parser.add_argument(
      "--model",
      default=DEFAULT_MODEL,
      help=f"Model for worker/fix sessions (default: {DEFAULT_MODEL})",
  )
  parser.add_argument(
      "--review-model",
      default=None,
      help="Model for review sessions (default: same as --model)",
  )
  parser.add_argument(
      "--out",
      default=None,
      help="Output directory (default: scratchpad/foreman/<step>)",
  )
  parser.add_argument(
      "--agy-bin",
      default="agy",
      help="agy binary path (default: agy)",
  )
  parser.add_argument(
      "--git-bin",
      default="git",
      help="git binary path (default: git)",
  )
  parser.add_argument(
      "--single",
      action="store_true",
      help="One worker session; no review or fix rounds",
  )
  parser.add_argument(
      "--read-only",
      action="store_true",
      help="With --single: agy plan mode, no branch setup, saves report.md",
  )
  args = parser.parse_args(argv)
  if args.read_only and not args.single:
    parser.error("--read-only requires --single")
  if args.single and args.review_model:
    parser.error("--review-model has no effect with --single")
  if args.read_only:
    if len(args.rest) != 1:
      parser.error("--read-only takes no branch: step lane prompt_file")
    branch, prompt_file = None, args.rest[0]
  else:
    if len(args.rest) != 2:
      parser.error("expected branch and prompt_file after the lane")
    branch, prompt_file = args.rest

  return run_foreman(
      step=args.step,
      lane=args.lane,
      branch=branch,
      prompt_file=prompt_file,
      model=args.model,
      review_model=args.review_model,
      out_dir=args.out,
      agy_bin=args.agy_bin,
      git_bin=args.git_bin,
      single=args.single,
      read_only=args.read_only,
  )


if __name__ == "__main__":
  sys.exit(main(sys.argv[1:]))
