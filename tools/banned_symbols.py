# SPDX-License-Identifier: MIT
"""Banned symbols in the shipped binary (plan step 26.8).

The deny list (banned_symbols.txt) names symbols the shipped binary must not
reference, each with the rule it enforces. A violation is

- an object that was linked into the binary (first party or a library) and
  references a banned symbol (`nm -u`), unless an `allow` line covers that
  origin and symbol; or
- a banned symbol defined in the linked binary that no scanned object
  references (so something outside the Bazel graph, glibc or libc++, pulled
  it in).

Line format of the deny list (`#` starts a comment, `|` separates fields):

  ban SYMBOL_GLOB | rule
  allow ORIGIN_GLOB SYMBOL_GLOB | reason
  allow ORIGIN_GLOB SYMBOL_GLOB via FUNCTION_GLOB | reason

SYMBOL_GLOB is an fnmatch pattern over the (mangled) symbol name without a
version suffix; ORIGIN_GLOB one over an object's path ("dcfs/_objs/...",
"libfuse+/_objs/..."); the origin `<runtime>` covers a symbol that is
defined in the binary but referenced by no scanned object. A line with `via`
covers only the references made from the code of the object's functions
that FUNCTION_GLOB matches (by mangled name, from the object's disassembly):
every object of ours references __assert_fail from the Abseil header
functions compiled into it, and that line lets the same symbol from a
function of ours stay a violation.
"""

import argparse
import collections
import fnmatch
import functools
import re
import subprocess
import sys


# The origin of a symbol the C or C++ runtime (glibc, libc++) pulls in on
# its own, which no scanned object references.
RUNTIME = "<runtime>"


class DenyListError(Exception):
    """The deny list is malformed."""


def parse_deny_list(text):
    """Returns (bans, allows): [(glob, rule)] and [(origin, glob, via,
    reason)], `via` None on a line without one."""
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
            allows.append((words[1], words[2], None, why))
        elif len(words) == 5 and words[0] == "allow" and words[3] == "via":
            allows.append((words[1], words[2], words[4], why))
        else:
            raise DenyListError("line %d: not 'ban GLOB', 'allow ORIGIN GLOB' "
                                "or 'allow ORIGIN GLOB via FUNCTION': %r"
                                % (number, head))
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


def record(used, entries):
    if used is not None:
        used.update(entries)


def covering(allows, origin, symbol):
    """The allow lines whose origin and symbol globs match."""
    return [entry for entry in allows
            if fnmatch.fnmatchcase(origin, entry[0]) and
            fnmatch.fnmatchcase(symbol, entry[1])]


# llvm-objdump -dr: a function's first line, and a relocation line (the
# symbol, then an addend such as -0x4; mangled and C names have no + or -).
FUNCTION_RE = re.compile(r"^[0-9a-f]+ <(?P<name>[^>]+)>:$")
RELOCATION_RE = re.compile(r"\sR_\w+\s+(?P<symbol>[^\s+-]+)")


@functools.cache
def referrers_by_symbol(objdump, path):
    """{symbol: the functions of the object at `path` whose code has a
    relocation against it}, one disassembly per object."""
    out = subprocess.run([objdump, "-dr", "--no-show-raw-insn", path],
                         check=True, capture_output=True, text=True).stdout
    functions = collections.defaultdict(set)
    function = None
    for line in out.splitlines():
        header = FUNCTION_RE.match(line)
        if header:
            function = header.group("name")
            continue
        relocation = RELOCATION_RE.search(line)
        if relocation:
            functions[relocation.group("symbol")].add(function)
    return functions


# llvm-objdump -r: the section the relocations below it apply to.
SECTION_RE = re.compile(r"^RELOCATION RECORDS FOR \[(?P<section>[^\]]+)\]:$")


@functools.cache
def data_referrers_by_symbol(objdump, path):
    """{symbol: the sections of the object at `path` other than code
    (.text*) with a relocation against it}: a function pointer in data."""
    out = subprocess.run([objdump, "-r", path], check=True,
                         capture_output=True, text=True).stdout
    sections = collections.defaultdict(set)
    section = None
    for line in out.splitlines():
        header = SECTION_RE.match(line)
        if header:
            section = header.group("section")
            continue
        relocation = RELOCATION_RE.search(line)
        if relocation and not section.startswith(".text"):
            sections[relocation.group("symbol")].add(section)
    return sections


def uncovered(objdump, allows, origin, path, symbol, used):
    """Where the object `origin` references `symbol` without an allow line
    covering it: [] when covered, otherwise " in FUNCTION" per function and
    " from data (SECTION)" per data section, or one "" for the object as a
    whole. The functions and sections are looked up only when lines with
    `via` are all that match; no `via` covers a reference from data, which
    no function makes."""
    lines = covering(allows, origin, symbol)
    whole = [entry for entry in lines if entry[2] is None]
    record(used, whole)
    if whole:
        return []
    if not lines:
        return [""]
    left = [" from data (%s)" % section for section in sorted(
        data_referrers_by_symbol(objdump, path).get(symbol, set()))]
    functions = referrers_by_symbol(objdump, path).get(symbol, set())
    for function in sorted(functions):
        hits = [entry for entry in lines
                if fnmatch.fnmatchcase(function, entry[2])]
        record(used, hits)
        if not hits:
            left.append(" in " + function)
    return left


def check(nm, objdump, objects, binary, deny_text, used=None):
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
            for where in uncovered(objdump, allows, origin, path, symbol,
                                   used):
                problems.append("banned symbol %s referenced by %s%s: %s"
                                % (symbol, origin, where, rule))
    for symbol in sorted(nm_symbols(nm, binary, "--defined-only")):
        rule = rule_of(bans, symbol)
        if rule is None or symbol in referenced:
            continue
        # Nothing scanned references it, so there is no function to match:
        # only a line without `via` covers it.
        whole = [entry for entry in covering(allows, RUNTIME, symbol)
                 if entry[2] is None]
        record(used, whole)
        if not whole:
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
    parser.add_argument("--objdump", required=True)
    parser.add_argument("--deny_list", required=True)
    parser.add_argument("--objects", required=True,
                        help="manifest written by linked_objects")
    parser.add_argument("--binary", required=True)
    args = parser.parse_args(argv)
    with open(args.deny_list, encoding="utf-8") as f:
        deny_text = f.read()
    problems = check(args.nm, args.objdump, read_objects(args.objects),
                     args.binary, deny_text)
    for problem in problems:
        print(problem, file=sys.stderr)
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
