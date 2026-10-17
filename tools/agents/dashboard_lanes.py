#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Keep the lanes table of the private dashboard current from hooks.

Claude Code hooks call this tool with the hook's JSON on stdin; it records
which subagents run on which lane, writes the lanes table to a file, and
tells the orchestrator when that file differs from the last uploaded copy.
It is driven by events only: no timers, no polling. See README.md in this
directory for the hook configuration.

    dashboard_lanes.py post-agent      PostToolUse hook, matcher "Agent"
    dashboard_lanes.py subagent-stop   SubagentStop hook
    dashboard_lanes.py stop            the main session's Stop hook
    dashboard_lanes.py mark-uploaded   record the current file as uploaded
    dashboard_lanes.py show            print the lanes file

State lives in $DCFS_DASHBOARD_DIR (default ~/.cache/dcfs-dashboard):
running.json, lanes.json, uploaded.sha256, and an optional config.json with
"model" and "effort" for the orchestrator row.
"""

import dataclasses
import hashlib
import json
import os
import re
import sys
import tempfile
import time
from typing import Callable, Optional

LANE_IN_PROMPT = re.compile(r"dircachefs-lanes/lane-(\d+)")
LANE_DIR = re.compile(r"lane-(\d+)$")
STEP_BRANCH = re.compile(r"step-(.+)$")


@dataclasses.dataclass
class Env:
    """The directories and the clock the tool works against."""

    dashboard_dir: str
    lanes_root: str
    agents_dir: str
    proc_root: str
    now: Callable[[], str]

    @classmethod
    def from_environ(cls, environ):
        home = environ.get("HOME", "/")
        return cls(
            dashboard_dir=environ.get(
                "DCFS_DASHBOARD_DIR",
                os.path.join(home, ".cache", "dcfs-dashboard"),
            ),
            lanes_root=os.path.join(home, "Sources", "dircachefs-lanes"),
            agents_dir=os.path.join(
                environ.get("CLAUDE_PROJECT_DIR", "."), ".claude", "agents"
            ),
            proc_root="/proc",
            now=lambda: time.strftime("%Y-%m-%dT%H:%M:%S%z"),
        )

    def path(self, name):
        return os.path.join(self.dashboard_dir, name)


def read_text(path):
    with open(path) as f:
        return f.read()


def write_atomic(path, text):
    directory = os.path.dirname(path)
    os.makedirs(directory, exist_ok=True)
    fd, tmp = tempfile.mkstemp(dir=directory)
    with os.fdopen(fd, "w") as f:
        f.write(text)
    os.replace(tmp, path)


def load_json(path, default):
    if not os.path.exists(path):
        return default
    return json.loads(read_text(path))


def frontmatter(env, subagent_type):
    """The key: value pairs of .claude/agents/<type>.md, or {}."""
    path = os.path.join(env.agents_dir, subagent_type + ".md")
    if not os.path.exists(path):
        return {}
    lines = read_text(path).split("\n")
    pairs = {}
    for line in lines[1 : lines.index("---", 1)]:
        key, _, value = line.partition(":")
        pairs[key.strip()] = value.strip()
    return pairs


def record_agent(env, event):
    tool_input = event["tool_input"]
    response = event["tool_response"]
    if response["status"] != "async_launched":
        return
    front = frontmatter(env, tool_input["subagent_type"])
    match = LANE_IN_PROMPT.search(tool_input["prompt"])
    running = load_json(env.path("running.json"), {})
    running[response["agentId"]] = {
        "agent_id": response["agentId"],
        "subagent_type": tool_input["subagent_type"],
        "description": tool_input["description"],
        "lane": "lane-" + match.group(1) if match else None,
        "model": tool_input.get("model")
        or response.get("resolvedModel")
        or front.get("model", ""),
        "effort": front.get("effort", ""),
        "started": env.now(),
    }
    write_atomic(env.path("running.json"), json.dumps(running, indent=2))


def lane_branch(lane_dir):
    """The branch checked out in a lane (a linked worktree), or ''."""
    dot_git = os.path.join(lane_dir, ".git")
    git_dir = dot_git
    if os.path.isfile(dot_git):
        git_dir = read_text(dot_git).split(":", 1)[1].strip()
    head = read_text(os.path.join(git_dir, "HEAD")).strip()
    return head.removeprefix("ref: refs/heads/")


def agy_processes(env):
    """Maps a working directory to the --model of an agy running there."""
    found = {}
    for pid in os.listdir(env.proc_root):
        cmdline = os.path.join(env.proc_root, pid, "cmdline")
        if not os.path.exists(cmdline):
            continue
        with open(cmdline, "rb") as f:
            argv = f.read().decode(errors="replace").split("\0")
        if os.path.basename(argv[0]) != "agy":
            continue
        model = ""
        for i, arg in enumerate(argv):
            if arg == "--model" and i + 1 < len(argv):
                model = argv[i + 1]
            elif arg.startswith("--model="):
                model = arg.removeprefix("--model=")
        cwd = os.path.realpath(os.path.join(env.proc_root, pid, "cwd"))
        found[cwd] = model
    return found


def build_rows(env):
    config = load_json(env.path("config.json"), {})
    rows = [
        {
            "lane": "orchestrator",
            "step": "oversight, review, merges",
            "agent": "main session",
            "model": config.get("model", "Claude Opus 5.5"),
            "effort": config.get("effort", "default"),
            "state": "active",
        }
    ]
    running = load_json(env.path("running.json"), {})
    by_lane = {}
    for record in running.values():  # later launches replace earlier ones
        by_lane[record["lane"]] = record
    agy = agy_processes(env)
    lanes = sorted(
        (int(m.group(1)), name)
        for name in os.listdir(env.lanes_root)
        if (m := LANE_DIR.match(name))
    )
    for _, name in lanes:
        lane_dir = os.path.join(env.lanes_root, name)
        row = {"lane": name, "step": "", "agent": "", "model": "",
               "effort": "", "state": "idle"}
        if name in by_lane:
            record = by_lane[name]
            step = STEP_BRANCH.match(lane_branch(lane_dir))
            row.update(
                step=step.group(1) if step else record["description"],
                agent=record["subagent_type"],
                model=record["model"],
                effort=record["effort"],
                state="running",
            )
        elif os.path.realpath(lane_dir) in agy:
            row.update(
                agent="agy, sandboxed",
                model=agy[os.path.realpath(lane_dir)],
                state="running",
            )
        rows.append(row)
    return rows


def regenerate(env):
    """Writes lanes.json; returns whether it differs from the uploaded copy."""
    text = json.dumps(build_rows(env), indent=2) + "\n"
    write_atomic(env.path("lanes.json"), text)
    uploaded = None
    if os.path.exists(env.path("uploaded.sha256")):
        uploaded = read_text(env.path("uploaded.sha256")).strip()
    return hashlib.sha256(text.encode()).hexdigest() != uploaded


def post_agent(env, event):
    if "agent_id" in event:
        return ""
    record_agent(env, event)
    if not regenerate(env):
        return ""
    path = env.path("lanes.json")
    return json.dumps({"hookSpecificOutput": {
        "hookEventName": "PostToolUse",
        "additionalContext": (
            "Dashboard lanes changed: upload %s and run "
            "dashboard_lanes.py mark-uploaded." % path),
    }})


def subagent_stop(env, event):
    running = load_json(env.path("running.json"), {})
    del running[event["agent_id"]]
    write_atomic(env.path("running.json"), json.dumps(running, indent=2))
    regenerate(env)
    return ""


def stop(env, event):
    if "agent_id" in event or event.get("stop_hook_active", False):
        return ""
    if not regenerate(env):
        return ""
    return json.dumps({"decision": "block", "reason": (
        "Dashboard lanes changed: upload %s to the dashboard's lanes "
        "dataset and run dashboard_lanes.py mark-uploaded."
        % env.path("lanes.json"))})


def mark_uploaded(env, event):
    digest = hashlib.sha256(read_text(env.path("lanes.json")).encode())
    write_atomic(env.path("uploaded.sha256"), digest.hexdigest() + "\n")
    return ""


def show(env, event):
    return read_text(env.path("lanes.json"))


COMMANDS = {
    "post-agent": post_agent,
    "subagent-stop": subagent_stop,
    "stop": stop,
    "mark-uploaded": mark_uploaded,
    "show": show,
}


def main(argv, stdin=sys.stdin, stdout=sys.stdout, stderr=sys.stderr,
         env: Optional[Env] = None):
    """Runs one subcommand; always returns 0.

    A hook that exits nonzero or crashes can wedge the Claude Code session
    (a Stop hook that errors keeps the session from finishing), and the
    dashboard is a convenience, so a malformed input or an unreadable state
    file is reported in one line on stderr and the hook succeeds silently.
    """
    env = env or Env.from_environ(os.environ)
    try:
        command = COMMANDS[argv[0]]
        event = json.loads(stdin.read() or "{}")
        if not isinstance(event, dict):
            raise ValueError("hook input is not a JSON object")
        stdout.write(command(env, event))
    except (KeyError, ValueError, TypeError, OSError, IndexError) as e:
        stderr.write("dashboard_lanes: %s: %r\n" % (argv[:1], e))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
