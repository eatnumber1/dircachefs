# SPDX-License-Identifier: MIT
"""Tests for dashboard_lanes: hook events to the lanes file.

Every test runs on temporary directories standing in for the dashboard
directory, the lanes root, the agent definitions and /proc.
"""

import io
import json
import os
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
        self.make_lane(1, "step-25.9b")
        self.make_lane(2, "main")
        self.env = dashboard_lanes.Env(
            dashboard_dir=self.dash,
            lanes_root=self.lanes,
            agents_dir=self.agents,
            proc_root=self.proc,
        )

    def write(self, path, text):
        with open(path, "w") as f:
            f.write(text)

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
        with open(os.path.join(self.dash, "lanes.json")) as f:
            return {r["lane"]: r for r in json.load(f)}

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

    def test_recorded_agent_wins_over_agy(self):
        self.make_agy(42, "lane-2", ["--model", "gemini-x"])
        self.launch()
        self.assertEqual(self.rows()["lane-2"]["agent"], "dcfs-implementer")


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

    def test_malformed_message_event(self):
        out, err = self.run_hook("post-message", {"tool_input": {}})
        self.assertEqual(out, "")
        self.assertEqual(len(err.splitlines()), 0)


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


if __name__ == "__main__":
    unittest.main()
