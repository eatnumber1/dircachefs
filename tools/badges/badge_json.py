# SPDX-License-Identifier: MIT
"""The endpoint JSON of a README badge (plan step 26.28).

  badge_json.py coverage --input <gate output> --baseline <coverage_baseline.txt>
  badge_json.py reproducible --input <job log>
  badge_json.py mutation --input <report.md>
  badge_json.py tlc --input <test.log of //formal:large_test>
  badge_json.py osv --input <scanner JSON> [--input ...] --config osv-scanner.toml
      [--out <file>]   (stdout when omitted)

Each kind parses the one line its job prints (osv: the scanner's JSON, one
file per scan) and writes shields.io's endpoint schema: {"schemaVersion": 1,
"label", "message", "color"}. A line that is not there is an error (exit 2,
message on stderr, nothing written): a changed line must not turn into a stale
or empty badge.
"""

import argparse
import json
import re
import sys
import tomllib

COVERAGE = re.compile(
    r"^coverage_gate\.sh: dcfs: lines \d+/\d+ = (\d+\.\d+)%, "
    r"branches \d+/\d+ = (\d+\.\d+)%$", re.M)
BASELINE = re.compile(r"^(lines|branches) (\d+\.\d+)$", re.M)
REPRO_OK = re.compile(r"^reproducible: .+ are byte-identical across two builds$", re.M)
REPRO_DIFF = re.compile(r"^NOT REPRODUCIBLE: .+ differ in \d+ bytes$", re.M)
ALL_ROW = re.compile(r"^\| \*\*all\*\* \|(.*)\|$", re.M)
TLC_STATES = re.compile(
    r"^(\d+) states generated, (\d+) distinct states found, \d+ states left on queue\.$",
    re.M)
TLC_OK = re.compile(r"^Model checking completed\. No error has been found\.$", re.M)
TLC_ERROR = re.compile(r"^Error: .+$", re.M)


class BadInput(Exception):
    pass


def read(path):
    with open(path, encoding="utf-8") as f:
        return f.read()


def coverage(text, baseline):
    m = COVERAGE.search(text)
    if not m:
        raise BadInput("no 'coverage_gate.sh: dcfs: lines ... branches ...' line")
    base = dict(BASELINE.findall(baseline))
    if set(base) != {"lines", "branches"}:
        raise BadInput("baseline needs a 'lines' and a 'branches' value")
    lines, branches = m.group(1), m.group(2)
    at_baseline = (float(lines) >= float(base["lines"])
                   and float(branches) >= float(base["branches"]))
    return ("lines %s%% · branches %s%%" % (lines, branches),
            "green" if at_baseline else "red")


def reproducible(text):
    if REPRO_DIFF.search(text):
        return "differs", "red"
    if REPRO_OK.search(text):
        return "bit-identical", "green"
    raise BadInput("no reproducible verdict line")


def mutation(text):
    m = ALL_ROW.search(text)
    if not m:
        raise BadInput("no '| **all** |' row in the report")
    cells = [c.strip() for c in m.group(1).split("|")]
    if len(cells) != 7 or not all(c.isdigit() for c in cells):
        raise BadInput("the '**all**' row is not six status counts and a total")
    killed, total = int(cells[0]), int(cells[-1])
    if total == 0:
        raise BadInput("no mutant ran (the total is 0)")
    return ("killed %d/%d (%d%%)" % (killed, total, round(100 * killed / total)),
            "blue")


def tlc(text):
    m = TLC_STATES.search(text)
    if not m:
        raise BadInput("no 'N states generated, M distinct states found' line")
    distinct = m.group(2)
    if TLC_ERROR.search(text):
        return "%s states · violation" % distinct, "red"
    if TLC_OK.search(text):
        return "%s states · 0 violations" % distinct, "green"
    raise BadInput("neither 'Model checking completed' nor an 'Error:' line")


def osv(texts, config):
    """N findings not ignored by the config, M ignores, the earliest expiry.
    The scanner's JSON lists each finding's id under results[].packages[]
    .vulnerabilities[]; an id the config ignores does not count."""
    try:
        docs = [json.loads(t) for t in texts]
        cfg = tomllib.loads(config)
    except ValueError as e:
        raise BadInput("not JSON or TOML: %s" % e)
    found = set()
    for doc in docs:
        for result in doc.get("results", []):
            for pkg in result.get("packages", []):
                found.update(v["id"] for v in pkg.get("vulnerabilities", []))
    ignores = cfg.get("IgnoredVulns", [])
    ignored = {e["id"] for e in ignores}
    unignored = len(found - ignored)
    if not ignores:
        return "%d vulnerabilities · 0 ignored" % unignored, (
            "green" if unignored == 0 else "red")
    until = min(e["ignoreUntil"] for e in ignores).isoformat()
    return ("%d vulnerabilities · %d ignored until %s" % (
        unignored, len(ignores), until), "green" if unignored == 0 else "red")


def badge(kind, text, baseline=None, config=None):
    """The endpoint object for one job's output (osv: a list of JSON texts)."""
    if kind == "coverage":
        message, color = coverage(text, baseline or "")
    elif kind == "reproducible":
        message, color = reproducible(text)
    elif kind == "mutation":
        message, color = mutation(text)
    elif kind == "tlc":
        message, color = tlc(text)
    elif kind == "osv":
        message, color = osv(text, config or "")
    else:
        raise BadInput("unknown kind %r" % kind)
    return {"schemaVersion": 1, "label": kind, "message": message, "color": color}


def main(argv):
    p = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    p.add_argument("kind", choices=("coverage", "reproducible", "mutation", "tlc", "osv"))
    p.add_argument("--input", action="append",
                   help="the job's output (- for stdin); osv: one per scan")
    p.add_argument("--baseline", help="coverage: dcfs/coverage_baseline.txt")
    p.add_argument("--config", help="osv: osv-scanner.toml")
    p.add_argument("--out", help="the JSON file (stdout when omitted)")
    a = p.parse_args(argv)
    inputs = a.input or ["-"]
    if a.kind == "coverage" and not a.baseline:
        p.error("coverage needs --baseline")
    if a.kind == "osv" and not a.config:
        p.error("osv needs --config")
    if a.kind != "osv" and len(inputs) != 1:
        p.error("%s takes one --input" % a.kind)
    texts = [sys.stdin.read() if i == "-" else read(i) for i in inputs]
    baseline = read(a.baseline) if a.baseline else None
    config = read(a.config) if a.config else None
    try:
        obj = badge(a.kind, texts if a.kind == "osv" else texts[0], baseline, config)
    except BadInput as e:
        print("badge_json: %s: %s" % (a.kind, e), file=sys.stderr)
        return 2
    out = json.dumps(obj, separators=(",", ":")) + "\n"
    if a.out:
        with open(a.out, "w", encoding="utf-8") as f:
            f.write(out)
    else:
        sys.stdout.write(out)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
