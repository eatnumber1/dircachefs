# SPDX-License-Identifier: MIT
"""Mechanical agent liveness watchdog and dashboard generator.

Monitors active subagents in the Antigravity workspace, tracks transcript
updates, detects stalls/failures, and continuously updates the maintainer's
status dashboard without generating periodic turn noise in the agent session.
"""

import argparse
import datetime
import json
import os
import re
import sys
import tempfile
import time


def parse_iso_time(iso_str):
  """Parses an ISO 8601 UTC timestamp string into Unix epoch seconds."""
  if not iso_str:
    return 0.0
  clean_str = iso_str.rstrip("Z")
  try:
    dt = datetime.datetime.fromisoformat(clean_str)
    return dt.replace(tzinfo=datetime.timezone.utc).timestamp()
  except (ValueError, TypeError):
    return 0.0


def discover_subagents(brain_dir, parent_id, active_ids=None):
  """Scans parent transcript and subagent logs to discover subagents."""
  discovered = {}
  if active_ids:
    for cid in active_ids:
      cid = cid.strip()
      if cid:
        discovered[cid] = {
            "id": cid,
            "role": "Subagent",
            "typeName": "subagent",
        }
  else:
    parent_log = os.path.join(brain_dir, parent_id, ".system_generated",
                              "logs", "transcript.jsonl")
    if os.path.exists(parent_log):
      try:
        with open(parent_log, "r", encoding="utf-8") as f:
          for line in f:
            line = line.strip()
            if not line:
              continue
            try:
              data = json.loads(line)
            except json.JSONDecodeError:
              continue

            content = str(data.get("content", ""))
            try:
              cdata = json.loads(content)
              if isinstance(cdata, dict) and "conversationId" in cdata:
                cid = cdata["conversationId"]
                role = cdata.get("role") or cdata.get("Role") or "Subagent"
                typename = (
                    cdata.get("typeName") or cdata.get("TypeName") or "subagent"
                )
                if cid and cid != parent_id:
                  discovered[cid] = {
                      "id": cid,
                      "role": role,
                      "typeName": typename,
                  }
            except (json.JSONDecodeError, TypeError):
              pass

            for match in re.finditer(r'"conversationId":\s*"([a-zA-Z0-9-]+)"',
                                     content):
              cid = match.group(1)
              if cid and cid not in discovered and cid != parent_id:
                discovered[cid] = {
                    "id": cid,
                    "role": "Subagent",
                    "typeName": "subagent",
                }
      except OSError:
        pass

  # Infer specific roles from subagent logs if available
  for cid, info in discovered.items():
    sub_log = os.path.join(brain_dir, cid, ".system_generated", "logs",
                           "transcript.jsonl")
    if os.path.exists(sub_log):
      try:
        with open(sub_log, "r", encoding="utf-8") as f:
          first_line = f.readline()
          if first_line:
            first_data = json.loads(first_line)
            c = str(first_data.get("content", ""))
            if "Command Reviewer" in c:
              info["role"] = "Command Reviewer"
            elif "Code Reviewer" in c:
              info["role"] = "Code Reviewer"
            elif "Syscalls" in c or "implementing" in c.lower():
              info["role"] = "Syscalls Implementer"
      except (OSError, json.JSONDecodeError):
        pass

  return list(discovered.values())


def inspect_subagent(brain_dir, subagent_id, now=None,
                     stall_threshold_sec=600):
  """Inspects a subagent transcript and reports its current state."""
  if now is None:
    now = time.time()

  subagent_log = os.path.join(brain_dir, subagent_id, ".system_generated",
                              "logs", "transcript.jsonl")
  if not os.path.exists(subagent_log):
    return {
        "state": "unknown",
        "step_index": 0,
        "detail": "No transcript log found",
        "elapsed_sec": 0,
    }

  last_entry = None
  try:
    with open(subagent_log, "r", encoding="utf-8") as f:
      for line in f:
        line = line.strip()
        if line:
          try:
            last_entry = json.loads(line)
          except json.JSONDecodeError:
            continue
  except OSError as e:
    return {
        "state": "error",
        "step_index": 0,
        "detail": f"Log read error: {e}",
        "elapsed_sec": 0,
    }

  if not last_entry:
    return {
        "state": "unknown",
        "step_index": 0,
        "detail": "Empty transcript log",
        "elapsed_sec": 0,
    }

  step_index = last_entry.get("step_index", 0)
  created_at = last_entry.get("created_at", "")
  created_epoch = parse_iso_time(created_at)
  elapsed_sec = max(0.0, now - created_epoch) if created_epoch > 0 else 0.0

  # Determine detail from transcript
  detail = ""
  tool_calls = last_entry.get("tool_calls", [])
  if tool_calls:
    tc = tool_calls[0]
    name = tc.get("name", "tool")
    args = tc.get("args", {})
    action = args.get("toolAction") or args.get("toolSummary") or name
    detail = f"{name}: {action}"
  elif last_entry.get("content"):
    content = str(last_entry.get("content", "")).strip().replace("\n", " ")
    detail = content[:80] + ("..." if len(content) > 80 else "")
  else:
    detail = f"Step {step_index} ({last_entry.get('type', 'generic')})"

  # Check for background tasks in .system_generated/tasks/
  tasks_dir = os.path.join(brain_dir, subagent_id, ".system_generated", "tasks")
  latest_task_mtime = 0.0
  latest_task_line = ""
  if os.path.isdir(tasks_dir):
    try:
      for fname in os.listdir(tasks_dir):
        if fname.endswith(".log"):
          fpath = os.path.join(tasks_dir, fname)
          st = os.stat(fpath)
          if st.st_mtime > latest_task_mtime:
            latest_task_mtime = st.st_mtime
            try:
              with open(fpath, "r", encoding="utf-8", errors="replace") as tf:
                lines = [l.strip() for l in tf if l.strip()]
                if lines:
                  latest_task_line = lines[-1]
            except OSError:
              pass
    except OSError:
      pass

  # If background task has more recent activity than transcript turn:
  has_active_task = False
  if latest_task_mtime > created_epoch:
    elapsed_sec = max(0.0, now - latest_task_mtime)
    has_active_task = True
    if latest_task_line:
      detail = f"Task: {latest_task_line[:80]}"
    else:
      detail = "Task running in background"

  # Determine state
  state = "running"
  if last_entry.get("status") == "ERROR":
    state = "error"
  elif (
      not has_active_task
      and last_entry.get("type") == "PLANNER_RESPONSE"
      and not tool_calls
  ):
    state = "idle"
  elif elapsed_sec > stall_threshold_sec:
    state = "stalled"

  return {
      "state": state,
      "step_index": step_index,
      "detail": detail,
      "elapsed_sec": int(elapsed_sec),
  }


def format_dashboard(parent_id, agents_status, now=None):
  """Formats an updated Markdown status dashboard."""
  if now is None:
    now = time.time()
  dt_str = datetime.datetime.fromtimestamp(
      now, tz=datetime.timezone.utc
  ).strftime("%Y-%m-%d %H:%M:%S UTC")

  lines = [
      "# dcfs Status & Agent Watchdog Dashboard",
      "",
      f"**Last Updated:** {dt_str}",
      f"**Parent Session:** `{parent_id}`",
      "",
      "### Active Subagents",
      "| Role | Conversation ID | State | Last Step | Elapsed | Detail |",
      "|---|---|---|---|---|---|",
  ]

  for agent in agents_status:
    role = agent.get("role", "Unknown")
    cid = agent.get("id", "Unknown")
    st = agent.get("status", {})
    state = st.get("state", "unknown")
    state_badge = {
        "running": "**running**",
        "stalled": "⚠️ **stalled**",
        "error": "❌ **error**",
        "idle": "*idle*",
    }.get(state, state)
    step = st.get("step_index", 0)
    elapsed = f"{st.get('elapsed_sec', 0)}s"
    detail = st.get("detail", "")
    lines.append(f"| {role} | `{cid[:8]}...` | {state_badge} | {step} |"
                 f" {elapsed} | {detail} |")

  lines.extend([
      "",
      "> [!NOTE]",
      "> This dashboard is maintained mechanically by "
      "`tools/agent_watchdog.py`.",
      "> It monitors transcript activity without generating turn cycles.",
      "",
  ])
  return "\n".join(lines)


def write_status_atomically(target_path, content):
  """Writes content atomically to the destination file."""
  target_dir = os.path.dirname(os.path.abspath(target_path))
  os.makedirs(target_dir, exist_ok=True)
  with tempfile.NamedTemporaryFile("w", dir=target_dir, delete=False,
                                   encoding="utf-8") as f:
    f.write(content)
    temp_name = f.name
  os.replace(temp_name, target_path)


def run_watchdog(brain_dir, parent_id, status_file,
                 active_ids=None, interval_sec=5, stall_threshold_sec=600,
                 once=False):
  """Runs the watchdog monitor loop or a single pass."""
  while True:
    now = time.time()
    subagents = discover_subagents(brain_dir, parent_id, active_ids=active_ids)
    agents_status = []
    for sub in subagents:
      st = inspect_subagent(brain_dir, sub["id"], now=now,
                            stall_threshold_sec=stall_threshold_sec)
      agents_status.append({
          "id": sub["id"],
          "role": sub.get("role", "Subagent"),
          "typeName": sub.get("typeName", ""),
          "status": st,
      })

    dashboard = format_dashboard(parent_id, agents_status, now=now)
    write_status_atomically(status_file, dashboard)

    if once:
      print(f"Watchdog: checked {len(agents_status)} subagents, "
            f"updated {status_file}")
      break

    time.sleep(interval_sec)
  return 0


def main(argv):
  parser = argparse.ArgumentParser(description=__doc__)
  default_brain = os.path.expanduser("~/.gemini/antigravity-cli/brain")
  parser.add_argument("--brain-dir", default=default_brain,
                      help="Path to Antigravity brain directory")
  parser.add_argument("--parent-id", required=True,
                      help="Conversation ID of parent orchestrator")
  parser.add_argument("--active-ids", default="",
                      help="Comma-separated IDs of active subagents to monitor")
  parser.add_argument("--status-file", default="/tmp/dcfs_status.md",
                      help="Destination status markdown file")
  parser.add_argument("--interval", type=int, default=5,
                      help="Polling interval in seconds (daemon mode)")
  parser.add_argument("--stall-threshold", type=int, default=600,
                      help="Seconds without transcript update before stall")
  parser.add_argument("--once", action="store_true",
                      help="Run a single inspection pass and exit")
  args = parser.parse_args(argv)

  active_ids = (
      [s.strip() for s in args.active_ids.split(",") if s.strip()]
      if args.active_ids
      else None
  )

  return run_watchdog(
      brain_dir=args.brain_dir,
      parent_id=args.parent_id,
      status_file=args.status_file,
      active_ids=active_ids,
      interval_sec=args.interval,
      stall_threshold_sec=args.stall_threshold,
      once=args.once,
  )


if __name__ == "__main__":
  sys.exit(main(sys.argv[1:]))
