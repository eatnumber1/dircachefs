"""The endpoint JSON of a README badge (plan step 26.28).

  badge_json.py coverage --input <gate output> --baseline <coverage_baseline.txt>
  badge_json.py reproducible --input <job log>
  badge_json.py mutation --input <report.md>
      [--out <file>]   (stdout when omitted)

Each kind parses the one line its job prints and writes shields.io's endpoint
schema: {"schemaVersion": 1, "label", "message", "color"}. A line that is not
there is an error (exit 2, message on stderr, nothing written): a changed line
must not turn into a stale or empty badge.
"""

import argparse
import json
import re
import sys

COVERAGE = re.compile(
    r"^coverage_gate\.sh: dcfs: lines \d+/\d+ = (\d+\.\d+)%, "
    r"branches \d+/\d+ = (\d+\.\d+)%$", re.M)
BASELINE = re.compile(r"^(lines|branches) (\d+\.\d+)$", re.M)
REPRO_OK = re.compile(r"^reproducible: .+ are byte-identical across two builds$", re.M)
REPRO_DIFF = re.compile(r"^NOT REPRODUCIBLE: .+ differ in \d+ bytes$", re.M)
ALL_ROW = re.compile(r"^\| \*\*all\*\* \|(.*)\|$", re.M)


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


def badge(kind, text, baseline=None):
    """The endpoint object for one job's output."""
    if kind == "coverage":
        message, color = coverage(text, baseline or "")
    elif kind == "reproducible":
        message, color = reproducible(text)
    elif kind == "mutation":
        message, color = mutation(text)
    else:
        raise BadInput("unknown kind %r" % kind)
    return {"schemaVersion": 1, "label": kind, "message": message, "color": color}


def main(argv):
    p = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    p.add_argument("kind", choices=("coverage", "reproducible", "mutation"))
    p.add_argument("--input", default="-", help="the job's output (- for stdin)")
    p.add_argument("--baseline", help="coverage: dcfs/coverage_baseline.txt")
    p.add_argument("--out", help="the JSON file (stdout when omitted)")
    a = p.parse_args(argv)
    if a.kind == "coverage" and not a.baseline:
        p.error("coverage needs --baseline")
    text = sys.stdin.read() if a.input == "-" else read(a.input)
    baseline = read(a.baseline) if a.baseline else None
    try:
        obj = badge(a.kind, text, baseline)
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
