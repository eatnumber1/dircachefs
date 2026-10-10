#!/usr/bin/env python3
"""Summarizes one `bazel test` of the noisy CI job (step 26.14e).

The noisy job runs the suite with the quiet kernel's sysctls left at the
kernel's defaults, every guest at two vCPUs and each small and medium test
three times (README.md, "A noisy run"). A failure there is a finding, not a
red push: this reads the build event file (`--build_event_json_file`), prints
a markdown table of the tests that failed or were flaky (some runs of
`--runs_per_test` failed) for the job summary, and copies those tests' logs
and guest consoles into a directory the job uploads. It exits 0 whatever it
finds; only a file it cannot read is an error (exit 2).

  noisy_report.py --name NAME --bep FILE [--testlogs DIR] [--logs-out DIR]
"""

import argparse
import json
import os
import shutil
import sys


def load(path):
    """The (label, status, runs, failed runs) of every test summary in the file."""
    tests = {}
    with open(path, encoding="utf-8") as f:
        for number, line in enumerate(f, 1):
            line = line.strip()
            if not line:
                continue
            try:
                event = json.loads(line)
            except ValueError as e:
                raise ValueError("%s:%d: %s" % (path, number, e))
            summary = event.get("testSummary")
            if summary is None:
                continue
            label = event["id"]["testSummary"]["label"]
            status = summary.get("overallStatus", "NO_STATUS")
            failed = len(summary.get("failed", []))
            passed = len(summary.get("passed", []))
            runs = summary.get("totalRunCount") or (failed + passed) or 1
            tests[label] = (status, runs, failed)
    return tests


def report(name, tests):
    """The markdown for the job summary."""
    counts = {}
    for status, _, _ in tests.values():
        counts[status] = counts.get(status, 0) + 1
    findings = sorted(
        (label, status, runs, failed)
        for label, (status, runs, failed) in tests.items()
        if status != "PASSED"
    )
    lines = ["### Noisy run: %s" % name, ""]
    lines.append(
        "%d tests: %s." % (len(tests), ", ".join("%d %s" % (n, s.lower()) for s, n in sorted(counts.items())) or "none")
    )
    lines.append("")
    if not findings:
        lines.append("No failures and no flaky tests.")
        return "\n".join(lines) + "\n"
    lines.append("| Test | Result | Failed runs |")
    lines.append("|---|---|---|")
    for label, status, runs, failed in findings:
        lines.append("| `%s` | %s | %d of %d |" % (label, status.lower(), failed, runs))
    lines.append("")
    lines.append(
        "Each is a finding (a race or a kernel-timing dependence the quiet "
        "default hides), not a failed push: pin the interleaving in a "
        "deterministic test before fixing (AGENTS.md, \"Test first\"). Logs: "
        "the job's `noisy-report-*` artifact, under `logs/%s`." % name
    )
    return "\n".join(lines) + "\n"


def copy_logs(tests, testlogs, out):
    """Copies the log directory of every test that was not a plain pass."""
    copied = 0
    for label, (status, _, _) in sorted(tests.items()):
        if status == "PASSED":
            continue
        package, _, target = label.lstrip("/").partition(":")
        src = os.path.join(testlogs, package, target)
        if not os.path.isdir(src):
            continue
        shutil.copytree(src, os.path.join(out, package, target), dirs_exist_ok=True, symlinks=False, ignore_dangling_symlinks=True)
        copied += 1
    return copied


def main(argv):
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--name", required=True)
    parser.add_argument("--bep", required=True)
    parser.add_argument("--testlogs")
    parser.add_argument("--logs-out")
    args = parser.parse_args(argv)
    try:
        tests = load(args.bep)
    except (OSError, ValueError) as e:
        print("noisy_report.py: %s" % e, file=sys.stderr)
        return 2
    sys.stdout.write(report(args.name, tests))
    if args.testlogs and args.logs_out:
        os.makedirs(args.logs_out, exist_ok=True)
        copy_logs(tests, args.testlogs, args.logs_out)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
