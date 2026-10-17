# SPDX-License-Identifier: MIT
"""Tests for dashboard_lanes: hook events to the lanes file.

Every test runs on temporary directories standing in for the dashboard
directory, the lanes root, the agent definitions and /proc.
"""

import io
import json
import os
import subprocess
import tempfile
import unittest

import dashboard_lanes

AGENT_DEF = (
    "---\nname: dcfs-implementer\nmodel: sonnet\neffort: medium\n---\nbody\n"
)


class Fixture(unittest.TestCase):
    """Not a test case by itself: the temp directories the tests share."""

    def setUp(self):
        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        root = tmp.name
        self.dash = os.path.join(root, "dash")
        self.lanes = os.path.join(root, "lanes")
        self.agents = os.path.join(root, "agents")
        self.proc = os.path.join(root, "proc")
        for d in (self.lanes, self.agents, self.proc):
            os.makedirs(d)
        self.write(os.path.join(self.agents, "dcfs-implementer.md"), AGENT_DEF)
        self.write(os.path.join(self.agents, "dcfs-reviewer.md"),
                   "---\nname: dcfs-reviewer\nmodel: opus\neffort: high\n"
                   "---\nbody\n")
        self.make_lane(1, "step-25.9b")
        self.make_lane(2, "main")
        self.main_repo = os.path.join(root, "dircachefs")
        os.makedirs(self.main_repo)
        subprocess.check_call(
            ["git", "init", "-q", "-b", "main"], cwd=self.main_repo)
        subprocess.check_call(
            ["git", "config", "user.email", "test@example.com"],
            cwd=self.main_repo)
        subprocess.check_call(
            ["git", "config", "user.name", "test"], cwd=self.main_repo)
        self.write(os.path.join(self.main_repo, "README.md"), "hello\n")
        subprocess.check_call(["git", "add", "."], cwd=self.main_repo)
        subprocess.check_call(
            ["git", "commit", "-q", "-m", "init"], cwd=self.main_repo)
        self.env = dashboard_lanes.Env(
            dashboard_dir=self.dash,
            lanes_root=self.lanes,
            agents_dir=self.agents,
            proc_root=self.proc,
            main_repo=self.main_repo,
        )

    def write(self, path, text):
        with open(path, "w", encoding="utf-8") as f:
            f.write(text)

    def commit_file(self, relpath, text):
        full = os.path.join(self.main_repo, relpath)
        os.makedirs(os.path.dirname(full), exist_ok=True)
        self.write(full, text)
        subprocess.check_call(["git", "add", relpath], cwd=self.main_repo)
        subprocess.check_call(
            ["git", "commit", "-q", "-m", "add %s" % relpath],
            cwd=self.main_repo)

    def make_lane(self, n, branch):
        lane = os.path.join(self.lanes, "lane-%d" % n)
        os.makedirs(lane)
        # A linked worktree: .git is a file naming the git directory.
        gitdir = os.path.join(self.lanes, "gitdir-%d" % n)
        os.makedirs(gitdir)
        head = "ref: refs/heads/%s\n" % branch
        self.write(os.path.join(gitdir, "HEAD"), head)
        self.write(os.path.join(lane, ".git"), "gitdir: %s\n" % gitdir)

    def make_agy(self, pid, lane, args):
        d = os.path.join(self.proc, str(pid))
        os.makedirs(d)
        os.symlink(os.path.join(self.lanes, lane), os.path.join(d, "cwd"))
        argv = ["/usr/bin/agy"] + args
        with open(os.path.join(d, "cmdline"), "wb") as f:
            f.write(b"\0".join(a.encode() for a in argv) + b"\0")

    def run_hook(self, sub, payload):
        out, err = io.StringIO(), io.StringIO()
        text = payload if isinstance(payload, str) else json.dumps(payload)
        code = dashboard_lanes.main(
            [sub], stdin=io.StringIO(text), stdout=out, stderr=err, env=self.env
        )
        self.assertEqual(code, 0)
        return out.getvalue(), err.getvalue()

    def rows(self):
        with open(os.path.join(self.dash, "lanes.json"),
                  encoding="utf-8") as f:
            return {r["lane"]: r for r in json.load(f)}

    def sloc_rows(self):
        with open(os.path.join(self.dash, "sloc.json"),
                  encoding="utf-8") as f:
            return json.load(f)

    def launch(self, agent_id="a1", status="async_launched", **extra):
        prompt = extra.pop("prompt", "in ~/Sources/dircachefs-lanes/lane-2")
        tool_input = {
            "prompt": prompt,
            "description": "do thing",
            "subagent_type": "dcfs-implementer",
        }
        tool_input.update(extra)
        return self.run_hook("post-agent", {
            "tool_input": tool_input,
            "tool_response": {"status": status, "agentId": agent_id},
        })


class PostAgentTest(Fixture):

    def test_launch_then_stop_round_trip(self):
        out, _ = self.launch()
        context = json.loads(out)["hookSpecificOutput"]
        self.assertIn("additionalContext", context)
        row = self.rows()["lane-2"]
        self.assertEqual(row["state"], "running")
        self.assertEqual(row["agent"], "dcfs-implementer")
        self.assertEqual(row["model"], "sonnet")
        self.assertEqual(row["effort"], "medium")
        self.assertEqual(row["step"], "main")  # any branch names the step
        out, _ = self.run_hook("subagent-stop", {"agent_id": "a1"})
        self.assertEqual(out, "")
        row = self.rows()["lane-2"]
        self.assertEqual(row["state"], "idle")
        self.assertEqual(row["agent"], "")

    def test_orchestrator_row_and_idle_lanes(self):
        self.launch()
        rows = self.rows()
        self.assertEqual(rows["orchestrator"]["model"], "Claude Opus 5.5")
        self.assertEqual(rows["lane-1"]["state"], "idle")
        self.assertEqual(
            list(rows), ["orchestrator", "lane-1", "lane-2"])

    def test_two_agents_on_one_lane_share_its_row(self):
        self.launch()
        self.launch(agent_id="a2", subagent_type="dcfs-reviewer")
        row = self.rows()["lane-2"]
        self.assertEqual(row["state"], "running")
        self.assertEqual(row["step"], "main")
        self.assertEqual(row["agent"], "dcfs-implementer, dcfs-reviewer")
        self.assertEqual(row["model"], "sonnet, opus")
        self.assertEqual(row["effort"], "medium, high")

    def test_stopping_one_of_two_shows_the_rest(self):
        self.launch()
        self.launch(agent_id="a2", subagent_type="dcfs-reviewer")
        self.run_hook("subagent-stop", {"agent_id": "a1"})
        row = self.rows()["lane-2"]
        self.assertEqual(row["state"], "running")
        self.assertEqual(row["agent"], "dcfs-reviewer")
        self.assertEqual((row["model"], row["effort"]), ("opus", "high"))

    def test_orchestrator_config(self):
        os.makedirs(self.dash)
        self.write(os.path.join(self.dash, "config.json"),
                   '{"model": "M", "effort": "E"}')
        self.launch()
        self.assertEqual(self.rows()["orchestrator"]["model"], "M")
        self.assertEqual(self.rows()["orchestrator"]["effort"], "E")

    def test_subagent_originated_call_is_ignored(self):
        out, err = self.run_hook("post-agent", {
            "agent_id": "parent",
            "tool_input": {"prompt": "x", "description": "d",
                           "subagent_type": "dcfs-implementer"},
            "tool_response": {"status": "async_launched", "agentId": "a1"},
        })
        self.assertEqual((out, err), ("", ""))
        self.assertFalse(os.path.exists(os.path.join(self.dash, "lanes.json")))

    def test_foreground_completion_records_nothing_running(self):
        self.launch(status="completed")
        self.assertEqual(self.rows()["lane-2"]["state"], "idle")

    def test_branch_names_the_step(self):
        self.launch(prompt="in /home/x/Sources/dircachefs-lanes/lane-1 please")
        row = self.rows()["lane-1"]
        self.assertEqual(row["step"], "25.9b")
        self.assertEqual(row["state"], "running")

    def test_agent_without_lane_is_recorded_but_not_shown(self):
        self.launch(prompt="no lane here")
        rows = self.rows()
        del rows["orchestrator"]
        self.assertEqual({r["state"] for r in rows.values()}, {"idle"})
        with open(os.path.join(self.dash, "running.json")) as f:
            self.assertIn("a1", json.load(f))

    def test_model_fallback_order(self):
        self.launch(model="opus")
        self.assertEqual(self.rows()["lane-2"]["model"], "opus")
        self.run_hook("subagent-stop", {"agent_id": "a1"})
        # resolvedModel beats the frontmatter.
        self.run_hook("post-agent", {
            "tool_input": {"prompt": "lane-2: dircachefs-lanes/lane-2",
                           "description": "d",
                           "subagent_type": "dcfs-implementer"},
            "tool_response": {"status": "async_launched", "agentId": "a2",
                              "resolvedModel": "claude-x"},
        })
        self.assertEqual(self.rows()["lane-2"]["model"], "claude-x")
        self.run_hook("subagent-stop", {"agent_id": "a2"})
        self.launch(agent_id="a3")
        self.assertEqual(self.rows()["lane-2"]["model"], "sonnet")

    def test_explicit_effort_wins_over_the_type_default(self):
        # dcfs-reviewer's frontmatter says high; the call passes medium.
        self.launch(subagent_type="dcfs-reviewer", effort="medium")
        self.assertEqual(self.rows()["lane-2"]["effort"], "medium")

    def test_unknown_agent_type_has_empty_model_and_effort(self):
        self.run_hook("post-agent", {
            "tool_input": {"prompt": "dircachefs-lanes/lane-2",
                           "description": "d", "subagent_type": "nosuch"},
            "tool_response": {"status": "async_launched", "agentId": "a1"},
        })
        row = self.rows()["lane-2"]
        self.assertEqual((row["model"], row["effort"]), ("", ""))

    def test_no_context_when_unchanged(self):
        self.launch()
        self.run_hook("mark-uploaded", {})
        out, _ = self.launch()  # same agent id: same table
        self.assertEqual(out, "")


class AgyTest(Fixture):

    def test_agy_process_maps_to_its_lane(self):
        self.make_agy(42, "lane-1", ["--model", "gemini-x"])
        self.make_agy(43, "lane-2", ["--model=gemini-y"])
        self.launch(prompt="none")
        rows = self.rows()
        self.assertEqual(rows["lane-1"]["agent"], "agy, sandboxed")
        self.assertEqual(rows["lane-1"]["model"], "gemini-x")
        self.assertEqual(rows["lane-1"]["state"], "running")
        self.assertEqual(rows["lane-2"]["model"], "gemini-y")

    def test_effort_suffix_moves_to_the_effort_column(self):
        self.make_agy(42, "lane-1", ["--model", "gemini-3.8-flash-high"])
        self.make_agy(43, "lane-2", ["--model=gemini-4-argon-medium"])
        self.launch(prompt="none")
        rows = self.rows()
        self.assertEqual(
            (rows["lane-1"]["model"], rows["lane-1"]["effort"]),
            ("gemini-3.8-flash", "high"))
        self.assertEqual(
            (rows["lane-2"]["model"], rows["lane-2"]["effort"]),
            ("gemini-4-argon", "medium"))

    def test_model_without_effort_suffix_keeps_empty_effort(self):
        self.make_agy(42, "lane-1", ["--model", "gemini-x"])
        self.launch(prompt="none")
        self.assertEqual(self.rows()["lane-1"]["effort"], "")

    def test_agy_row_step_is_the_branch(self):
        self.make_agy(42, "lane-1", ["--model", "gemini-x"])
        self.make_agy(43, "lane-2", [])
        self.launch(prompt="none")
        self.assertEqual(self.rows()["lane-1"]["step"], "25.9b")
        self.assertEqual(self.rows()["lane-2"]["step"], "main")

    def test_agy_row_on_detached_head_has_no_step(self):
        self.write(os.path.join(self.lanes, "gitdir-1", "HEAD"), "abc123\n")
        self.make_agy(42, "lane-1", [])
        self.launch(prompt="none")
        self.assertEqual(self.rows()["lane-1"]["step"], "")

    def test_other_processes_and_missing_model(self):
        d = os.path.join(self.proc, "7")
        os.makedirs(d)
        self.write(os.path.join(d, "cmdline"), "bash\0")
        self.make_agy(8, "lane-1", [])
        os.makedirs(os.path.join(self.proc, "self"))  # not a process
        self.launch(prompt="none")
        self.assertEqual(self.rows()["lane-1"]["model"], "")
        self.assertEqual(self.rows()["lane-2"]["state"], "idle")

    def test_model_as_the_last_argument(self):
        self.make_agy(42, "lane-1", ["--model"])
        self.launch(prompt="none")
        self.assertEqual(self.rows()["lane-1"]["model"], "")

    def test_process_that_exited_mid_scan(self):
        os.makedirs(os.path.join(self.proc, "9", "cmdline"))  # unreadable
        self.launch(prompt="none")
        self.assertEqual(self.rows()["lane-1"]["state"], "idle")

    def test_agy_is_appended_to_the_agents_on_its_lane(self):
        self.make_agy(42, "lane-2", ["--model", "gemini-x-high"])
        self.launch()
        self.launch(agent_id="a2", subagent_type="dcfs-reviewer")
        row = self.rows()["lane-2"]
        self.assertEqual(row["agent"],
                         "dcfs-implementer, dcfs-reviewer, agy, sandboxed")
        self.assertEqual(row["model"], "sonnet, opus, gemini-x")
        self.assertEqual(row["effort"], "medium, high, high")
        self.assertEqual(row["step"], "main")

    def test_agy_stays_when_the_agent_on_its_lane_stops(self):
        self.make_agy(42, "lane-2", ["--model", "gemini-x"])
        self.launch()
        self.run_hook("subagent-stop", {"agent_id": "a1"})
        row = self.rows()["lane-2"]
        self.assertEqual(row["agent"], "agy, sandboxed")
        self.assertEqual(row["state"], "running")


class BranchTest(Fixture):

    def test_non_step_branch_is_the_step(self):
        self.make_lane(3, "sweep-gemini")
        self.launch(prompt="dircachefs-lanes/lane-3")
        self.assertEqual(self.rows()["lane-3"]["step"], "sweep-gemini")

    def test_relative_gitdir_resolves_against_the_lane(self):
        self.write(os.path.join(self.lanes, "lane-1", ".git"),
                   "gitdir: ../gitdir-1\n")
        self.launch(prompt="dircachefs-lanes/lane-1")
        self.assertEqual(self.rows()["lane-1"]["step"], "25.9b")

    def test_detached_head_falls_back_to_the_description(self):
        self.write(os.path.join(self.lanes, "gitdir-1", "HEAD"), "abc123\n")
        self.launch(prompt="dircachefs-lanes/lane-1")
        self.assertEqual(self.rows()["lane-1"]["step"], "do thing")

    def test_unreadable_lane_gets_an_empty_row_not_a_failed_table(self):
        os.makedirs(os.path.join(self.lanes, "lane-3"))  # no .git
        self.launch(prompt="dircachefs-lanes/lane-3")
        self.assertEqual(self.rows()["lane-3"]["state"], "idle")
        self.assertEqual(self.rows()["lane-1"]["state"], "idle")


class PostMessageTest(Fixture):

    def message(self, to, **extra):
        return self.run_hook("post-message", {
            "tool_name": "SendMessage",
            "tool_input": dict({"to": to, "message": "go on"}, **extra),
        })

    def test_resumed_agent_runs_again(self):
        self.launch()
        self.run_hook("subagent-stop", {"agent_id": "a1"})
        self.run_hook("mark-uploaded", {})
        self.assertEqual(self.rows()["lane-2"]["state"], "idle")
        out, err = self.message("a1")
        self.assertEqual(err, "")
        context = json.loads(out)["hookSpecificOutput"]
        self.assertEqual(context["hookEventName"], "PostToolUse")
        self.assertIn("lanes.json", context["additionalContext"])
        row = self.rows()["lane-2"]
        self.assertEqual(row["state"], "running")
        self.assertEqual(row["agent"], "dcfs-implementer")

    def test_resumed_agent_stops_again(self):
        self.launch()
        self.run_hook("subagent-stop", {"agent_id": "a1"})
        self.message("a1")
        self.run_hook("subagent-stop", {"agent_id": "a1"})
        self.assertEqual(self.rows()["lane-2"]["state"], "idle")
        self.assertIn("additionalContext", self.message("a1")[0])

    def test_unknown_or_running_target_prints_nothing(self):
        self.launch()
        self.run_hook("mark-uploaded", {})
        self.assertEqual(self.message("nobody"), ("", ""))
        self.assertEqual(self.message("a1"), ("", ""))  # still running

    def test_unchanged_table_prints_nothing(self):
        self.launch(prompt="no lane here")
        self.run_hook("subagent-stop", {"agent_id": "a1"})
        self.run_hook("mark-uploaded", {})
        self.assertEqual(self.message("a1"), ("", ""))

    def test_subagent_originated_call_is_ignored(self):
        self.launch()
        self.run_hook("subagent-stop", {"agent_id": "a1"})
        out, err = self.run_hook("post-message", {
            "agent_id": "parent", "tool_input": {"to": "a1"}})
        self.assertEqual((out, err), ("", ""))
        self.assertEqual(self.rows()["lane-2"]["state"], "idle")

    def test_finished_is_capped_to_the_most_recent_50(self):
        for i in range(60):
            self.launch(agent_id="a%d" % i)
            self.run_hook("subagent-stop", {"agent_id": "a%d" % i})
        with open(os.path.join(self.dash, "finished.json")) as f:
            finished = json.load(f)
        self.assertEqual(len(finished), 50)
        self.assertNotIn("a9", finished)
        self.assertIn("a10", finished)
        self.assertIn("a59", finished)

    def test_message_without_target_prints_nothing(self):
        out, err = self.run_hook("post-message", {"tool_input": {}})
        self.assertEqual((out, err), ("", ""))

    def test_malformed_message_event(self):
        out, err = self.run_hook("post-message", {})
        self.assertEqual(out, "")
        self.assertEqual(len(err.splitlines()), 1)


class WriteAtomicTest(unittest.TestCase):

    def test_failed_write_leaves_no_temp_file(self):
        with tempfile.TemporaryDirectory() as d:
            with self.assertRaises(TypeError):
                dashboard_lanes.write_atomic(os.path.join(d, "f"), 123)
            self.assertEqual(os.listdir(d), [])


class StopTest(Fixture):

    def stop(self, **kw):
        payload = {"stop_hook_active": False}
        payload.update(kw)
        return self.run_hook("stop", payload)

    def test_blocks_once_changed_and_mark_uploaded_clears(self):
        out, _ = self.stop()
        decision = json.loads(out)
        self.assertEqual(decision["decision"], "block")
        self.assertIn("lanes.json", decision["reason"])
        self.run_hook("mark-uploaded", {})
        self.assertEqual(self.stop()[0], "")

    def test_never_blocks_when_stop_hook_active(self):
        self.assertEqual(self.stop(stop_hook_active=True)[0], "")

    def test_subagent_stop_is_ignored(self):
        self.assertEqual(self.stop(agent_id="a1")[0], "")
        self.assertFalse(os.path.exists(os.path.join(self.dash, "lanes.json")))

    def test_blocks_again_after_a_change(self):
        self.stop()
        self.run_hook("mark-uploaded", {})
        self.launch()
        self.assertEqual(json.loads(self.stop()[0])["decision"], "block")

    def test_show_prints_the_file(self):
        self.stop()
        out, _ = self.run_hook("show", {})
        with open(os.path.join(self.dash, "lanes.json")) as f:
            self.assertEqual(out, f.read())


class MalformedTest(Fixture):

    def check(self, sub, text):
        out, err = self.run_hook(sub, text)
        self.assertEqual(out, "")
        self.assertEqual(len(err.splitlines()), 1)

    def test_not_json(self):
        self.check("post-agent", "{nope")

    def test_not_an_object(self):
        self.check("stop", "[1]")

    def test_missing_fields(self):
        self.check("post-agent", {"tool_input": {}})
        self.check("subagent-stop", {})

    def test_unknown_agent_stop_is_not_an_error_and_regenerates(self):
        out, err = self.run_hook("subagent-stop", {"agent_id": "never"})
        self.assertEqual((out, err), ("", ""))
        self.assertIn("lane-1", self.rows())

    def test_state_file_of_the_wrong_type(self):
        os.makedirs(self.dash)
        self.write(os.path.join(self.dash, "running.json"), "[]")
        self.check("subagent-stop", {"agent_id": "a1"})

    def test_state_file_that_is_not_json(self):
        os.makedirs(self.dash)
        self.write(os.path.join(self.dash, "running.json"), "{nope")
        self.check("subagent-stop", {"agent_id": "a1"})

    def test_empty_stdin_is_an_empty_event(self):
        self.check("subagent-stop", "")

    def test_mark_uploaded_and_show_before_any_file(self):
        self.check("mark-uploaded", "")
        self.check("show", "")

    def test_show_does_not_read_stdin(self):
        class Tty:
            def read(self):
                raise AssertionError("show read stdin")

        self.run_hook("stop", {"stop_hook_active": False})
        out = io.StringIO()
        dashboard_lanes.main(["show"], stdin=Tty(), stdout=out,
                             stderr=io.StringIO(), env=self.env)
        self.assertIn("orchestrator", out.getvalue())

    def test_no_arguments(self):
        out, err = io.StringIO(), io.StringIO()
        code = dashboard_lanes.main(
            [], stdin=io.StringIO(""), stdout=out, stderr=err, env=self.env)
        self.assertEqual((code, out.getvalue()), (0, ""))
        self.assertEqual(len(err.getvalue().splitlines()), 1)

    def test_lock_file_is_taken_in_the_dashboard_dir(self):
        self.run_hook("stop", {})
        self.assertTrue(os.path.exists(os.path.join(self.dash, "lock")))

    def test_unknown_subcommand(self):
        out, err = io.StringIO(), io.StringIO()
        code = dashboard_lanes.main(
            ["bogus"], stdin=io.StringIO("{}"), stdout=out, stderr=err,
            env=self.env)
        self.assertEqual((code, out.getvalue()), (0, ""))
        self.assertEqual(len(err.getvalue().splitlines()), 1)


class EnvFromEnvironTest(unittest.TestCase):

    def test_dashboard_dir_from_environment(self):
        env = dashboard_lanes.Env.from_environ({"DCFS_DASHBOARD_DIR": "/x"})
        self.assertEqual(env.dashboard_dir, "/x")

    def test_default_dashboard_dir(self):
        env = dashboard_lanes.Env.from_environ({"HOME": "/h"})
        self.assertEqual(env.dashboard_dir, "/h/.cache/dcfs-dashboard")
        self.assertEqual(env.lanes_root, "/h/Sources/dircachefs-lanes")

    def test_main_repo_from_environment(self):
        env = dashboard_lanes.Env.from_environ({"DCFS_MAIN_REPO": "/repo"})
        self.assertEqual(env.main_repo, "/repo")


class CountingRulesTest(unittest.TestCase):

    def count(self, text, lang):
        return dashboard_lanes.count_sloc(text.splitlines(), lang)

    def test_cpp_counting(self):
        src = (
            "// comment only\n"
            "\n"
            "/* block comment\n"
            "   still block\n"
            "*/\n"
            "int a = 1;\n"
            "int b = 2; // inline comment\n"
            "/* single line block */\n"
            "int c = 3; /* inline block */\n"
        )
        self.assertEqual(self.count(src, "C++"), 3)

    def test_c_counting(self):
        src = (
            "/* header block */\n"
            "#include <stdio.h>\n"
            "// line comment\n"
            "int main(void) {\n"
            "    return 0;\n"
            "}\n"
        )
        self.assertEqual(self.count(src, "C"), 4)

    def test_clang_query_counting(self):
        src = (
            "// A matcher\n"
            "match varDecl().bind(\"v\")\n"
            "/* block comment */\n"
        )
        self.assertEqual(self.count(src, "clang-query"), 1)

    def test_tla_counting(self):
        src = (
            "\\* Line comment\n"
            "(* Boxed comment\n"
            "   more comment *)\n"
            "---------------- MODULE test ----------------\n"
            "EXTENDS Naturals\n"
            "VARIABLE x\n"
            "Init == x = 0\n"
            "=============================================\n"
        )
        self.assertEqual(self.count(src, "TLA+"), 5)

    def test_shell_counting(self):
        src = (
            "#!/bin/bash\n"
            "# Comment line\n"
            "\n"
            "set -euo pipefail\n"
            "echo 'hello' # inline comment\n"
        )
        self.assertEqual(self.count(src, "shell"), 2)

    def test_python_counting(self):
        src = (
            "# License header\n"
            "import os\n"
            "\n"
            "# Function doc\n"
            "def foo():\n"
            "    return 1\n"
        )
        self.assertEqual(self.count(src, "Python"), 3)

    def test_starlark_counting(self):
        src = (
            "# BUILD file\n"
            "load(\":r.bzl\", \"r\")\n"
            "\n"
            "r(name = \"x\")\n"
        )
        self.assertEqual(self.count(src, "Starlark"), 2)


class AreaRulesTest(unittest.TestCase):

    def classify(self, path):
        return dashboard_lanes.classify_file(path)

    def test_prod_area(self):
        self.assertEqual(self.classify("dcfs/main.cc"), ("prod", "C++"))
        self.assertEqual(self.classify("dcfs/backing.h"), ("prod", "C++"))
        self.assertEqual(
            self.classify("dcfs/BUILD.bazel"), ("prod", "Starlark")
        )

    def test_test_area(self):
        self.assertEqual(
            self.classify("dcfs/testonly/fake.cc"), ("test", "C++")
        )
        self.assertEqual(self.classify("dcfs/backing_test.cc"), ("test", "C++"))
        self.assertEqual(
            self.classify("dcfs/test_helper_test.h"), ("test", "C++")
        )
        self.assertEqual(
            self.classify("dcfs/script_test.py"), ("test", "Python")
        )
        self.assertEqual(self.classify("dcfs/guest_test.sh"), ("test", "shell"))
        self.assertEqual(
            self.classify("test/qemu/guest/init"), ("test", "shell")
        )
        self.assertEqual(self.classify("test/qemu/run.sh"), ("test", "shell"))
        self.assertEqual(
            self.classify("test/qemu/BUILD.bazel"), ("test", "Starlark")
        )

    def test_model_area(self):
        self.assertEqual(self.classify("formal/dcfs.tla"), ("model", "TLA+"))
        self.assertEqual(
            self.classify("formal/BUILD.bazel"), ("model", "Starlark")
        )

    def test_tools_area(self):
        self.assertEqual(
            self.classify("tools/agents/dashboard_lanes.py"),
            ("tools", "Python"),
        )
        self.assertEqual(self.classify("bench/bench.cc"), ("tools", "C++"))
        self.assertEqual(
            self.classify("tools/style_matchers/rule.query"),
            ("tools", "clang-query"),
        )
        self.assertEqual(self.classify("tools/testutil.c"), ("tools", "C"))

    def test_skipped_files(self):
        self.assertEqual(self.classify("README.md"), ("tools", None))
        self.assertEqual(self.classify("dcfs/doc.txt"), ("prod", None))
        self.assertEqual(self.classify("MODULE.bazel.lock"), ("tools", None))


class HeadChangeTriggerTest(Fixture):

    def test_sloc_regenerated_when_head_changes(self):
        self.commit_file("dcfs/foo.cc", "int x = 1;\n")
        self.run_hook("stop", {"stop_hook_active": False})
        rows1 = self.sloc_rows()
        prod_row1 = next(
            r for r in rows1 if r["area"] == "prod" and r["language"] == "C++"
        )
        self.assertEqual(prod_row1["sloc"], 1)

        # Stop again without commits: does not regenerate, sloc unchanged.
        self.run_hook("stop", {"stop_hook_active": False})
        self.assertEqual(self.sloc_rows(), rows1)

        # Add commit changing main HEAD: stop regenerates with new count.
        self.commit_file("dcfs/foo.cc", "int x = 1;\nint y = 2;\n")
        self.run_hook("stop", {"stop_hook_active": False})
        rows2 = self.sloc_rows()
        prod_row2 = next(
            r for r in rows2 if r["area"] == "prod" and r["language"] == "C++"
        )
        self.assertEqual(prod_row2["sloc"], 2)

    def test_sloc_rows_order_and_format(self):
        self.commit_file("dcfs/a.cc", "int a = 1;\nint b = 2;\n")
        self.commit_file("dcfs/BUILD.bazel", "# comment\ncall()\n")
        self.commit_file("test/t.sh", "echo 1\necho 2\necho 3\n")
        self.commit_file("formal/m.tla", "Init == TRUE\n")
        self.commit_file("tools/s.py", "x = 1\n")
        self.run_hook("stop", {"stop_hook_active": False})
        rows = self.sloc_rows()
        for r in rows:
            self.assertEqual(set(r.keys()), {"area", "language", "sloc"})

        # The last 4 rows must be the 'all' rows in order.
        all_rows = rows[-4:]
        self.assertEqual(
            [r["area"] for r in all_rows],
            ["prod", "test", "model", "tools"],
        )
        self.assertEqual({r["language"] for r in all_rows}, {"all"})

        # Detail rows grouped by area in order: prod, test, model, tools.
        detail_rows = rows[:-4]
        seen_areas = []
        for r in detail_rows:
            if not seen_areas or seen_areas[-1] != r["area"]:
                seen_areas.append(r["area"])
        self.assertEqual(seen_areas, ["prod", "test", "model", "tools"])

        # In prod, C++ (2) comes before Starlark (1).
        prod_langs = [
            r["language"] for r in detail_rows if r["area"] == "prod"
        ]
        self.assertEqual(prod_langs, ["C++", "Starlark"])


class StopMessageTest(Fixture):

    def test_stop_message_includes_sloc_and_lanes(self):
        # Initial stop with both lanes and sloc changed.
        self.commit_file("dcfs/foo.cc", "int x = 1;\n")
        out, _ = self.run_hook("stop", {"stop_hook_active": False})
        decision = json.loads(out)
        self.assertEqual(decision["decision"], "block")
        reason = decision["reason"]
        self.assertIn("lanes.json", reason)
        self.assertIn("sloc.json", reason)
        self.assertIn("Dashboard lanes and sloc changed: upload", reason)

        # Mark uploaded clears both.
        self.run_hook("mark-uploaded", {})
        out, _ = self.run_hook("stop", {"stop_hook_active": False})
        self.assertEqual(out, "")

        # When only lanes changed (sloc unchanged).
        self.launch()
        out, _ = self.run_hook("stop", {"stop_hook_active": False})
        decision = json.loads(out)
        self.assertEqual(decision["decision"], "block")
        self.assertIn("lanes.json", decision["reason"])
        self.assertNotIn("sloc.json", decision["reason"])
        self.run_hook("mark-uploaded", {})

        # When only sloc changed (lanes unchanged).
        self.commit_file("dcfs/foo.cc", "int x = 1;\nint y = 2;\n")
        out, _ = self.run_hook("stop", {"stop_hook_active": False})
        decision = json.loads(out)
        self.assertEqual(decision["decision"], "block")
        self.assertIn("sloc.json", decision["reason"])
        self.assertNotIn("lanes.json", decision["reason"])
        self.assertIn("Dashboard sloc changed: upload", decision["reason"])


if __name__ == "__main__":
    unittest.main()
