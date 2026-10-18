#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Keep the lanes table and code size panel of the dashboard current.

Claude Code hooks call this tool with the hook's JSON on stdin; it records
which subagents run on which lane, computes code size (SLOC) over the main
checkout, writes the dashboard files, and tells the orchestrator when they
differ from the last uploaded copies. Everything is event-driven except the
hourly meter reading, an approved timer exception: no polling. See README.md in
this directory for the hook configuration.

    dashboard_lanes.py post-agent      PostToolUse hook, matcher "Agent"
    dashboard_lanes.py post-message    PostToolUse hook, matcher "SendMessage"
    dashboard_lanes.py subagent-stop   SubagentStop hook
    dashboard_lanes.py stop            the main session's Stop hook
    dashboard_lanes.py mark-uploaded   record the current file as uploaded
    dashboard_lanes.py show            print the lanes file
    dashboard_lanes.py quota           read usage meters and update quota/budget
    dashboard_lanes.py lane-state ...  set or clear lane state override
    dashboard_lanes.py ledger add ...  append row for unhooked agent job
    dashboard_lanes.py ledger set ...  fill outcome fields at merge time

State lives in $DCFS_DASHBOARD_DIR (default ~/.cache/dcfs-dashboard):
running.json, finished.json, lanes.json, uploaded.sha256,
lanes.uploaded.json, lane_states.json, lane_states.uploaded.json,
sloc.json, sloc.head, sloc.uploaded.sha256, quota.json,
quota.uploaded.sha256, quota.read_at, quota.log, budget.json,
budget.uploaded.sha256, and an optional config.json with "model" and
"effort" for the orchestrator row. Dispatch ledger is kept in
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
import zoneinfo

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
    runner: object = None
    launcher: object = None
    environ: dict = dataclasses.field(default_factory=dict)

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
            environ=dict(environ),
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


MONTHS = {
    "jan": 1, "feb": 2, "mar": 3, "apr": 4, "may": 5, "jun": 6,
    "jul": 7, "aug": 8, "sep": 9, "oct": 10, "nov": 11, "dec": 12,
}
RESET_TIME_RE = re.compile(
    r"^(?:(?P<month>[A-Za-z]{3})\s*(?P<day>\d{1,2})\s*,?\s*)?"
    r"(?P<hour>\d{1,2})(?::(?P<minute>\d{2}))?\s*(?P<ampm>am|pm)$",
    re.IGNORECASE,
)
POOL_ORDER = [
    "Claude session",
    "Claude week",
    "Gemini week",
    "Gemini five-hour",
    "agy Claude/GPT week",
    "agy Claude/GPT five-hour",
    "Gemini 4 Argon week",
]


def parse_iso_datetime(t):
    if t.endswith("Z"):
        t = t[:-1] + "+00:00"
    return datetime.datetime.fromisoformat(t)


def get_current_datetime(env):
    return parse_iso_datetime(env.current_time())


def get_now_timestamp(env):
    return get_current_datetime(env).timestamp()


def local_timezone(env=None):
    return datetime.datetime.now().astimezone().tzinfo


def resolve_claude_reset(raw_time, tz_str, env):
    if not tz_str:
        env.stderr.write(
            "dashboard_lanes: missing timezone in reset time %r\n" % raw_time
        )
        return None
    try:
        tz = zoneinfo.ZoneInfo(tz_str.strip())
    except (zoneinfo.ZoneInfoNotFoundError, KeyError, ValueError) as e:
        env.stderr.write(
            "dashboard_lanes: unknown timezone %r: %s\n" % (tz_str, e)
        )
        return None

    now_dt = get_current_datetime(env)
    now_local = now_dt.astimezone(tz)

    m = RESET_TIME_RE.match(raw_time.strip())
    if not m:
        env.stderr.write(
            "dashboard_lanes: unparseable reset time: %r\n" % raw_time
        )
        return None

    hour = int(m.group("hour"))
    minute = int(m.group("minute") or 0)
    ampm = m.group("ampm").lower()
    if ampm == "pm" and hour < 12:
        hour += 12
    elif ampm == "am" and hour == 12:
        hour = 0

    if m.group("month"):
        month_name = m.group("month").lower()[:3]
        if month_name not in MONTHS:
            env.stderr.write(
                "dashboard_lanes: unparseable month in reset time: %r\n"
                % raw_time
            )
            return None
        month = MONTHS[month_name]
        day = int(m.group("day"))
        try:
            candidate = datetime.datetime(
                now_local.year, month, day, hour, minute, tzinfo=tz
            )
        except ValueError as e:
            env.stderr.write(
                "dashboard_lanes: invalid date in reset time %r: %s\n"
                % (raw_time, e)
            )
            return None
        if candidate < now_local:
            try:
                candidate = datetime.datetime(
                    now_local.year + 1, month, day, hour, minute, tzinfo=tz
                )
            except ValueError as e:
                env.stderr.write(
                    "dashboard_lanes: invalid date in reset time %r: %s\n"
                    % (raw_time, e)
                )
                return None
    else:
        try:
            candidate = datetime.datetime(
                now_local.year, now_local.month, now_local.day, hour, minute,
                tzinfo=tz
            )
        except ValueError as e:
            env.stderr.write(
                "dashboard_lanes: invalid date in reset time %r: %s\n"
                % (raw_time, e)
            )
            return None
        if candidate < now_local:
            candidate += datetime.timedelta(days=1)

    return candidate.astimezone(datetime.timezone.utc).strftime(
        "%Y-%m-%dT%H:%M:%SZ"
    )


def parse_claude_usage(text, env=None):
    if env is None:
        env = Env.from_environ(os.environ)
    rows = []
    current_pool = None
    used_pct = None
    for line in text.splitlines():
        line_clean = line.strip()
        lower = line_clean.lower()
        if "current" in lower and "session" in lower:
            current_pool = "Claude session"
            used_pct = None
        elif "current" in lower and "week" in lower:
            current_pool = "Claude week"
            used_pct = None
        pct_match = re.search(r"(\d+)%\s*used", line_clean)
        if pct_match and current_pool:
            used_pct = int(pct_match.group(1))
        reset_match = re.search(
            r"Resets\s*([A-Za-z0-9,:\s]+?)(?:\s*\(([^)]+)\))?$", line_clean
        )
        if reset_match and current_pool and used_pct is not None:
            raw_time = reset_match.group(1).strip()
            tz_str = reset_match.group(2)
            resets = resolve_claude_reset(raw_time, tz_str, env)
            if resets is not None:
                rows.append({
                    "pool": current_pool,
                    "used_pct": used_pct,
                    "resets": resets,
                })
            current_pool = None
            used_pct = None
    return rows


def parse_agy_usage(text):
    rows = []
    for line in text.splitlines():
        if not line.strip() or "disabled" in line.lower():
            continue
        parts = [p.strip() for p in line.split("\t")]
        if len(parts) < 4:
            continue
        model_group, limit_name, pct_str, resets = parts[:4]
        m = re.search(r"(\d+)%", pct_str)
        if not m:
            continue
        remaining = int(m.group(1))
        used_pct = 100 - remaining

        pool = None
        if "Gemini Models" in model_group:
            if "Weekly" in limit_name:
                pool = "Gemini week"
            elif "Five Hour" in limit_name:
                pool = "Gemini five-hour"
        elif "Claude and GPT" in model_group:
            if "Weekly" in limit_name:
                pool = "agy Claude/GPT week"
            elif "Five Hour" in limit_name:
                pool = "agy Claude/GPT five-hour"
        elif "Gemini 4 Argon" in model_group:
            if "Weekly" in limit_name:
                pool = "Gemini 4 Argon week"

        if pool:
            rows.append({
                "pool": pool,
                "used_pct": used_pct,
                "resets": resets,
            })
    return rows


def run_meter(cmd, runner, env, **kwargs):
    cmd_name = " ".join(cmd)
    try:
        res = runner(cmd, **kwargs)
        if isinstance(res, subprocess.CompletedProcess):
            if res.returncode == 0:
                return res.stdout
            err = (res.stderr or "").strip()
            env.stderr.write(
                "dashboard_lanes: %s exited %d: %s\n"
                % (cmd_name, res.returncode, err)
            )
            return None
        return res
    except subprocess.CalledProcessError as e:
        err = (
            (e.stderr or "").strip()
            if hasattr(e, "stderr") and e.stderr
            else str(e)
        )
        env.stderr.write(
            "dashboard_lanes: %s exited %d: %s\n"
            % (cmd_name, e.returncode, err)
        )
        return None
    except subprocess.TimeoutExpired as e:
        env.stderr.write(
            "dashboard_lanes: %s timed out after %ss\n"
            % (cmd_name, e.timeout)
        )
        return None
    except OSError as e:
        env.stderr.write(
            "dashboard_lanes: %s failed: %s\n"
            % (cmd_name, e)
        )
        return None


def quota(env):
    runner = getattr(env, "runner", None) or subprocess.run
    claude_out = run_meter(
        ["python3", "-I", "tools/agents/claude_usage.py"],
        runner,
        env,
        cwd=env.main_repo,
        timeout=90,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    agy_out = run_meter(
        ["agy", "-p", "/usage"],
        runner,
        env,
        timeout=120,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )

    existing = load_json(env.path("quota.json"), [])
    by_pool = {r["pool"]: r for r in existing if "pool" in r}

    if claude_out is not None:
        try:
            for r in parse_claude_usage(claude_out, env):
                by_pool[r["pool"]] = r
        except (ValueError, KeyError, IndexError) as e:
            env.stderr.write("dashboard_lanes: parse claude: %r\n" % e)

    if agy_out is not None:
        try:
            for r in parse_agy_usage(agy_out):
                by_pool[r["pool"]] = r
        except (ValueError, KeyError, IndexError) as e:
            env.stderr.write("dashboard_lanes: parse agy: %r\n" % e)

    ordered_rows = []
    for p in POOL_ORDER:
        if p in by_pool:
            ordered_rows.append(by_pool[p])
    for p, r in by_pool.items():
        if p not in POOL_ORDER:
            ordered_rows.append(r)

    quota_text = json.dumps(ordered_rows, indent=2) + "\n"
    write_atomic(env.path("quota.json"), quota_text)

    claude_week = by_pool.get("Claude week")
    if claude_week is not None:
        actual = claude_week.get("used_pct")
        when = env.current_time()
        now_dt = get_current_datetime(env)
        local_tz = local_timezone(env)
        local_dt = now_dt.astimezone(local_tz)
        label = local_dt.strftime("%a %H:%M")
        budget = load_json(env.path("budget.json"), [])
        readings_before = [
            r for r in budget
            if r.get("when", "") <= when and r.get("actual") is not None
        ]
        if not readings_before or readings_before[-1].get("actual") != actual:
            idx = 0
            while idx < len(budget) and budget[idx].get("when", "") <= when:
                idx += 1
            budget.insert(idx, {
                "when": when,
                "label": label,
                "target": None,
                "actual": actual,
            })
            write_atomic(
                env.path("budget.json"), json.dumps(budget, indent=2) + "\n"
            )

    return ""


DATASETS = ["lanes", "sloc", "quota", "budget"]


def dataset_paths(env, name):
    json_path = env.path("%s.json" % name)
    sha_file = (
        "uploaded.sha256" if name == "lanes" else "%s.uploaded.sha256" % name
    )
    return json_path, env.path(sha_file)


def file_digest(path):
    return hashlib.sha256(read_text(path).encode("utf-8")).hexdigest()


def lanes_material_changed(env):
    """Whether lanes has a material change vs the last uploaded table."""
    lanes_path = env.path("lanes.json")
    if not os.path.exists(lanes_path):
        return False
    sha_path = env.path("uploaded.sha256")
    if not os.path.exists(sha_path):
        return True
    if file_digest(lanes_path) == read_text(sha_path).strip():
        return False

    up_path = env.path("lanes.uploaded.json")
    if not os.path.exists(up_path):
        return True
    up_rows = json.loads(read_text(up_path))

    cur_states = load_json(env.path("lane_states.json"), {})
    up_states = load_json(env.path("lane_states.uploaded.json"), {})
    if cur_states != up_states:
        return True

    cur_rows = json.loads(read_text(lanes_path))

    cur_by_lane = {r["lane"]: r for r in cur_rows if "lane" in r}
    up_by_lane = {r["lane"]: r for r in up_rows if "lane" in r}

    for name, cur in cur_by_lane.items():
        if name == "orchestrator":
            continue
        up = up_by_lane.get(name)
        if not up:
            return True
        # Newly dispatched: went to running from idle or override
        if cur.get("state") == "running" and up.get("state") != "running":
            return True
        # Step changed (excluding running -> idle transitions)
        if cur.get("state") != "idle" and up.get("state") != "idle":
            if cur.get("step") != up.get("step"):
                return True

    return False


def dataset_changed(env, name):
    if name == "lanes":
        return lanes_material_changed(env)
    json_path, sha_path = dataset_paths(env, name)
    if not os.path.exists(json_path):
        return False
    uploaded = (
        read_text(sha_path).strip() if os.path.exists(sha_path) else None
    )
    return file_digest(json_path) != uploaded


def file_changed(path, uploaded_path):
    if not os.path.exists(path):
        return False
    uploaded = (
        read_text(uploaded_path).strip()
        if os.path.exists(uploaded_path)
        else None
    )
    return file_digest(path) != uploaded


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


def clear_lane_override(env, lane):
    states_path = env.path("lane_states.json")
    states = load_json(states_path, {})
    if lane in states:
        del states[lane]
        write_atomic(states_path, json.dumps(states, indent=2) + "\n")


def record_agent(env, event):
    tool_input = event["tool_input"]
    response = event["tool_response"]
    if response["status"] != "async_launched":
        return
    front = frontmatter(env, tool_input["subagent_type"])
    match = LANE_IN_PROMPT.search(tool_input["prompt"])
    lane = "lane-" + match.group(1) if match else None
    if lane:
        clear_lane_override(env, lane)
    running = load_json(env.path("running.json"), {})
    running[response["agentId"]] = {
        "subagent_type": tool_input["subagent_type"],
        "description": tool_input["description"],
        "lane": lane,
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
    """Maps a working directory to (model, label) of an agy running there."""
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
        mode = ""
        sandboxed = False
        for i, arg in enumerate(argv):
            if arg == "--model" and i + 1 < len(argv):
                model = argv[i + 1]
            elif arg.startswith("--model="):
                model = arg.removeprefix("--model=")
            elif arg == "--mode" and i + 1 < len(argv):
                mode = argv[i + 1]
            elif arg.startswith("--mode="):
                mode = arg.removeprefix("--mode=")
            elif arg == "--sandbox":
                sandboxed = True
        label = "agy"
        if mode == "plan":
            label += ", read-only"
        if sandboxed:
            label += ", sandboxed"
        cwd = os.path.realpath(os.path.join(env.proc_root, pid, "cwd"))
        found[cwd] = (model, label)
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
    overrides = load_json(env.path("lane_states.json"), {})
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
            model, label = agy[os.path.realpath(lane_dir)]
            effort = EFFORT_SUFFIX.match(model)
            shown.append((label,
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
        if name in overrides:
            ov = overrides[name]
            fallback = records[0]["description"] if records else ""
            step = step_of(branch) if branch else fallback
            row.update(
                step=step,
                agent=ov.get("note", ""),
                model="",
                effort="",
                state=ov.get("state", ""),
            )
        rows.append(row)
    return rows


def regenerate(env):
    """Writes lanes.json; returns whether it differs from uploaded copy."""
    text = json.dumps(build_rows(env), indent=2) + "\n"
    write_atomic(env.path("lanes.json"), text)
    return dataset_changed(env, "lanes")


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
            return description
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


def lane_state(env, args):
    if len(args) < 2:
        raise ValueError(
            "usage: lane-state LANE STATE [NOTE] or lane-state LANE clear"
        )
    lane = args[0]
    if not lane.startswith("lane-") and lane.isdigit():
        lane = "lane-" + lane
    states_path = env.path("lane_states.json")
    states = load_json(states_path, {})
    action = args[1]
    if action == "clear":
        states.pop(lane, None)
    else:
        note = " ".join(args[2:]) if len(args) > 2 else ""
        states[lane] = {"state": action, "note": note}
    write_atomic(states_path, json.dumps(states, indent=2) + "\n")
    regenerate(env)
    return ""


def post_agent(env, event):
    # An agent_id means a subagent made this call: its launches are not
    # lanes of the orchestrator.
    if "agent_id" in event:
        return ""
    record_agent(env, event)
    try:
        record_ledger_dispatch(env, event)
    except (OSError, ValueError, KeyError) as e:
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
    record = finished.pop(target)
    lane = record.get("lane")
    if lane:
        clear_lane_override(env, lane)
    running[target] = record
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
    except (OSError, ValueError, KeyError) as e:
        env.stderr.write("dashboard_lanes: ledger: %r\n" % e)
    return ""


def launch_quota(env):
    cmd = [sys.executable, "-I", os.path.abspath(__file__), "quota"]
    launcher = getattr(env, "launcher", None) or subprocess.Popen
    log_path = env.path("quota.log")
    child_env = dict(env.environ) if env.environ else dict(os.environ)
    child_env["DCFS_DASHBOARD_DIR"] = env.dashboard_dir
    try:
        with open(os.devnull, "r") as devnull, open(
            log_path, "a", encoding="utf-8"
        ) as log_file:
            launcher(
                cmd,
                stdin=devnull,
                stdout=log_file,
                stderr=log_file,
                start_new_session=True,
                env=child_env,
            )
    except (OSError, subprocess.SubprocessError) as e:
        env.stderr.write("dashboard_lanes: launch quota: %r\n" % e)


def stop(env, event):
    # A subagent's stop is not the session's. stop_hook_active means the
    # session is already continuing after our block: blocking again would
    # loop.
    if "agent_id" in event or event.get("stop_hook_active"):
        return ""

    regenerate(env)
    regenerate_sloc(env)

    # The hourly meter reading is an approved timer exception, since the
    # meters cannot signal: "I approve this exception *because* I'm
    # sometimes spending credits out-of-band" (russ, 2026-10-17).
    read_at_path = env.path("quota.read_at")
    should_read = True
    if os.path.exists(read_at_path):
        try:
            read_at_dt = parse_iso_datetime(read_text(read_at_path).strip())
            now_dt = get_current_datetime(env)
            if (now_dt - read_at_dt).total_seconds() < 3600:
                should_read = False
        except (ValueError, OSError) as e:
            env.stderr.write("dashboard_lanes: parse read_at: %r\n" % e)

    if should_read:
        write_atomic(read_at_path, env.current_time() + "\n")
        launch_quota(env)

    changed = []
    for name in DATASETS:
        if dataset_changed(env, name):
            changed.append((name, env.path("%s.json" % name)))

    if not changed:
        return ""

    if len(changed) == 1:
        name, path = changed[0]
        reason = (
            "Dashboard %s changed: upload %s to the dashboard's %s dataset "
            "and run dashboard_lanes.py mark-uploaded."
            % (name, path, name)
        )
    else:
        names = [c[0] for c in changed]
        paths = [c[1] for c in changed]
        names_str = ", ".join(names[:-1]) + " and " + names[-1]
        paths_str = ", ".join(paths[:-1]) + " and " + paths[-1]
        reason = (
            "Dashboard %s changed: upload %s and run dashboard_lanes.py "
            "mark-uploaded."
            % (names_str, paths_str)
        )
    return json.dumps({"decision": "block", "reason": reason})


def mark_uploaded(env):
    marked = False
    for name in DATASETS:
        json_path, sha_path = dataset_paths(env, name)
        if os.path.exists(json_path):
            write_atomic(sha_path, file_digest(json_path) + "\n")
            marked = True
    if not marked:
        raise FileNotFoundError("no dashboard files to mark uploaded")
    if os.path.exists(env.path("lanes.json")):
        write_atomic(
            env.path("lanes.uploaded.json"),
            read_text(env.path("lanes.json")),
        )
    states_path = env.path("lane_states.json")
    write_atomic(
        env.path("lane_states.uploaded.json"),
        json.dumps(load_json(states_path, {}), indent=2) + "\n",
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
MANUAL = {"mark-uploaded": mark_uploaded, "show": show, "quota": quota}


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
            elif argv[0] == "lane-state":
                stdout.write(lane_state(env, argv[1:]))
            elif argv[0] in MANUAL:
                stdout.write(MANUAL[argv[0]](env))
            else:
                event = json.loads(stdin.read() or "{}")
                if not isinstance(event, dict):
                    raise ValueError("hook input is not a JSON object")
                stdout.write(HOOKS[argv[0]](env, event))
    except (
        OSError,
        ValueError,
        KeyError,
        IndexError,
        TypeError,
        AttributeError,
        subprocess.SubprocessError,
    ) as e:
        stderr.write("dashboard_lanes: %s: %r\n" % (argv[0] if argv else "", e))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
