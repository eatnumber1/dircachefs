#!/usr/bin/env python3
"""Compares two coverage reports of one commit (step 26.14).

  coverage_diff.py [--exact] [--prefix P]... A.dat B.dat
  coverage_diff.py [--exact] [--prefix P]... --per-test DIR_A DIR_B

A test's coverage must depend on nothing but the test, so two runs of
`bazel coverage` on one commit must report the same lines and branches. The
first form compares two lcov files (e.g. each run's
`bazel-out/_coverage/_coverage_report.dat`); the second compares, test by
test, every `coverage.dat` under two copies of `bazel-testlogs` and names the
tests that differ. A line or branch differs when it is covered in one report
and not in the other; with --exact, when its execution counts differ.
Only files under a --prefix (default dcfs/, bench/ and tools/) are compared.

Prints one line per difference and a summary; exits 0 when there is none, 1
when there is, 2 on a usage error.
"""

import argparse
import os
import sys

DEFAULT_PREFIXES = ("dcfs/", "bench/", "tools/")


def parse(text):
    """Returns {file: {("line", n): count, ("branch", n, block, branch): count}}.

    A file listed more than once (several records of one source) has its
    counts added; a branch that was not taken ("-") counts 0.
    """
    files = {}
    current = None
    for raw in text.splitlines():
        if raw.startswith("SF:"):
            current = files.setdefault(raw[3:], {})
        elif raw == "end_of_record":
            current = None
        elif current is None:
            continue
        elif raw.startswith("DA:"):
            fields = raw[3:].split(",")
            key = ("line", int(fields[0]))
            current[key] = current.get(key, 0) + int(fields[1])
        elif raw.startswith("BRDA:"):
            fields = raw[5:].split(",")
            key = ("branch", int(fields[0]), fields[1], fields[2])
            taken = 0 if fields[3] == "-" else int(fields[3])
            current[key] = current.get(key, 0) + taken
    return files


def keep(name, prefixes):
    return any(name.startswith(p) for p in prefixes)


def differences(a, b, exact=False, prefixes=DEFAULT_PREFIXES):
    """Returns sorted (file, key, count_a, count_b) for every difference.

    A line or branch absent from one report has the count None there.
    """
    found = []
    for name in sorted(set(a) | set(b)):
        if not keep(name, prefixes):
            continue
        in_a = a.get(name, {})
        in_b = b.get(name, {})
        for key in sorted(set(in_a) | set(in_b), key=str):
            ca = in_a.get(key)
            cb = in_b.get(key)
            if ca == cb:
                continue
            if not exact and ca is not None and cb is not None and (
                    (ca > 0) == (cb > 0)):
                continue
            found.append((name, key, ca, cb))
    return sorted(found, key=lambda d: (d[0], d[1][1], str(d[1])))


def describe(key):
    if key[0] == "line":
        return "line %d" % key[1]
    return "line %d branch %s.%s" % (key[1], key[2], key[3])


def report(diffs, label, out):
    for name, key, ca, cb in diffs:
        print("%s%s: %s: first %s, second %s" %
              (label, name, describe(key), ca, cb), file=out)


def read(path):
    with open(path, "r", errors="replace") as f:
        return parse(f.read())


def per_test_files(root):
    """Returns {path relative to root: absolute path} of every coverage.dat."""
    found = {}
    for directory, _, names in os.walk(root):
        for name in names:
            if name == "coverage.dat":
                path = os.path.join(directory, name)
                found[os.path.relpath(path, root)] = path
    return found


def main(argv, out=sys.stdout):
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--exact", action="store_true",
                        help="compare execution counts, not covered or not")
    parser.add_argument("--prefix", action="append", default=[],
                        help="compare files under this path (repeatable)")
    parser.add_argument("--per-test", action="store_true",
                        help="the arguments are two copies of bazel-testlogs")
    parser.add_argument("first")
    parser.add_argument("second")
    try:
        args = parser.parse_args(argv)
    except SystemExit as e:
        return 2 if e.code else 0
    prefixes = tuple(args.prefix) or DEFAULT_PREFIXES

    total = 0
    differing_tests = []
    if args.per_test:
        first = per_test_files(args.first)
        second = per_test_files(args.second)
        for rel in sorted(set(first) | set(second)):
            if rel not in first or rel not in second:
                print("%s: only in the %s run" %
                      (os.path.dirname(rel),
                       "first" if rel in first else "second"), file=out)
                differing_tests.append(rel)
                total += 1
                continue
            diffs = differences(read(first[rel]), read(second[rel]),
                                args.exact, prefixes)
            if diffs:
                differing_tests.append(rel)
                total += len(diffs)
                report(diffs, os.path.dirname(rel) + ": ", out)
        print("%d tests compared, %d differ, %d differences" %
              (len(set(first) | set(second)), len(differing_tests), total),
              file=out)
    else:
        diffs = differences(read(args.first), read(args.second), args.exact,
                            prefixes)
        report(diffs, "", out)
        total = len(diffs)
        print("%d differences" % total, file=out)
    return 1 if total else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
