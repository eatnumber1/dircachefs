#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Keep the lanes table and code size panel of the dashboard current.

Claude Code hooks call this tool with the hook's JSON on stdin; it records
which subagents run on which lane, computes code size (SLOC) over the main
checkout, writes the dashboard files, and tells the orchestrator when they
differ from the last uploaded copies. It is driven by events only: no timers,
no polling. See README.md in this directory for the hook configuration.

    dashboard_lanes.py post-agent      PostToolUse hook, matcher "Agent"
    dashboard_lanes.py post-message    PostToolUse hook, matcher "SendMessage"
    dashboard_lanes.py subagent-stop   SubagentStop hook
    dashboard_lanes.py stop            the main session's Stop hook
    dashboard_lanes.py mark-uploaded   record the current file as uploaded
    dashboard_lanes.py show            print the lanes file
    dashboard_lanes.py ledger add ...  append row for unhooked agent job
    dashboard_lanes.py ledger set ...  fill outcome fields at merge time

State lives in $DCFS_DASHBOARD_DIR (default ~/.cache/dcfs-dashboard):
running.json, finished.json, lanes.json, uploaded.sha256, sloc.json,
sloc.head, sloc.uploaded.sha256, and an optional config.json with "model"
and "effort" for the orchestrator row. Dispatch ledger is kept in
docs/plan/dispatch-ledger.tsv under the main repository checkout.
"""

import dataclasses
import datetime
import fcntl
import hashlib
import json
import os
import re
import subprocess
import sys
import tempfile
import uuid

LANE_IN_PROMPT = re.compile(r"dircachefs-lanes/lane-(\d+)")
LANE_DIR = re.compile(r"lane-(\d+)$")
EFFORT_SUFFIX = re.compile(r"(.+)-(low|medium|high)$")
FINISHED_KEPT = 50
TEST_FILE = re.compile(r"_test\.(cc|h|py|sh)$")
SLOC_AREAS = ["prod", "test", "model", "tools"]

LEDGER_COLUMNS = [
    "id",
    "dispatched",
    "step",
    "task_class",
    "agent",
    "model",
    "effort",
    "tokens",
    "duration_s",
    "rounds",
    "blockers",
    "shoulds",
    "nits",
    "escapes",
    "verdict",
    "notes",
]
LEDGER_HEADER = "\t".join(LEDGER_COLUMNS) + "\n"

TASK_CLASSES = {
    "dcfs-mechanical": "mechanical",
    "dcfs-implementer": "implementation",
    "dcfs-investigator": "investigation",
    "dcfs-protocol": "protocol",
    "dcfs-reviewer": "review",
    "dcfs-style-reviewer": "style-review",
}


@dataclasses.dataclass
class Env:
    dashboard_dir: str
    lanes_root: str
    agents_dir: str
    proc_root: str
    main_repo: str = ""
    now: object = None
    stderr: object = sys.stderr

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
            main_repo=environ.get(
                "DCFS_MAIN_REPO",
                environ.get(
                    "DCFS_MAIN_CHECKOUT",
                    os.path.join(home, "Sources", "dircachefs"),
                ),
            ),
            now=lambda: datetime.datetime.now(
                datetime.timezone.utc
            ).strftime("%Y-%m-%dT%H:%M:%SZ"),
            stderr=sys.stderr,
        )

    def path(self, name):
        return os.path.join(self.dashboard_dir, name)

    def ledger_path(self):
        return os.path.join(self.main_repo, "docs", "plan",
                            "dispatch-ledger.tsv")

    def current_time(self):
        if self.now is not None:
            return self.now() if callable(self.now) else str(self.now)
        return datetime.datetime.now(
            datetime.timezone.utc
        ).strftime("%Y-%m-%dT%H:%M:%SZ")


def read_text(path):
    with open(path, encoding="utf-8") as f:
        return f.read()


def write_atomic(path, text):
    directory = os.path.dirname(path)
    os.makedirs(directory, exist_ok=True)
    fd, tmp = tempfile.mkstemp(dir=directory)
    try:
        with os.fdopen(fd, "w", encoding="utf-8") as f:
            f.write(text)
        os.replace(tmp, path)
    except BaseException:
        if os.path.exists(tmp):
            os.unlink(tmp)
        raise


def classify_file(path):
    """Returns (area, language) or (area, None) if skipped."""
    if path.startswith("dcfs/"):
        if path.startswith("dcfs/testonly/") or TEST_FILE.search(path):
            area = "test"
        else:
            area = "prod"
    elif path.startswith("test/"):
        area = "test"
    elif path.startswith("formal/"):
        area = "model"
    else:
        area = "tools"

    base = os.path.basename(path)
    if path.endswith((".cc", ".h")):
        lang = "C++"
    elif path.endswith(".c"):
        lang = "C"
    elif path.endswith(".sh") or path == "test/qemu/guest/init":
        lang = "shell"
    elif path.endswith(".py"):
        lang = "Python"
    elif (path.endswith(".bzl") or base == "BUILD"
          or base.startswith("BUILD.") or base == "MODULE.bazel"):
        lang = "Starlark"
    elif path.endswith(".tla"):
        lang = "TLA+"
    elif path.endswith(".query"):
        lang = "clang-query"
    else:
        lang = None

    return area, lang


def count_sloc(lines, lang):
    """Counts non-blank, non-comment lines for the given language."""
    if lang in ("C++", "C", "clang-query"):
        line_comment, block_start, block_end = "//", "/*", "*/"
    elif lang == "TLA+":
        line_comment, block_start, block_end = r"\*", "(*", "*)"
    else:
        line_comment, block_start, block_end = "#", None, None

    in_block = False
    count = 0
    for line in lines:
        s = line.strip()
        if not s:
            continue
        if in_block:
            if block_end and block_end in s:
                in_block = False
            continue
        if block_start and s.startswith(block_start):
            if not block_end or block_end not in s:
                in_block = True
            continue
        if s.startswith(line_comment):
            continue
        count += 1
    return count


def main_head(env):
    """The HEAD commit hex of main_repo, or None."""
    try:
        res = subprocess.run(
            ["git", "rev-parse", "HEAD"],
            cwd=env.main_repo,
            stdout=subprocess.PIPE,
            stderr=subprocess.DEVNULL,
            text=True,
            check=True,
        )
        return res.stdout.strip()
    except (OSError, subprocess.CalledProcessError):
        return None


def git_ls_files(repo_dir):
    res = subprocess.run(
        ["git", "ls-files", "-z"],
        cwd=repo_dir,
        stdout=subprocess.PIPE,
        check=True,
    )
    return [f for f in res.stdout.decode("utf-8").split("\0") if f]


def compute_sloc_rows(main_repo):
    """Computes code size rows {"area", "language", "sloc"}."""
    counts = {}
    area_totals = {area: 0 for area in SLOC_AREAS}
    for path in git_ls_files(main_repo):
        area, lang = classify_file(path)
        if not lang:
            continue
        full_path = os.path.join(main_repo, path)
        try:
            with open(full_path, encoding="utf-8", errors="replace") as f:
                lines = f.read().splitlines()
        except OSError:
            continue
        c = count_sloc(lines, lang)
        counts[(area, lang)] = counts.get((area, lang), 0) + c
        area_totals[area] += c

    rows = []
    for area in SLOC_AREAS:
        area_langs = [
            (lang, c)
            for (a, lang), c in counts.items()
            if a == area and c > 0
        ]
        area_langs.sort(key=lambda item: (-item[1], item[0]))
        for lang, c in area_langs:
            rows.append({"area": area, "language": lang, "sloc": c})

    for area in SLOC_AREAS:
        rows.append({
            "area": area,
            "language": "all",
            "sloc": area_totals[area],
        })
    return rows


def regenerate_sloc(env):
    """Regenerates sloc.json if main HEAD changed. Returns True if changed."""
    head = main_head(env)
    recorded_head = None
    if os.path.exists(env.path("sloc.head")):
        recorded_head = read_text(env.path("sloc.head")).strip()

    if head is not None and (
        head != recorded_head or not os.path.exists(env.path("sloc.json"))
    ):
        rows = compute_sloc_rows(env.main_repo)
        text = json.dumps(rows, indent=2) + "\n"
        write_atomic(env.path("sloc.json"), text)
        write_atomic(env.path("sloc.head"), head + "\n")

    if not os.path.exists(env.path("sloc.json")):
        return False
    current_digest = hashlib.sha256(
        read_text(env.path("sloc.json")).encode()
    ).hexdigest()
    uploaded = None
    if os.path.exists(env.path("sloc.uploaded.sha256")):
        uploaded = read_text(env.path("sloc.uploaded.sha256")).strip()
    return current_digest != uploaded


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
        "effort": tool_input.get("effort") or front.get("effort", ""),
    }
    write_atomic(env.path("running.json"), json.dumps(running, indent=2))


def lane_branch(lane_dir):
    """The branch checked out in a lane (a linked worktree).

    A detached HEAD returns None.
    """
    dot_git = os.path.join(lane_dir, ".git")
    git_dir = dot_git
    if os.path.isfile(dot_git):
        gitdir = read_text(dot_git).partition(":")[2].strip()
        git_dir = os.path.join(lane_dir, gitdir)  # may be relative
    head = read_text(os.path.join(git_dir, "HEAD")).strip()
    if not head.startswith("ref: refs/heads/"):
        return None
    return head.removeprefix("ref: refs/heads/")


def step_of(branch):
    return branch.removeprefix("step-")


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
    for record in running.values():  # launch order
        by_lane.setdefault(record["lane"], []).append(record)
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
        # The recorded agents on the lane, then an agy process there.
        records = by_lane.get(name, [])
        shown = [(r["subagent_type"], r["model"], r["effort"])
                 for r in records]
        if os.path.realpath(lane_dir) in agy:
            model = agy[os.path.realpath(lane_dir)]
            effort = EFFORT_SUFFIX.match(model)
            shown.append(("agy, sandboxed",
                          effort.group(1) if effort else model,
                          effort.group(2) if effort else ""))
        if shown:
            fallback = records[0]["description"] if records else ""
            row.update(
                step=step_of(branch) if branch else fallback,
                agent=", ".join(s[0] for s in shown),
                model=", ".join(s[1] for s in shown),
                effort=", ".join(s[2] for s in shown),
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


def upload_context(env):
    """The PostToolUse output asking for an upload, or "" if unchanged."""
    if not regenerate(env):
        return ""
    path = env.path("lanes.json")
    return json.dumps({"hookSpecificOutput": {
        "hookEventName": "PostToolUse",
        "additionalContext": (
            "Dashboard lanes changed: upload %s and run "
            "dashboard_lanes.py mark-uploaded." % path),
    }})


def read_ledger(path):
    """Reads ledger rows as list of dicts. If file missing, returns []."""
    if not os.path.exists(path):
        return []
    text = read_text(path)
    lines = text.splitlines()
    if not lines:
        return []
    header = lines[0].split("\t")
    rows = []
    for line in lines[1:]:
        if not line.strip():
            continue
        vals = line.split("\t")
        if len(vals) < len(header):
            vals.extend([""] * (len(header) - len(vals)))
        rows.append(dict(zip(header, vals)))
    return rows


def write_ledger(path, rows):
    """Writes ledger rows atomically with LEDGER_HEADER."""
    lines = [LEDGER_HEADER]
    for r in rows:
        lines.append(
            "\t".join(str(r.get(col, "")) for col in LEDGER_COLUMNS) + "\n"
        )
    write_atomic(path, "".join(lines))


def extract_tokens_and_duration(data):
    """Extracts (tokens_str, duration_str) from event/response dict."""
    tokens = ""
    duration = ""
    if "tokens" in data and data["tokens"] is not None:
        tokens = str(data["tokens"])
    elif "total_tokens" in data and data["total_tokens"] is not None:
        tokens = str(data["total_tokens"])
    elif "usage" in data and isinstance(data["usage"], dict):
        u = data["usage"]
        if "total_tokens" in u and u["total_tokens"] is not None:
            tokens = str(u["total_tokens"])
        elif "tokens" in u and u["tokens"] is not None:
            tokens = str(u["tokens"])
        elif "input_tokens" in u and "output_tokens" in u:
            tokens = str(u["input_tokens"] + u["output_tokens"])

    if "duration_s" in data and data["duration_s"] is not None:
        duration = str(data["duration_s"])
    elif "duration" in data and data["duration"] is not None:
        duration = str(data["duration"])
    elif "duration_seconds" in data and data["duration_seconds"] is not None:
        duration = str(data["duration_seconds"])

    return tokens, duration


def resolve_step(env, prompt, description):
    """Resolves step from lane branch or falls back to description."""
    match = LANE_IN_PROMPT.search(prompt)
    if match:
        lane_dir = os.path.join(env.lanes_root, "lane-" + match.group(1))
        try:
            branch = lane_branch(lane_dir)
            if branch:
                return step_of(branch)
        except OSError:
            pass
    return description


def record_ledger_dispatch(env, event):
    tool_input = event.get("tool_input", {})
    response = event.get("tool_response", {})
    agent_id = (response.get("agentId")
                or event.get("agent_id")
                or uuid.uuid4().hex[:8])
    subagent_type = tool_input.get("subagent_type", "")
    front = frontmatter(env, subagent_type)
    model = (tool_input.get("model")
             or response.get("resolvedModel")
             or front.get("model", ""))
    effort = tool_input.get("effort") or front.get("effort", "")
    step = resolve_step(env, tool_input.get("prompt", ""),
                        tool_input.get("description", ""))
    task_class = TASK_CLASSES.get(subagent_type, "")

    tokens, duration = "", ""
    if response.get("status") != "async_launched":
        tokens, duration = extract_tokens_and_duration(response)
        if not tokens and not duration:
            tokens, duration = extract_tokens_and_duration(event)

    row = {col: "" for col in LEDGER_COLUMNS}
    row.update({
        "id": agent_id,
        "dispatched": env.current_time(),
        "step": step,
        "task_class": task_class,
        "agent": subagent_type,
        "model": model,
        "effort": effort,
        "tokens": tokens,
        "duration_s": duration,
    })

    ledger_path = env.ledger_path()
    rows = read_ledger(ledger_path)
    rows.append(row)
    write_ledger(ledger_path, rows)


def record_ledger_stop(env, event):
    agent_id = event.get("agent_id")
    if not agent_id:
        return
    tokens, duration = extract_tokens_and_duration(event)
    if not tokens and not duration:
        return
    ledger_path = env.ledger_path()
    rows = read_ledger(ledger_path)
    updated = False
    for r in reversed(rows):
        if r.get("id") == agent_id:
            if tokens:
                r["tokens"] = tokens
            if duration:
                r["duration_s"] = duration
            updated = True
            break
    if updated:
        write_ledger(ledger_path, rows)


def parse_key_value_args(args):
    """Parses list of arguments as dict of key-value pairs."""
    kwargs = {}
    i = 0
    while i < len(args):
        arg = args[i]
        if arg.startswith("--"):
            key = arg[2:]
            if "=" in key:
                k, v = key.split("=", 1)
                kwargs[k.replace("-", "_")] = v
            elif i + 1 < len(args) and not args[i + 1].startswith("-"):
                kwargs[key.replace("-", "_")] = args[i + 1]
                i += 1
            else:
                kwargs[key.replace("-", "_")] = ""
        elif "=" in arg:
            k, v = arg.split("=", 1)
            kwargs[k.replace("-", "_")] = v
        else:
            if "id" not in kwargs:
                kwargs["id"] = arg
        i += 1
    return kwargs


def ledger_add(env, args):
    kwargs = parse_key_value_args(args)
    job_id = kwargs.get("id") or uuid.uuid4().hex[:8]
    agent = kwargs.get("agent", "agy")
    model = kwargs.get("model", "")
    effort = kwargs.get("effort", "")
    m = EFFORT_SUFFIX.match(model)
    if m:
        model = m.group(1)
        if not effort:
            effort = m.group(2)
    task_class = kwargs.get("task_class") or TASK_CLASSES.get(agent, "")

    row = {col: "" for col in LEDGER_COLUMNS}
    row.update({
        "id": job_id,
        "dispatched": kwargs.get("dispatched") or env.current_time(),
        "step": kwargs.get("step", ""),
        "task_class": task_class,
        "agent": agent,
        "model": model,
        "effort": effort,
        "tokens": kwargs.get("tokens", ""),
        "duration_s": kwargs.get("duration_s", ""),
        "rounds": kwargs.get("rounds", ""),
        "blockers": kwargs.get("blockers", ""),
        "shoulds": kwargs.get("shoulds", ""),
        "nits": kwargs.get("nits", ""),
        "escapes": kwargs.get("escapes", ""),
        "verdict": kwargs.get("verdict", ""),
        "notes": kwargs.get("notes", ""),
    })

    ledger_path = env.ledger_path()
    rows = read_ledger(ledger_path)
    rows.append(row)
    write_ledger(ledger_path, rows)
    return ""


def ledger_set(env, args):
    if len(args) < 2:
        raise ValueError("usage: ledger set ID field=value ...")
    job_id = args[0]
    updates = {}
    for arg in args[1:]:
        if "=" in arg:
            k, v = arg.split("=", 1)
            updates[k.strip().replace("-", "_")] = v.strip()
        else:
            raise ValueError("invalid field=value argument: %r" % arg)

    ledger_path = env.ledger_path()
    rows = read_ledger(ledger_path)
    updated = False
    for r in rows:
        if r.get("id") == job_id:
            for k, v in updates.items():
                if k in LEDGER_COLUMNS:
                    r[k] = v
            updated = True
            break
    if not updated:
        raise KeyError("ledger: no row with id %r" % job_id)
    write_ledger(ledger_path, rows)
    return ""


def ledger(env, args):
    if not args:
        raise ValueError("ledger requires a subcommand (add or set)")
    sub = args[0]
    if sub == "add":
        return ledger_add(env, args[1:])
    elif sub == "set":
        return ledger_set(env, args[1:])
    else:
        raise ValueError("unknown ledger subcommand: %r" % sub)


def post_agent(env, event):
    # An agent_id means a subagent made this call: its launches are not
    # lanes of the orchestrator.
    if "agent_id" in event:
        return ""
    record_agent(env, event)
    try:
        record_ledger_dispatch(env, event)
    except Exception as e:
        env.stderr.write("dashboard_lanes: ledger: %r\n" % e)
    return upload_context(env)


def post_message(env, event):
    # SendMessage resumes a finished agent: SubagentStop dropped its
    # record, so bring it back from the finished map.
    if "agent_id" in event:
        return ""
    target = event["tool_input"].get("to")
    finished = load_json(env.path("finished.json"), {})
    if target not in finished:
        return ""
    running = load_json(env.path("running.json"), {})
    running[target] = finished.pop(target)
    write_atomic(env.path("running.json"), json.dumps(running, indent=2))
    write_atomic(env.path("finished.json"), json.dumps(finished, indent=2))
    return upload_context(env)


def subagent_stop(env, event):
    running = load_json(env.path("running.json"), {})
    # Agents we never recorded stop too: foreground agents, subagents'
    # own launches, and agents launched before the hooks were installed.
    record = running.pop(event["agent_id"], None)
    write_atomic(env.path("running.json"), json.dumps(running, indent=2))
    if record is not None:
        finished = load_json(env.path("finished.json"), {})
        finished.pop(event["agent_id"], None)
        # Re-insert so a resumed agent counts as most recent.
        finished[event["agent_id"]] = record
        kept = dict(list(finished.items())[-FINISHED_KEPT:])
        write_atomic(env.path("finished.json"), json.dumps(kept, indent=2))
    regenerate(env)
    try:
        record_ledger_stop(env, event)
    except Exception as e:
        env.stderr.write("dashboard_lanes: ledger: %r\n" % e)
    return ""


def stop(env, event):
    # A subagent's stop is not the session's. stop_hook_active means the
    # session is already continuing after our block: blocking again would
    # loop.
    if "agent_id" in event or event.get("stop_hook_active"):
        return ""
    lanes_changed = regenerate(env)
    sloc_changed = regenerate_sloc(env)
    if not lanes_changed and not sloc_changed:
        return ""

    if lanes_changed and sloc_changed:
        reason = (
            "Dashboard lanes and sloc changed: upload %s and %s and run "
            "dashboard_lanes.py mark-uploaded."
            % (env.path("lanes.json"), env.path("sloc.json"))
        )
    elif lanes_changed:
        reason = (
            "Dashboard lanes changed: upload %s to the dashboard's lanes "
            "dataset and run dashboard_lanes.py mark-uploaded."
            % env.path("lanes.json")
        )
    else:
        reason = (
            "Dashboard sloc changed: upload %s to the dashboard's sloc "
            "dataset and run dashboard_lanes.py mark-uploaded."
            % env.path("sloc.json")
        )
    return json.dumps({"decision": "block", "reason": reason})


def mark_uploaded(env):
    lanes_path = env.path("lanes.json")
    sloc_path = env.path("sloc.json")
    if not os.path.exists(lanes_path) and not os.path.exists(sloc_path):
        raise FileNotFoundError("no dashboard files to mark uploaded")
    if os.path.exists(lanes_path):
        digest = hashlib.sha256(read_text(lanes_path).encode())
        write_atomic(env.path("uploaded.sha256"), digest.hexdigest() + "\n")
    if os.path.exists(sloc_path):
        digest = hashlib.sha256(read_text(sloc_path).encode())
        write_atomic(
            env.path("sloc.uploaded.sha256"), digest.hexdigest() + "\n"
        )
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
HOOKS = {"post-agent": post_agent, "post-message": post_message,
         "subagent-stop": subagent_stop, "stop": stop}
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
    env.stderr = stderr
    try:
        with lock(env):
            if argv[0] == "ledger":
                stdout.write(ledger(env, argv[1:]))
            elif argv[0] in MANUAL:
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
