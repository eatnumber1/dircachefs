#!/usr/bin/env python3
"""Compares two builds of one output byte for byte (step 26.12).

  repro_compare.py A B [A B ...]

Exits 0 when every pair is identical. For a pair that differs it prints the
sizes, the number of differing bytes and the printable strings (6 or more
characters) present in one file and not the other, which is where an embedded
path, host name or timestamp shows up, and exits 1.
"""

import re
import sys

STRING = re.compile(rb"[\x20-\x7e]{6,}")


def strings(data):
    return set(STRING.findall(data))


def differing_bytes(a, b):
    n = min(len(a), len(b))
    return sum(1 for i in range(n) if a[i] != b[i]) + abs(len(a) - len(b))


def compare(path_a, path_b, out=sys.stderr):
    """Returns True when the files are identical, else reports and returns False."""
    with open(path_a, "rb") as f:
        a = f.read()
    with open(path_b, "rb") as f:
        b = f.read()
    if a == b:
        return True
    print("NOT REPRODUCIBLE: %s (%d bytes) and %s (%d bytes) differ in %d bytes" %
          (path_a, len(a), path_b, len(b), differing_bytes(a, b)), file=out)
    only_a = sorted(strings(a) - strings(b))
    only_b = sorted(strings(b) - strings(a))
    for tag, only in (("only in the first", only_a), ("only in the second", only_b)):
        for s in only[:20]:
            print("  %s: %s" % (tag, s.decode("ascii")), file=out)
        if len(only) > 20:
            print("  %s: ... and %d more strings" % (tag, len(only) - 20), file=out)
    return False


def main(argv):
    if len(argv) < 2 or len(argv) % 2:
        print(__doc__, file=sys.stderr)
        return 2
    ok = True
    for i in range(0, len(argv), 2):
        ok = compare(argv[i], argv[i + 1]) and ok
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
