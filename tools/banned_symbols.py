"""Banned symbols in the shipped binary (plan step 26.8).

The deny list (banned_symbols.txt) names symbols the shipped binary must not
reference, each with the rule it enforces. A violation is

- an object that was linked into the binary (first party or a library) and
  references a banned symbol (`nm -u`), unless an `allow` line covers that
  origin and symbol; or
- a banned symbol defined in the linked binary that no scanned object
  references (so something outside the Bazel graph, glibc or libstdc++, pulled
  it in).

Line format of the deny list (`#` starts a comment, `|` separates fields):

  ban SYMBOL_GLOB | rule
  allow ORIGIN_GLOB SYMBOL_GLOB | reason

SYMBOL_GLOB is an fnmatch pattern over the (mangled) symbol name without a
version suffix; ORIGIN_GLOB one over an object's path ("dcfs/_objs/...",
"libfuse+/_objs/..."); the origin `<runtime>` covers a symbol that is
defined in the binary but referenced by no scanned object.
"""

import argparse
import fnmatch
import subprocess
import sys


# The origin of a symbol the C or C++ runtime (glibc, libstdc++) pulls in on
# its own, which no scanned object references.
RUNTIME = "<runtime>"


class DenyListError(Exception):
    """The deny list is malformed."""


def parse_deny_list(text):
    """Returns (bans, allows): [(glob, rule)] and [(origin, glob, reason)]."""
    bans = []
    allows = []
    for number, line in enumerate(text.splitlines(), 1):
        line = line.split("#", 1)[0].strip()
        if not line:
            continue
        head, bar, why = (part.strip() for part in line.partition("|"))
        words = head.split()
        if not bar or not why:
            raise DenyListError("line %d: no '| rule' or reason" % number)
        if len(words) == 2 and words[0] == "ban":
            bans.append((words[1], why))
        elif len(words) == 3 and words[0] == "allow":
            allows.append((words[1], words[2], why))
        else:
            raise DenyListError("line %d: not 'ban GLOB' or "
                                "'allow ORIGIN GLOB': %r" % (number, head))
    return bans, allows


def nm_symbols(nm, path, *flags):
    """Returns the names `nm` prints for `path`, version suffix removed."""
    out = subprocess.run([nm, "-P", *flags, path], check=True,
                         capture_output=True, text=True).stdout
    names = set()
    for line in out.splitlines():
        fields = line.split()
        if len(fields) >= 2:
            names.add(fields[0].split("@", 1)[0])
    return names


def rule_of(bans, symbol):
    """The rule of the first ban matching `symbol`, or None."""
    for glob, rule in bans:
        if fnmatch.fnmatchcase(symbol, glob):
            return rule
    return None


def allowed(allows, origin, symbol, used=None):
    """True if an allow line covers `symbol` from `origin`; records it."""
    hit = False
    for entry in allows:
        if (fnmatch.fnmatchcase(origin, entry[0]) and
                fnmatch.fnmatchcase(symbol, entry[1])):
            hit = True
            if used is not None:
                used.add(entry)
    return hit


def check(nm, objects, binary, deny_text, used=None):
    """Returns a list of violation messages (empty: the binary is clean).

    `objects` is the origin names and file paths of the linked objects:
    [(origin, path)]. If `used` is a set, the allow lines that silenced
    something are added to it.
    """
    bans, allows = parse_deny_list(deny_text)
    problems = []
    referenced = set()
    for origin, path in objects:
        for symbol in sorted(nm_symbols(nm, path, "--undefined-only")):
            rule = rule_of(bans, symbol)
            if rule is None:
                continue
            referenced.add(symbol)
            if allowed(allows, origin, symbol, used):
                continue
            problems.append("banned symbol %s referenced by %s: %s"
                            % (symbol, origin, rule))
    for symbol in sorted(nm_symbols(nm, binary, "--defined-only")):
        rule = rule_of(bans, symbol)
        if (rule is not None and symbol not in referenced and
                not allowed(allows, RUNTIME, symbol, used)):
            problems.append("banned symbol %s is in the linked binary but no "
                            "scanned object references it (pulled in by the "
                            "C or C++ runtime): %s" % (symbol, rule))
    return problems


def read_objects(manifest):
    """[(origin, path)] of a linked_objects manifest (first_party_objects.bzl).

    Paths are runfiles-relative: external repositories are at `../<repo>/`.
    """
    objects = []
    with open(manifest, encoding="utf-8") as f:
        for line in f:
            origin = line.strip()
            if origin:
                objects.append((origin.removeprefix("../"), origin))
    return objects


def main(argv):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--nm", required=True)
    parser.add_argument("--deny_list", required=True)
    parser.add_argument("--objects", required=True,
                        help="manifest written by linked_objects")
    parser.add_argument("--binary", required=True)
    args = parser.parse_args(argv)
    with open(args.deny_list, encoding="utf-8") as f:
        deny_text = f.read()
    problems = check(args.nm, read_objects(args.objects), args.binary,
                     deny_text)
    for problem in problems:
        print(problem, file=sys.stderr)
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
