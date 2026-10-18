# SPDX-License-Identifier: MIT
"""Foreman: runs queued steps end to end on Gemini in a lane.

Harness tool, not part of dcfs; see tools/agents/README.md. Drives worker
and review/fix rounds via agy in stream-json mode, holding stdin open until
JOB-COMPLETE is signaled.
"""

import argparse
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
    lines = [
        line for line in res.stdout.splitlines() if "Gemini Models" in line
    ]
    return "\n".join(lines) + ("\n" if lines else "")
  except (subprocess.SubprocessError, OSError):
    return ""


def run_stream_session(
    agy_bin,
    model,
    prompt,
    out_jsonl_path,
    out_txt_path,
    lane,
    clock=time.time,
    environ=os.environ,
):
  """Runs a worker or fix agy session in stream-json mode.

  Starts agy with --input-format stream-json --output-format stream-json and
  -p=, writes prompt as one input line with JOB-COMPLETE instruction appended,
  keeps stdin open, reads stdout events line by line, writes events to .jsonl
  and agent text to .out, closes stdin when an agent_response contains
  JOB-COMPLETE, and waits for agy to exit.

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
  stdin_closed = False
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
          continue

        event = data.get("event")
        if event == "step_update":
          su = data.get("step_update", {})
          if su.get("step_type") == "agent_response":
            text = su.get("text_delta")
            if text is None:
              text = su.get("text")
            if text is None:
              text = su.get("content", "")
            if text:
              txt_f.write(text)
              txt_f.flush()
              accumulated_response += text
              if "JOB-COMPLETE" in accumulated_response:
                saw_job_complete = True
                if not stdin_closed:
                  try:
                    proc.stdin.close()
                  except OSError:
                    pass
                  stdin_closed = True
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

  if not stdin_closed:
    try:
      proc.stdin.close()
    except OSError:
      pass

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
  """Runs a read-only review session using plain -p --mode plan."""
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
  ]
  try:
    res = subprocess.run(
        cmd,
        cwd=lane,
        env=environ,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
    )
    with open(out_review_path, "w", encoding="utf-8") as f:
      f.write(res.stdout)
    if res.returncode != 0:
      return (
          False,
          f"review exited with code {res.returncode}",
          res.stdout,
          False,
      )
    is_clean = any(line.strip() == "NONE" for line in res.stdout.splitlines())
    return True, None, res.stdout, is_clean
  except (subprocess.SubprocessError, OSError) as e:
    with open(out_review_path, "w", encoding="utf-8") as f:
      f.write(str(e))
    return False, f"review failed to run: {e}", "", False


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
    out_dir, step, lane, branch, base, git_bin="git", error_msg=None
):
  """Generates summary.md matching run.sh formatting."""
  lines = [f"# {step}", "", f"branch {branch} in {lane}, base {base}"]

  if error_msg:
    lines.extend(["", "## error", error_msg])

  commits = ""
  try:
    commits = subprocess.check_output(
        [git_bin, "log", "--oneline", f"{base}..HEAD"],
        cwd=lane,
        text=True,
    )
  except (subprocess.SubprocessError, OSError):
    pass
  lines.extend(["", "## commits", commits.rstrip("\n")])

  uncommitted = ""
  try:
    uncommitted = subprocess.check_output(
        [git_bin, "status", "--short"],
        cwd=lane,
        text=True,
    )
  except (subprocess.SubprocessError, OSError):
    pass
  lines.extend(["", "## uncommitted", uncommitted.rstrip("\n")])

  review_files = []
  if os.path.isdir(out_dir):
    for f in os.listdir(out_dir):
      if f.startswith("review") and f.endswith(".out"):
        review_files.append(f)
  review_files.sort()
  last_review_text = ""
  if review_files:
    latest_review = os.path.join(out_dir, review_files[-1])
    try:
      with open(latest_review, "r", encoding="utf-8") as rf:
        rlines = rf.readlines()
        last_review_text = "".join(rlines[-40:]).rstrip("\n")
    except OSError:
      pass
  lines.extend(["", "## last review", last_review_text])

  usage_before = ""
  usage_after = ""
  ub_path = os.path.join(out_dir, "usage.before")
  ua_path = os.path.join(out_dir, "usage.after")
  if os.path.exists(ub_path):
    try:
      with open(ub_path, "r", encoding="utf-8") as f:
        usage_before = f.read()
    except OSError:
      pass
  if os.path.exists(ua_path):
    try:
      with open(ua_path, "r", encoding="utf-8") as f:
        usage_after = f.read()
    except OSError:
      pass
  quota_text = (usage_before + usage_after).rstrip("\n")
  lines.extend(["", "## quota before / after", quota_text])

  summary_content = "\n".join(lines) + "\n"
  summary_path = os.path.join(out_dir, "summary.md")
  with open(summary_path, "w", encoding="utf-8") as f:
    f.write(summary_content)
  return summary_path


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
    clock=time.time,
    environ=os.environ,
):
  """Runs one queued step end to end on Gemini in a lane."""
  if not review_model:
    review_model = model
  if not out_dir:
    out_dir = default_out_dir(step, environ=environ)
  os.makedirs(out_dir, exist_ok=True)

  with open(prompt_file, "r", encoding="utf-8") as f:
    worker_prompt = f.read()

  base = setup_git_branch(
      lane=lane,
      branch=branch,
      git_bin=git_bin,
      environ=environ,
  )

  ub = read_usage(agy_bin, lane, environ=environ)
  with open(os.path.join(out_dir, "usage.before"), "w", encoding="utf-8") as f:
    f.write(ub)

  ok, err = run_stream_session(
      agy_bin=agy_bin,
      model=model,
      prompt=worker_prompt,
      out_jsonl_path=os.path.join(out_dir, "worker.jsonl"),
      out_txt_path=os.path.join(out_dir, "worker.out"),
      lane=lane,
      clock=clock,
      environ=environ,
  )
  if not ok:
    ua = read_usage(agy_bin, lane, environ=environ)
    with open(os.path.join(out_dir, "usage.after"), "w", encoding="utf-8") as f:
      f.write(ua)
    generate_summary(
        out_dir, step, lane, branch, base, git_bin=git_bin, error_msg=err
    )
    return 1

  for r in (1, 2):
    rev_ok, rev_err, rev_text, is_clean = run_review(
        agy_bin=agy_bin,
        review_model=review_model,
        base=base,
        worker_prompt=worker_prompt,
        out_review_path=os.path.join(out_dir, f"review{r}.out"),
        lane=lane,
        environ=environ,
    )
    if not rev_ok:
      ua = read_usage(agy_bin, lane, environ=environ)
      with open(
          os.path.join(out_dir, "usage.after"), "w", encoding="utf-8"
      ) as f:
        f.write(ua)
      generate_summary(
          out_dir, step, lane, branch, base, git_bin=git_bin, error_msg=rev_err
      )
      return 1

    if is_clean:
      break

    fix_prompt = build_fix_prompt(branch, rev_text, worker_prompt)
    fix_ok, fix_err = run_stream_session(
        agy_bin=agy_bin,
        model=model,
        prompt=fix_prompt,
        out_jsonl_path=os.path.join(out_dir, f"fix{r}.jsonl"),
        out_txt_path=os.path.join(out_dir, f"fix{r}.out"),
        lane=lane,
        clock=clock,
        environ=environ,
    )
    if not fix_ok:
      ua = read_usage(agy_bin, lane, environ=environ)
      with open(
          os.path.join(out_dir, "usage.after"), "w", encoding="utf-8"
      ) as f:
        f.write(ua)
      generate_summary(
          out_dir, step, lane, branch, base, git_bin=git_bin, error_msg=fix_err
      )
      return 1

  ua = read_usage(agy_bin, lane, environ=environ)
  with open(os.path.join(out_dir, "usage.after"), "w", encoding="utf-8") as f:
    f.write(ua)
  generate_summary(out_dir, step, lane, branch, base, git_bin=git_bin)
  return 0


def main(argv):
  parser = argparse.ArgumentParser(
      description=__doc__,
      formatter_class=argparse.RawDescriptionHelpFormatter,
  )
  parser.add_argument("step", help="Step name (e.g. 25.33)")
  parser.add_argument("lane", help="Path to lane directory")
  parser.add_argument("branch", help="Target branch name")
  parser.add_argument("prompt_file", help="Path to worker prompt file")
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
  args = parser.parse_args(argv)

  return run_foreman(
      step=args.step,
      lane=args.lane,
      branch=args.branch,
      prompt_file=args.prompt_file,
      model=args.model,
      review_model=args.review_model,
      out_dir=args.out,
      agy_bin=args.agy_bin,
      git_bin=args.git_bin,
  )


if __name__ == "__main__":
  sys.exit(main(sys.argv[1:]))
