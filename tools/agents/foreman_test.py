# SPDX-License-Identifier: MIT
"""Unit tests for foreman.py (Gemini lane foreman)."""

import contextlib
import io
import json
import os
import stat
import subprocess
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import foreman

TESTDATA = os.path.join(os.path.dirname(os.path.abspath(__file__)), "testdata")

FAKE_AGY_SCRIPT = """#!/usr/bin/env python3
import json
import os
import sys

args = sys.argv[1:]


def emit_text(text):
  sys.stdout.write(
      json.dumps({
          "event": "step_update",
          "step_update": {"step_type": "agent_response", "text_delta": text},
      })
      + "\\n"
  )
  sys.stdout.flush()

log_file = os.environ.get("FAKE_AGY_LOG")
if log_file:
  with open(log_file, "a", encoding="utf-8") as f:
    f.write(json.dumps(args) + "\\n")

if "-p" in args and "/usage" in args:
  sys.stdout.write(
      "Gemini Models\\tWeekly Limit Remaining\\t50%\\t2026-10-24T03:07:32Z\\n"
  )
  sys.stdout.flush()
  sys.exit(0)

if "--mode" in args and "plan" in args:
  # Review mode: one JSON object, as agy --output-format json prints it.
  if "--output-format" not in args or (
      args[args.index("--output-format") + 1] != "json"
  ):
    sys.stderr.write("fake_agy: review needs --output-format json\\n")
    sys.exit(1)
  defects_file = os.environ.get("FAKE_AGY_DEFECTS_FILE")
  if defects_file and os.path.exists(defects_file):
    with open(defects_file, "r", encoding="utf-8") as f:
      content = f.read().strip()
    os.remove(defects_file)
  else:
    content = "NONE"
  if os.environ.get("FAKE_AGY_REVIEW_MODE") == "error":
    sys.stderr.write("AGY_ERROR: {}\\n")
    content = ""
    status = "ERROR"
  else:
    status = "SUCCESS"
  sys.stdout.write(
      json.dumps({
          "conversation_id": "review-conv",
          "status": status,
          "response": content,
          "duration_seconds": 1.0,
          "num_turns": 1,
          "usage": {
              "input_tokens": 1000,
              "output_tokens": 100,
              "thinking_tokens": 10,
              "cache_read_tokens": 5000,
              "total_tokens": 1100,
          },
      })
      + "\\n"
  )
  sys.stdout.flush()
  sys.exit(0)

if "--input-format" in args and "stream-json" in args:
  mode = os.environ.get("FAKE_AGY_STREAM_MODE", "normal")
  # Read user prompt event
  line = sys.stdin.readline()
  if not line:
    sys.exit(1)

  if mode == "exit_early":
    event = {
        "event": "step_update",
        "step_update": {
            "step_type": "agent_response",
            "text_delta": "failing before complete\\n",
        },
    }
    sys.stdout.write(json.dumps(event) + "\\n")
    sys.stdout.flush()
    sys.exit(1)

  if mode == "error_result":
    event = {
        "event": "result",
        "result": {
            "status": "ERROR",
            "response": "Resource exhausted / quota",
        },
    }
    sys.stdout.write(json.dumps(event) + "\\n")
    sys.stdout.flush()
    sys.exit(0)

  if mode == "mention_first":
    # JOB-COMPLETE inside a sentence is not the marker. The early-close
    # predicate is unit-tested; this checks the session still ends on the
    # bare line.
    emit_text("I will print JOB-COMPLETE when done.\\n")
    emit_text("More work.\\n")
    emit_text("JOB-COMPLETE\\n")
    sys.stdin.read()
    sys.stdout.write(
        json.dumps({
            "event": "result",
            "result": {"status": "SUCCESS", "num_turns": 1},
        })
        + "\\n"
    )
    sys.stdout.flush()
    sys.exit(0)

  if mode == "wrong_field":
    # Agent text is read from text_delta only; a text field is ignored.
    sys.stdout.write(
        json.dumps({
            "event": "step_update",
            "step_update": {
                "step_type": "agent_response",
                "text": "JOB-COMPLETE\\n",
            },
        })
        + "\\n"
    )
    sys.stdout.flush()
    sys.exit(0)

  # Normal stream-json mode:
  sys.stdout.write(
      json.dumps({
          "event": "init",
          "conversation_id": "stream-conv",
          "init": {"model": args[args.index("--model") + 1]},
      })
      + "\\n"
  )
  if mode == "garbage":
    sys.stdout.write("this is not json\\n")
    sys.stdout.flush()
  # 1. user_input step
  sys.stdout.write(
      json.dumps({
          "event": "step_update",
          "step_update": {"step_type": "user_input", "state": "DONE"},
      })
      + "\\n"
  )
  sys.stdout.flush()

  # 2. initial agent response
  sys.stdout.write(
      json.dumps({
          "event": "step_update",
          "step_update": {
              "step_type": "agent_response",
              "state": "ACTIVE",
              "text_delta": "Starting background job.\\n",
          },
      })
      + "\\n"
  )
  sys.stdout.flush()

  # 3. background completion system_message
  sys.stdout.write(
      json.dumps({
          "event": "step_update",
          "step_update": {
              "step_type": "system_message",
              "state": "DONE",
              "duration_seconds": 15.0,
          },
      })
      + "\\n"
  )
  sys.stdout.flush()

  # 4. agent response with JOB-COMPLETE
  sys.stdout.write(
      json.dumps({
          "event": "step_update",
          "step_update": {
              "step_type": "agent_response",
              "state": "DONE",
              "text_delta": "Work is done.\\nJOB-COMPLETE\\n",
          },
      })
      + "\\n"
  )
  sys.stdout.flush()

  # 5. wait for stdin EOF (foreman closes stdin on JOB-COMPLETE)
  sys.stdin.read()

  # 6. emit final result and exit
  sys.stdout.write(
      json.dumps({
          "event": "result",
          "result": {
              "status": "SUCCESS",
              "response": (
                  "Starting background job.\\nWork is done.\\nJOB-COMPLETE\\n"
              ),
              "num_turns": 1,
              "usage": {
                  "input_tokens": 2000,
                  "output_tokens": 200,
                  "thinking_tokens": 20,
                  "cache_read_tokens": 9000,
                  "total_tokens": 2200,
              },
          },
      })
      + "\\n"
  )
  sys.stdout.flush()
  sys.exit(0)

sys.stderr.write("fake_agy: unrecognized arguments: " + " ".join(args) + "\\n")
sys.exit(1)
"""


class ForemanTest(unittest.TestCase):

  def setUp(self):
    self.temp_dir = tempfile.TemporaryDirectory()
    self.addCleanup(self.temp_dir.cleanup)
    self.root = self.temp_dir.name

    # Set up fake agy script
    self.fake_agy_path = os.path.join(self.root, "fake_agy.py")
    with open(self.fake_agy_path, "w", encoding="utf-8") as f:
      f.write(FAKE_AGY_SCRIPT)
    st = os.stat(self.fake_agy_path)
    os.chmod(self.fake_agy_path, st.st_mode | stat.S_IEXEC)

    # Set up fake git repos: main_repo and lane
    self.main_repo = os.path.join(self.root, "main_repo")
    self.lane = os.path.join(self.root, "lane")
    os.makedirs(self.main_repo)
    os.makedirs(self.lane)

    # Initialize main_repo
    self._git(self.main_repo, ["init", "-b", "main"])
    self._git(self.main_repo, ["config", "user.email", "test@example.com"])
    self._git(self.main_repo, ["config", "user.name", "Test User"])
    test_file = os.path.join(self.main_repo, "hello.txt")
    with open(test_file, "w", encoding="utf-8") as f:
      f.write("hello\n")
    self._git(self.main_repo, ["add", "hello.txt"])
    self._git(self.main_repo, ["commit", "-m", "initial commit"])

    # Initialize lane
    self._git(self.lane, ["init", "-b", "main"])
    self._git(self.lane, ["config", "user.email", "test@example.com"])
    self._git(self.lane, ["config", "user.name", "Test User"])
    self._git(self.lane, ["remote", "add", "origin", self.main_repo])

    # Prompt file
    self.prompt_file = os.path.join(self.root, "prompt.txt")
    with open(self.prompt_file, "w", encoding="utf-8") as f:
      f.write("Test task prompt\n")

    self.out_dir = os.path.join(self.root, "out")
    self.dashboard = os.path.join(self.root, "dashboard")

  def _git(self, cwd, args):
    subprocess.run(["git"] + args, cwd=cwd, check=True, capture_output=True)

  def test_worker_and_clean_review_success(self):
    ret = foreman.run_foreman(
        step="test-step",
        lane=self.lane,
        branch="step-branch",
        prompt_file=self.prompt_file,
        out_dir=self.out_dir,
        agy_bin=self.fake_agy_path,
        environ={
            "DCFS_MAIN_REPO": self.main_repo,
            "DCFS_DASHBOARD_DIR": self.dashboard,
        },
    )
    self.assertEqual(ret, 0)

    # Check files created
    worker_jsonl = os.path.join(self.out_dir, "worker.jsonl")
    worker_out = os.path.join(self.out_dir, "worker.out")
    review_out = os.path.join(self.out_dir, "review1.out")
    summary_md = os.path.join(self.out_dir, "summary.md")
    usage_before = os.path.join(self.out_dir, "usage.before")
    usage_after = os.path.join(self.out_dir, "usage.after")

    self.assertTrue(os.path.exists(worker_jsonl))
    self.assertTrue(os.path.exists(worker_out))
    self.assertTrue(os.path.exists(review_out))
    self.assertTrue(os.path.exists(summary_md))
    self.assertTrue(os.path.exists(usage_before))
    self.assertTrue(os.path.exists(usage_after))

    with open(worker_out, "r", encoding="utf-8") as f:
      out_text = f.read()
    self.assertIn("Starting background job.", out_text)
    self.assertIn("JOB-COMPLETE", out_text)

    with open(review_out, "r", encoding="utf-8") as f:
      self.assertEqual(f.read().strip(), "NONE")

    with open(summary_md, "r", encoding="utf-8") as f:
      summary = f.read()
    self.assertIn("# test-step", summary)
    self.assertIn("branch step-branch in", summary)
    self.assertIn("## commits", summary)
    self.assertIn("## uncommitted", summary)
    self.assertIn("## last review\nNONE", summary)
    self.assertIn("## quota before / after", summary)

  def test_review_defects_and_fix_round(self):
    # Set up defect file for first review
    defects_file = os.path.join(self.root, "defects.txt")
    with open(defects_file, "w", encoding="utf-8") as f:
      f.write("foo.cc:10 — style.md:1.2 — missing ref — fix it")

    env = {
        "DCFS_MAIN_REPO": self.main_repo,
        "DCFS_DASHBOARD_DIR": self.dashboard,
        "FAKE_AGY_DEFECTS_FILE": defects_file,
    }
    ret = foreman.run_foreman(
        step="fix-step",
        lane=self.lane,
        branch="step-fix",
        prompt_file=self.prompt_file,
        out_dir=self.out_dir,
        agy_bin=self.fake_agy_path,
        environ=env,
    )
    self.assertEqual(ret, 0)

    # Both review1 and review2 ran, and fix1 ran
    review1 = os.path.join(self.out_dir, "review1.out")
    review2 = os.path.join(self.out_dir, "review2.out")
    fix1_jsonl = os.path.join(self.out_dir, "fix1.jsonl")
    fix1_out = os.path.join(self.out_dir, "fix1.out")

    self.assertTrue(os.path.exists(review1))
    self.assertTrue(os.path.exists(review2))
    self.assertTrue(os.path.exists(fix1_jsonl))
    self.assertTrue(os.path.exists(fix1_out))

    with open(review1, "r", encoding="utf-8") as f:
      self.assertIn("missing ref", f.read())
    with open(review2, "r", encoding="utf-8") as f:
      self.assertEqual(f.read().strip(), "NONE")

  def test_early_exit_without_job_complete(self):
    env = {
        "DCFS_MAIN_REPO": self.main_repo,
        "DCFS_DASHBOARD_DIR": self.dashboard,
        "FAKE_AGY_STREAM_MODE": "exit_early",
    }
    ret = foreman.run_foreman(
        step="fail-step",
        lane=self.lane,
        branch="step-fail",
        prompt_file=self.prompt_file,
        out_dir=self.out_dir,
        agy_bin=self.fake_agy_path,
        environ=env,
    )
    self.assertNotEqual(ret, 0)

    summary_md = os.path.join(self.out_dir, "summary.md")
    self.assertTrue(os.path.exists(summary_md))
    with open(summary_md, "r", encoding="utf-8") as f:
      summary = f.read()
    self.assertIn("## error", summary)
    self.assertIn("JOB-COMPLETE", summary)
    # Review should not have run
    self.assertFalse(os.path.exists(os.path.join(self.out_dir, "review1.out")))

  def test_error_result_stops_job(self):
    env = {
        "DCFS_MAIN_REPO": self.main_repo,
        "DCFS_DASHBOARD_DIR": self.dashboard,
        "FAKE_AGY_STREAM_MODE": "error_result",
    }
    ret = foreman.run_foreman(
        step="quota-step",
        lane=self.lane,
        branch="step-quota",
        prompt_file=self.prompt_file,
        out_dir=self.out_dir,
        agy_bin=self.fake_agy_path,
        environ=env,
    )
    self.assertNotEqual(ret, 0)

    summary_md = os.path.join(self.out_dir, "summary.md")
    self.assertTrue(os.path.exists(summary_md))
    with open(summary_md, "r", encoding="utf-8") as f:
      summary = f.read()
    self.assertIn("## error", summary)
    self.assertIn("Resource exhausted / quota", summary)

  def test_job_complete_inside_a_sentence_does_not_end_session(self):
    env = {
        "DCFS_MAIN_REPO": self.main_repo,
        "DCFS_DASHBOARD_DIR": self.dashboard,
        "FAKE_AGY_STREAM_MODE": "mention_first",
    }
    ret = foreman.run_foreman(
        step="mention-step",
        lane=self.lane,
        branch="step-mention",
        prompt_file=self.prompt_file,
        out_dir=self.out_dir,
        agy_bin=self.fake_agy_path,
        environ=env,
    )
    self.assertEqual(ret, 0)
    with open(os.path.join(self.out_dir, "worker.out"), "r",
              encoding="utf-8") as f:
      out_text = f.read()
    self.assertIn("More work.", out_text)

  def test_job_complete_line_predicate(self):
    self.assertFalse(
        foreman.has_job_complete_line("I will print JOB-COMPLETE when done.")
    )
    self.assertTrue(foreman.has_job_complete_line("x\nJOB-COMPLETE\n"))
    self.assertTrue(foreman.has_job_complete_line("  JOB-COMPLETE  "))
    self.assertFalse(foreman.has_job_complete_line("JOB-COMPLETE."))

  def test_agent_text_is_read_from_text_delta_only(self):
    worker_out = os.path.join(self.root, "wrong.out")
    foreman.run_stream_session(
        agy_bin=self.fake_agy_path,
        model="m",
        prompt="p",
        out_jsonl_path=os.path.join(self.root, "wrong.jsonl"),
        out_txt_path=worker_out,
        lane=self.lane,
        environ={
            **os.environ,
            "FAKE_AGY_STREAM_MODE": "wrong_field",
        },
    )
    with open(worker_out, "r", encoding="utf-8") as f:
      self.assertNotIn("JOB-COMPLETE", f.read())

  def test_undecodable_agy_line_is_reported_and_skipped(self):
    env = {
        "DCFS_MAIN_REPO": self.main_repo,
        "DCFS_DASHBOARD_DIR": self.dashboard,
        "FAKE_AGY_STREAM_MODE": "garbage",
    }
    stderr = io.StringIO()
    with contextlib.redirect_stderr(stderr):
      ret = foreman.run_foreman(
          step="garbage-step",
          lane=self.lane,
          branch="step-garbage",
          prompt_file=self.prompt_file,
          out_dir=self.out_dir,
          agy_bin=self.fake_agy_path,
          environ=env,
      )
    self.assertEqual(ret, 0)
    self.assertIn(
        "foreman: undecodable agy event: this is not json\n", stderr.getvalue()
    )

  def test_generate_summary_git_failure_propagates(self):
    os.makedirs(self.out_dir)
    with self.assertRaises(OSError):
      foreman.generate_summary(
          self.out_dir,
          "s",
          self.lane,
          "b",
          "base",
          git_bin=os.path.join(self.root, "no-such-git"),
      )

  def test_read_usage_failure_is_reported(self):
    text = foreman.read_usage(
        os.path.join(self.root, "no-such-agy"), self.lane
    )
    self.assertIn("usage meter failed", text)

  def test_default_out_dir(self):
    self.assertEqual(
        foreman.default_out_dir(
            "25.33", environ={"DCFS_FOREMAN_DIR": "/custom"}
        ),
        "/custom/25.33",
    )

  def _run(self, step="tok-step", env=None, **kwargs):
    environ = {
        "DCFS_MAIN_REPO": self.main_repo,
        "DCFS_DASHBOARD_DIR": self.dashboard,
    }
    environ.update(env or {})
    return foreman.run_foreman(
        step=step,
        lane=self.lane,
        branch="step-" + step,
        prompt_file=self.prompt_file,
        out_dir=self.out_dir,
        agy_bin=self.fake_agy_path,
        environ=environ,
        **kwargs,
    )

  def _tokens(self):
    with open(
        os.path.join(self.out_dir, "tokens.json"), "r", encoding="utf-8"
    ) as f:
      return json.load(f)

  def _ledger_rows(self):
    path = os.path.join(self.main_repo, "docs", "plan", "dispatch-ledger.tsv")
    with open(path, "r", encoding="utf-8") as f:
      lines = f.read().splitlines()
    header = lines[0].split("\t")
    return [dict(zip(header, line.split("\t"))) for line in lines[1:]]

  def test_tokens_json_records_each_session(self):
    defects_file = os.path.join(self.root, "defects.txt")
    with open(defects_file, "w", encoding="utf-8") as f:
      f.write("foo.cc:10 \u2014 style \u2014 bad \u2014 fix it")
    self.assertEqual(
        self._run(
            env={"FAKE_AGY_DEFECTS_FILE": defects_file}, model="m-work",
            review_model="m-rev",
        ),
        0,
    )
    tokens = self._tokens()
    roles = [(s["role"], s["name"], s["model"]) for s in tokens["sessions"]]
    self.assertEqual(
        roles,
        [
            ("worker", "worker", "m-work"),
            ("review", "review1", "m-rev"),
            ("fix", "fix1", "m-work"),
            ("review", "review2", "m-rev"),
        ],
    )
    worker = tokens["sessions"][0]["usage"]
    self.assertEqual(worker["input_tokens"], 2000)
    self.assertEqual(worker["output_tokens"], 200)
    self.assertEqual(worker["thinking_tokens"], 20)
    self.assertEqual(worker["cache_read_tokens"], 9000)
    self.assertEqual(worker["total_tokens"], 2200)
    self.assertEqual(tokens["sessions"][1]["usage"]["total_tokens"], 1100)
    self.assertEqual(tokens["total"]["total_tokens"], 2 * 2200 + 2 * 1100)
    self.assertEqual(tokens["total"]["input_tokens"], 2 * 2000 + 2 * 1000)
    self.assertIn("session total", tokens["usage_scope"])

  def test_summary_has_token_table(self):
    self.assertEqual(self._run(), 0)
    with open(
        os.path.join(self.out_dir, "summary.md"), "r", encoding="utf-8"
    ) as f:
      summary = f.read()
    self.assertIn("## tokens", summary)
    model = "gemini-3.8-flash-medium"
    self.assertIn(f"| worker | {model} | 2000 | 200 | 20 | 9000 | 2200 |",
                  summary)
    self.assertIn(f"| review | {model} | 1000 | 100 | 10 | 5000 | 1100 |",
                  summary)

  def test_failed_review_still_records_tokens(self):
    self.assertEqual(self._run(env={"FAKE_AGY_REVIEW_MODE": "error"}), 1)
    tokens = self._tokens()
    self.assertEqual(
        [s["role"] for s in tokens["sessions"]], ["worker", "review"]
    )

  def test_failed_worker_records_partial_usage(self):
    self.assertEqual(self._run(env={"FAKE_AGY_STREAM_MODE": "exit_early"}), 1)
    tokens = self._tokens()
    self.assertEqual([s["role"] for s in tokens["sessions"]], ["worker"])
    self.assertEqual(tokens["total"]["total_tokens"], 0)

  def test_one_ledger_row_per_run(self):
    self.assertEqual(self._run(step="25.99", model="gemini-3.8-flash-low"), 0)
    rows = self._ledger_rows()
    self.assertEqual(len(rows), 1)
    row = rows[0]
    self.assertEqual(row["step"], "25.99")
    self.assertEqual(row["agent"], "agy")
    self.assertEqual(row["model"], "gemini-3.8-flash")
    self.assertEqual(row["effort"], "low")
    self.assertEqual(row["tokens"], str(2200 + 1100))
    self.assertEqual(row["input_tokens"], str(2000 + 1000))
    self.assertEqual(row["output_tokens"], str(200 + 100))
    self.assertEqual(row["cache_read_tokens"], str(9000 + 5000))
    self.assertEqual(row["rounds"], "1")
    self.assertIn("mode=orchestrator", row["notes"])
    self.assertIn("worker=2200", row["notes"])
    self.assertIn("review1=1100", row["notes"])

  def test_ledger_row_written_when_run_fails(self):
    self.assertEqual(self._run(env={"FAKE_AGY_STREAM_MODE": "exit_early"}), 1)
    rows = self._ledger_rows()
    self.assertEqual(len(rows), 1)
    self.assertIn("failed", rows[0]["notes"])

  def test_single_session_gets_its_own_role(self):
    usage = foreman.parse_stream_usage(
        os.path.join(TESTDATA, "gemini_worker_excerpt.jsonl")
    )
    sessions = []
    foreman.add_session(sessions, "single", "single", "m", usage)
    tokens = foreman.build_tokens(sessions)
    self.assertEqual(tokens["sessions"][0]["role"], "single")
    self.assertEqual(tokens["total"]["total_tokens"], 243278)

  def test_parse_stream_usage_recorded_session(self):
    usage = foreman.parse_stream_usage(
        os.path.join(TESTDATA, "gemini_worker_excerpt.jsonl")
    )
    self.assertEqual(usage.model, "gemini-3.8-flash-medium")
    self.assertEqual(usage.counts["input_tokens"], 221527)
    self.assertEqual(usage.counts["output_tokens"], 21751)
    self.assertEqual(usage.counts["thinking_tokens"], 13429)
    self.assertEqual(usage.counts["cache_read_tokens"], 3319437)
    self.assertEqual(usage.counts["total_tokens"], 243278)
    self.assertEqual(usage.subagent_calls, 0)

  def test_parse_stream_usage_without_result_sums_steps(self):
    path = os.path.join(self.root, "partial.jsonl")
    with open(
        os.path.join(TESTDATA, "gemini_worker_excerpt.jsonl"), "r",
        encoding="utf-8",
    ) as src, open(path, "w", encoding="utf-8") as dst:
      dst.writelines(
          line for line in src if '"event": "result"' not in line
      )
    usage = foreman.parse_stream_usage(path)
    self.assertEqual(usage.counts["total_tokens"], 14276 + 16051 + 18195)
    self.assertEqual(usage.counts["thinking_tokens"], 575 + 28 + 16)

  def test_parse_stream_usage_counts_subagent_calls(self):
    path = os.path.join(self.root, "sub.jsonl")
    with open(path, "w", encoding="utf-8") as f:
      for name in ("invoke_subagent", "run_command", "browser_subagent"):
        f.write(json.dumps({
            "event": "step_update",
            "step_update": {"step_type": "tool", "tool_name": name},
        }) + "\n")
    self.assertEqual(foreman.parse_stream_usage(path).subagent_calls, 2)

  def test_parse_review_json_error_recording(self):
    with open(
        os.path.join(TESTDATA, "agy_review_error.json"), "r",
        encoding="utf-8",
    ) as f:
      parsed = foreman.parse_review_json(f.read())
    self.assertEqual(parsed["status"], "ERROR")
    self.assertEqual(parsed["usage"]["total_tokens"], 0)

  def test_parse_review_json_ignores_stderr_noise(self):
    text = 'error: noise\n{"status":"SUCCESS","response":"NONE"}\n'
    self.assertEqual(foreman.parse_review_json(text)["response"], "NONE")
    self.assertIsNone(foreman.parse_review_json("not json\n"))

  def test_custom_models_passed(self):
    log_file = os.path.join(self.root, "agy_args.log")
    env = {
        "DCFS_MAIN_REPO": self.main_repo,
        "DCFS_DASHBOARD_DIR": self.dashboard,
        "FAKE_AGY_LOG": log_file,
    }
    ret = foreman.run_foreman(
        step="model-step",
        lane=self.lane,
        branch="step-model",
        prompt_file=self.prompt_file,
        out_dir=self.out_dir,
        model="worker-model-x",
        review_model="review-model-y",
        agy_bin=self.fake_agy_path,
        environ=env,
    )
    self.assertEqual(ret, 0)

    with open(log_file, "r", encoding="utf-8") as f:
      calls = [json.loads(line) for line in f]

    worker_calls = [c for c in calls if "stream-json" in c]
    self.assertTrue(worker_calls)
    self.assertIn("worker-model-x", worker_calls[0])

    review_calls = [c for c in calls if "plan" in c]
    self.assertTrue(review_calls)
    self.assertIn("review-model-y", review_calls[0])


if __name__ == "__main__":
  unittest.main()
