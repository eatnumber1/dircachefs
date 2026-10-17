# SPDX-License-Identifier: MIT
"""Unit tests for gemini_watchdog (Gemini subagent liveness monitor)."""

import json
import os
import tempfile
import time
import unittest

import gemini_watchdog


class GeminiWatchdogTest(unittest.TestCase):

  def setUp(self):
    self.temp_dir = tempfile.TemporaryDirectory()
    self.brain_dir = os.path.join(self.temp_dir.name, "brain")
    os.makedirs(self.brain_dir, exist_ok=True)
    self.parent_id = "parent-1234"
    self.parent_dir = os.path.join(self.brain_dir, self.parent_id,
                                   ".system_generated", "logs")
    os.makedirs(self.parent_dir, exist_ok=True)

  def tearDown(self):
    self.temp_dir.cleanup()

  def test_discover_subagents(self):
    parent_log = os.path.join(self.parent_dir, "transcript.jsonl")
    subagent_id = "subagent-5678"
    entry = {
        "step_index": 1,
        "source": "MODEL",
        "type": "GENERIC",
        "content": json.dumps({
            "conversationId": subagent_id,
            "role": "Implementer",
            "typeName": "dcfs-implementer",
        }),
        "tool_calls": [{
            "name": "invoke_subagent",
            "args": {
                "Subagents": [{
                    "Role": "Implementer",
                    "TypeName": "dcfs-implementer",
                }]
            },
        }],
    }
    with open(parent_log, "w", encoding="utf-8") as f:
      f.write(json.dumps(entry) + "\n")

    agents = gemini_watchdog.discover_subagents(self.brain_dir, self.parent_id)
    self.assertEqual(len(agents), 1)
    self.assertEqual(agents[0]["id"], subagent_id)
    self.assertEqual(agents[0]["role"], "Implementer")

  def test_inspect_subagent_running(self):
    subagent_id = "subagent-5678"
    subagent_dir = os.path.join(self.brain_dir, subagent_id,
                                ".system_generated", "logs")
    os.makedirs(subagent_dir, exist_ok=True)
    subagent_log = os.path.join(subagent_dir, "transcript.jsonl")
    now_iso = time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime())
    entry = {
        "step_index": 5,
        "source": "MODEL",
        "type": "PLANNER_RESPONSE",
        "status": "DONE",
        "created_at": now_iso,
        "tool_calls": [{
            "name": "run_command",
            "args": {"toolAction": "Compiling source"},
        }],
    }
    with open(subagent_log, "w", encoding="utf-8") as f:
      f.write(json.dumps(entry) + "\n")

    status = gemini_watchdog.inspect_subagent(
        self.brain_dir, subagent_id, now=time.time())
    self.assertEqual(status["state"], "running")
    self.assertEqual(status["step_index"], 5)
    self.assertIn("Compiling source", status["detail"])

  def test_detect_stalled_agent(self):
    subagent_id = "subagent-5678"
    subagent_dir = os.path.join(self.brain_dir, subagent_id,
                                ".system_generated", "logs")
    os.makedirs(subagent_dir, exist_ok=True)
    subagent_log = os.path.join(subagent_dir, "transcript.jsonl")
    old_iso = "2026-10-12T12:00:00Z"
    entry = {
        "step_index": 5,
        "source": "MODEL",
        "type": "PLANNER_RESPONSE",
        "status": "DONE",
        "tool_calls": [{
            "name": "run_command",
            "args": {"CommandLine": "bazel test //..."},
        }],
        "created_at": old_iso,
    }
    with open(subagent_log, "w", encoding="utf-8") as f:
      f.write(json.dumps(entry) + "\n")

    # Current time is 1 hour later -> stall
    now_epoch = 1791810000.0  # arbitrary future timestamp
    status = gemini_watchdog.inspect_subagent(
        self.brain_dir, subagent_id, now=now_epoch, stall_threshold_sec=300
    )
    self.assertEqual(status["state"], "stalled")

  def test_detect_idle_agent(self):
    subagent_id = "subagent-idle"
    subagent_dir = os.path.join(self.brain_dir, subagent_id,
                                ".system_generated", "logs")
    os.makedirs(subagent_dir, exist_ok=True)
    subagent_log = os.path.join(subagent_dir, "transcript.jsonl")
    old_iso = "2026-10-12T12:00:00Z"
    entry = {
        "step_index": 5,
        "source": "MODEL",
        "type": "PLANNER_RESPONSE",
        "status": "DONE",
        "created_at": old_iso,
    }
    with open(subagent_log, "w", encoding="utf-8") as f:
      f.write(json.dumps(entry) + "\n")

    now_epoch = 1791810000.0
    status = gemini_watchdog.inspect_subagent(
        self.brain_dir, subagent_id, now=now_epoch, stall_threshold_sec=300
    )
    self.assertEqual(status["state"], "idle")

  def test_inspect_subagent_with_active_background_task(self):
    subagent_id = "subagent-9999"
    subagent_dir = os.path.join(self.brain_dir, subagent_id,
                                ".system_generated", "logs")
    os.makedirs(subagent_dir, exist_ok=True)
    subagent_log = os.path.join(subagent_dir, "transcript.jsonl")
    old_iso = "2026-10-12T12:00:00Z"
    entry = {
        "step_index": 10,
        "source": "MODEL",
        "type": "PLANNER_RESPONSE",
        "status": "DONE",
        "created_at": old_iso,
    }
    with open(subagent_log, "w", encoding="utf-8") as f:
      f.write(json.dumps(entry) + "\n")

    # Create background task directory with a recent log file
    tasks_dir = os.path.join(self.brain_dir, subagent_id,
                             ".system_generated", "tasks")
    os.makedirs(tasks_dir, exist_ok=True)
    task_log = os.path.join(tasks_dir, "task-1.log")
    with open(task_log, "w", encoding="utf-8") as f:
      f.write("[100/100] Compiling binary\n")

    # Inspect with now close to task file mtime
    task_mtime = os.stat(task_log).st_mtime
    status = gemini_watchdog.inspect_subagent(
        self.brain_dir, subagent_id, now=task_mtime + 5, stall_threshold_sec=300
    )
    self.assertEqual(status["state"], "running")
    self.assertIn("Compiling binary", status["detail"])
    self.assertEqual(status["elapsed_sec"], 5)

  def test_discover_subagents_with_active_ids(self):
    agents = gemini_watchdog.discover_subagents(
        self.brain_dir, self.parent_id, active_ids=["agent-1", "agent-2"]
    )
    self.assertEqual(len(agents), 2)
    self.assertEqual({a["id"] for a in agents}, {"agent-1", "agent-2"})

  def test_default_status_file_uses_xdg_runtime_dir(self):
    self.assertEqual(
        gemini_watchdog.default_status_file(
            {"XDG_RUNTIME_DIR": "/run/user/1000"}),
        "/run/user/1000/dcfs_agent_status.md")

  def test_default_status_file_falls_back_to_tmp_dir(self):
    self.assertEqual(
        gemini_watchdog.default_status_file({}),
        os.path.join(tempfile.gettempdir(), "dcfs_agent_status.md"))


if __name__ == "__main__":
  unittest.main()
