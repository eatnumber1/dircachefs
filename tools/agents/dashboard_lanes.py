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
import fcntl
import hashlib
import json
import os
import re
import sys
import tempfile

LANE_IN_PROMPT = re.compile(r"dircachefs-lanes/lane-(\d+)")
LANE_DIR = re.compile(r"lane-(\d+)$")
STEP_BRANCH = re.compile(r"step-(.+)$")


@dataclasses.dataclass
class Env:
    dashboard_dir: str
    lanes_root: str
    agents_dir: str
    proc_root: str

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
    try:
        with os.fdopen(fd, "w") as f:
            f.write(text)
        os.replace(tmp, path)
    except BaseException:
        if os.path.exists(tmp):
            os.unlink(tmp)
        raise


def load_json(path, default):
    if not os.path.exists(path):
        return default
    return json.loads(read_text(path))


def frontmatter(env, subagent_type):
    """The key: value pairs of .claude/agents/<type>.md, or {}.

    Parsed by hand: the tool is standard library only, and the frontmatter
    is flat `key: value` lines.
    """
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
        "subagent_type": tool_input["subagent_type"],
        "description": tool_input["description"],
        "lane": "lane-" + match.group(1) if match else None,
        "model": tool_input.get("model")
        or response.get("resolvedModel")
        or front.get("model", ""),
        "effort": front.get("effort", ""),
    }
    write_atomic(env.path("running.json"), json.dumps(running, indent=2))


def lane_branch(lane_dir):
    """The branch checked out in a lane (a linked worktree).

    A detached HEAD returns the commit hash, which matches no step branch.
    """
    dot_git = os.path.join(lane_dir, ".git")
    git_dir = dot_git
    if os.path.isfile(dot_git):
        gitdir = read_text(dot_git).partition(":")[2].strip()
        git_dir = os.path.join(lane_dir, gitdir)  # may be relative
    head = read_text(os.path.join(git_dir, "HEAD")).strip()
    return head.removeprefix("ref: refs/heads/")


def agy_processes(env):
    """Maps a working directory to the --model of an agy running there."""
    found = {}
    for pid in os.listdir(env.proc_root):
        try:
            with open(os.path.join(env.proc_root, pid, "cmdline"), "rb") as f:
                text = f.read().decode(errors="replace")
        except OSError:  # the process exited, or is not one (/proc/self)
            continue
        argv = text.rstrip("\0").split("\0")
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
        try:
            branch = lane_branch(lane_dir)
        except OSError:  # a lane that is not a worktree: show it empty
            rows.append(row)
            continue
        if name in by_lane:
            # A recorded agent wins over an agy process in the same lane.
            record = by_lane[name]
            step = STEP_BRANCH.match(branch)
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
    # An agent_id means a subagent made this call: its launches are not
    # lanes of the orchestrator.
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
    # Agents we never recorded stop too: foreground agents, subagents'
    # own launches, and agents launched before the hooks were installed.
    running.pop(event["agent_id"], None)
    write_atomic(env.path("running.json"), json.dumps(running, indent=2))
    regenerate(env)
    return ""


def stop(env, event):
    # A subagent's stop is not the session's. stop_hook_active means the
    # session is already continuing after our block: blocking again would
    # loop.
    if "agent_id" in event or event.get("stop_hook_active"):
        return ""
    if not regenerate(env):
        return ""
    return json.dumps({"decision": "block", "reason": (
        "Dashboard lanes changed: upload %s to the dashboard's lanes "
        "dataset and run dashboard_lanes.py mark-uploaded."
        % env.path("lanes.json"))})


def mark_uploaded(env):
    digest = hashlib.sha256(read_text(env.path("lanes.json")).encode())
    write_atomic(env.path("uploaded.sha256"), digest.hexdigest() + "\n")
    return ""


def show(env):
    return read_text(env.path("lanes.json"))


def lock(env):
    """Holds the dashboard lock until the returned file is closed.

    Hooks run concurrently (a subagent stopping while another launches); a
    blocking flock makes each command's read-modify-write whole, and it
    waits for the other hook's exit, not for a time.
    """
    os.makedirs(env.dashboard_dir, exist_ok=True)
    f = open(env.path("lock"), "w")
    fcntl.flock(f, fcntl.LOCK_EX)
    return f


# Hook commands take the hook's JSON event.
HOOKS = {"post-agent": post_agent, "subagent-stop": subagent_stop,
         "stop": stop}
# The others are run by hand and read no stdin.
MANUAL = {"mark-uploaded": mark_uploaded, "show": show}


# A table dispatch, not argparse: a hook must exit 0 with one line on
# stderr whatever it is given, and argparse exits 2 on a bad command line.
def main(argv, stdin=sys.stdin, stdout=sys.stdout, stderr=sys.stderr,
         env=None):
    """Runs one subcommand; always returns 0.

    A hook that exits nonzero or crashes can wedge the Claude Code session
    (a Stop hook that errors keeps the session from finishing), and the
    dashboard is a convenience, so any failure (malformed input, a state
    file of the wrong type or unreadable) is reported in one line on stderr
    and the hook succeeds silently.
    """
    env = env or Env.from_environ(os.environ)
    try:
        with lock(env):
            if argv[0] in MANUAL:
                stdout.write(MANUAL[argv[0]](env))
            else:
                event = json.loads(stdin.read() or "{}")
                if not isinstance(event, dict):
                    raise ValueError("hook input is not a JSON object")
                stdout.write(HOOKS[argv[0]](env, event))
    except Exception as e:
        stderr.write("dashboard_lanes: %s: %r\n" % (argv[0] if argv else "", e))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
