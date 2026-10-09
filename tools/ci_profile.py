#!/usr/bin/env python3
"""Sums Bazel JSON trace profiles into a table of where the time went (26.16).

  ci_profile.py PROFILE.json[.gz]...

One markdown table per profile (CI appends them to the job summary), made
with `bazel build --profile=FILE --experimental_profile_include_target_label
--experimental_profile_include_primary_output` (.github/ci/test.sh does):

  Total wall         from the start of the Bazel client to "Complete build"
  Repository fetches "Fetching repository" events, every repository but:
  @dcfs_llvm         the pinned LLVM toolchain's download and extraction
                     (the repository whose name ends in +dcfs_llvm)
  Third-party builds actions of a target under an @-repository or
                     //third_party (the kernel, QEMU, the Debian image, ...)
  Our compile, link  actions of a target under //dcfs, //tools, //bench or
                     //test: compiles, links, genrules, runfiles
  Other actions      the remaining non-test actions
  Test execution     TestRunner actions (tests served from the cache run no
                     action and are not counted)
  Critical path      the components Bazel reports, summed, and the longest

An action without a target label (a profile made without the flag) is
third-party when its primary output is under external/, else "other".

"Wall" is the time during which at least one event of the row was running
(the union of the intervals; the rows overlap, so they do not add up to the
total); "busy" is the sum of the events' durations, i.e. thread time; "n" is
the number of events. A repository fetch that waits for another one overlaps
it. Exits 0, 2 on a profile that cannot be read.
"""

import gzip
import json
import os
import re
import sys

OURS = ("dcfs", "tools", "bench", "test")
LLVM_REPO = "dcfs_llvm"
_EXTERNAL = re.compile(r"(^|/)external/")

# (row key, title), in output order.
ROWS = (
    ("fetch", "Repository fetches (not @dcfs_llvm)"),
    ("llvm", "@dcfs_llvm fetch and extraction"),
    ("third_party", "Third-party builds"),
    ("ours", "Our compile and link"),
    ("other", "Other actions"),
    ("test", "Test execution"),
    ("critical", "Critical path (Bazel's)"),
)


def load(path):
    """Returns the profile's events (dicts)."""
    opener = gzip.open if path.endswith(".gz") else open
    with opener(path, "rt", encoding="utf-8") as f:
        doc = json.load(f)
    events = doc["traceEvents"] if isinstance(doc, dict) else doc
    return [e for e in events if isinstance(e, dict)]


def repo_name(event):
    """The canonical repository name of a fetch event ("@@a+b" -> "a+b")."""
    return event.get("name", "").lstrip("@")


def classify_action(event):
    """The row key of an "action processing" event."""
    args = event.get("args") or {}
    if args.get("mnemonic") == "TestRunner":
        return "test"
    target = args.get("target", "")
    if target.startswith("@"):
        return "third_party"
    if target.startswith("//"):
        top = target[2:].split(":")[0].split("/")[0]
        if top == "third_party":
            return "third_party"
        return "ours" if top in OURS else "other"
    if _EXTERNAL.search(event.get("out", "")):
        return "third_party"
    return "other"


def classify(event):
    """Returns the row key of a complete event, or None if no row counts it."""
    cat = event.get("cat")
    if cat == "Fetching repository":
        if repo_name(event).rsplit("+", 1)[-1] == LLVM_REPO:
            return "llvm"
        return "fetch"
    if cat == "action processing":
        return classify_action(event)
    if cat == "critical path component":
        return "critical"
    return None


def union(intervals):
    """Total length of the union of (start, end) intervals."""
    total = 0
    end = None
    for start, stop in sorted(intervals):
        if end is None or start > end:
            total += stop - start
            end = stop
        elif stop > end:
            total += stop - end
            end = stop
    return total


def analyze(events):
    """Returns {"total": us, "rows": {key: (wall, busy, n)}, "longest": [...]}."""
    spans = {key: [] for key, _ in ROWS}
    components = []
    start = None
    end = None
    complete = [e for e in events if e.get("ph") == "X"]
    for e in events:
        if e.get("cat") == "build phase marker":
            ts = e.get("ts", 0)
            start = ts if start is None else min(start, ts)
            if e.get("name") == "Complete build":
                end = ts
    for e in complete:
        key = classify(e)
        if key is None:
            continue
        spans[key].append((e["ts"], e["ts"] + e.get("dur", 0)))
        if key == "critical":
            components.append((e.get("dur", 0), e.get("name", "")))
    if start is None:
        start = min((e["ts"] for e in complete), default=0)
    if end is None:
        end = max((e["ts"] + e.get("dur", 0) for e in complete), default=start)
    rows = {}
    for key, intervals in spans.items():
        busy = sum(b - a for a, b in intervals)
        rows[key] = (union(intervals), busy, len(intervals))
    components.sort(key=lambda c: -c[0])
    return {"total": end - start, "rows": rows, "longest": components[:3]}


def seconds(us):
    return "%.1f" % (us / 1e6)


def render(name, result):
    """The markdown table of one profile."""
    lines = [
        "#### %s" % name,
        "",
        "| What | Wall (s) | Busy (s) | n |",
        "|---|---:|---:|---:|",
        "| Total wall | %s | | |" % seconds(result["total"]),
    ]
    titles = dict(ROWS)
    for key, _ in ROWS:
        wall, busy, n = result["rows"][key]
        if key == "critical":
            wall = busy  # the components run one after the other
        lines.append("| %s | %s | %s | %d |" % (titles[key], seconds(wall), seconds(busy), n))
    if result["longest"]:
        lines.append("")
        lines.append("Longest critical path components:")
        for dur, label in result["longest"]:
            lines.append("- %s s: %s" % (seconds(dur), label))
    lines.append("")
    lines.append(
        "Wall is the time at least one such event was running (rows overlap); "
        "busy is the sum of the events' durations."
    )
    return "\n".join(lines) + "\n"


def stem(path):
    base = os.path.basename(path)
    for suffix in (".json.gz", ".json"):
        if base.endswith(suffix):
            return base[: -len(suffix)]
    return base


def main(argv, out=sys.stdout):
    if not argv or argv[0] in ("-h", "--help"):
        print(__doc__, file=sys.stderr)
        return 2
    status = 0
    first = True
    for path in argv:
        try:
            result = analyze(load(path))
        except (OSError, ValueError, KeyError) as err:
            print("ci_profile: %s: %s" % (path, err), file=sys.stderr)
            status = 2
            continue
        if not first:
            out.write("\n")
        first = False
        out.write(render(stem(path), result))
    return status


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
