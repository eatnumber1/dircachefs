# SPDX-License-Identifier: MIT
"""The approved macros (tools/approved_macros.txt) and the .clang-tidy that
checks them (plan step 25.58, docs/style.md 1.2 rule-macros-discouraged).

Two checks, independent of each other. The approval policy is the scan of the
C++ sources (scan_problems): every macro they define is a structural header
guard or on the list, and every list entry is defined somewhere. clang-tidy's
cppcoreguidelines-macro-usage reports only the macros another language
feature could express, so its AllowedRegexp holds just the names with an
allowance (the list's fourth field), and readability-identifier-naming wants
the DCFS_ prefix on every macro its MacroDefinitionIgnoredRegexp does not
name. Both regexes come from the one list: `bazel run //tools:approved_macros
-- --write` rewrites them in .clang-tidy, and //tools:approved_macros_test
fails when they differ from what the list gives.
"""

import os
import re
import sys

ALLOWED_KEY = "cppcoreguidelines-macro-usage.AllowedRegexp"
IGNORED_KEY = "readability-identifier-naming.MacroDefinitionIgnoredRegexp"
# Header guards are exempt from the prefix and case rule in general (clang's
# cppcoreguidelines-macro-usage already skips them), so no list entry names one.
# readability-identifier-naming needs a regex to skip them; scan_problems'
# structural guard check (the file's first two directives) is what keeps a
# non-guard name ending in _H_ out.
GUARDS = "[A-Z][A-Z0-9_]*_H_"
DEFINE = re.compile(r"^\s*#\s*define\s+(\w+)", re.MULTILINE)
DIRECTIVE = re.compile(r"^[ \t]*#[ \t]*(\w+)[ \t]*(\w*)([^\n]*)", re.MULTILINE)
KINDS = ("prefixed", "exempt", "third_party")
IDENTIFIER = re.compile(r"^[A-Za-z_][A-Za-z0-9_]*$")


def parse(text):
    """[(name, kind, date, allowance, grant)] of the list; ValueError."""
    entries, seen = [], set()
    for number, line in enumerate(text.splitlines(), 1):
        if not line.strip() or line.startswith("#"):
            continue
        parts = line.split("|", 4)
        if len(parts) != 5:
            raise ValueError("line %d: want name|kind|date|allowance|grant"
                             % number)
        name, prefix, date, allowance, grant = parts
        if not IDENTIFIER.match(name):
            raise ValueError("line %d: %r is not an identifier"
                             % (number, name))
        if prefix not in KINDS:
            raise ValueError("line %d: kind is one of %s"
                             % (number, ", ".join(KINDS)))
        if prefix == "prefixed" and not name.startswith("DCFS_"):
            raise ValueError("line %d: %s is marked prefixed but does not "
                             "start with DCFS_" % (number, name))
        if not re.fullmatch(r"\d{4}-\d\d-\d\d", date) or not grant.strip():
            raise ValueError("line %d: want a date and russ's words" % number)
        if name in seen:
            raise ValueError("line %d: %s is listed twice" % (number, name))
        seen.add(name)
        entries.append((name, prefix, date, allowance, grant))
    return entries


def alternation(names):
    return "'^(%s)$'" % "|".join(sorted(names))


def options(entries):
    """{key: value} of the two options the list determines."""
    return {
        ALLOWED_KEY: alternation(e[0] for e in entries if e[3]),
        IGNORED_KEY: alternation([GUARDS] + [e[0] for e in entries
                                             if e[1] != "prefixed"]),
    }


def current(config_text):
    """{key: value} of the same two options in .clang-tidy."""
    found = {}
    for line in config_text.splitlines():
        for key in (ALLOWED_KEY, IGNORED_KEY):
            if line.strip().startswith(key + ": "):
                found[key] = line.strip()[len(key) + 2:]
    return found


def problems(entries, config_text):
    want, have = options(entries), current(config_text)
    return ["%s: .clang-tidy has %s, tools/approved_macros.txt gives %s; run "
            "`bazel run //tools:approved_macros -- --write`"
            % (key, have.get(key, "nothing"), want[key])
            for key in want if have.get(key) != want[key]]


def guard_of(text):
    """The name of the header guard of `text`: `#ifndef N` immediately
    followed by `#define N` with no body, the first two directives, and the
    last directive an `#endif`; else None."""
    found = DIRECTIVE.findall(text)
    if len(found) < 3 or found[-1][0] != "endif":
        return None
    first, name, _ = found[0]
    second, defined, rest = found[1]
    if first == "ifndef" and second == "define" and defined == name \
            and not rest.split("//")[0].split("/*")[0].strip():
        return name
    return None


def scan_problems(files, entries):
    """Problems of the macros {path: text} define against the list.

    cppcoreguidelines-macro-usage reports only function-like, variadic and
    all-literal macros, so this scan is what enforces approval: a definition
    that is not a guard and not on the list is reported, and so is a list
    entry that nothing defines."""
    listed = {e[0] for e in entries}
    defined = set()
    problems = []
    for path, text in sorted(files.items()):
        guard = guard_of(text)
        for name in DEFINE.findall(text):
            if name == guard:
                continue
            defined.add(name)
            if name not in listed:
                problems.append(
                    "%s: macro %s is not in tools/approved_macros.txt: a new "
                    "macro needs russ's approval; report it instead of "
                    "adding it to the list" % (path, name))
    problems += ["tools/approved_macros.txt: %s is listed but defined "
                 "nowhere: take it out" % name
                 for name in sorted(listed - defined)]
    return problems


def write(entries, config_text):
    want = options(entries)
    lines = []
    for line in config_text.splitlines():
        for key, value in want.items():
            if line.strip().startswith(key + ": "):
                line = "  %s: %s" % (key, value)
        lines.append(line)
    return "\n".join(lines) + "\n"


def main(argv):
    root = os.environ.get("BUILD_WORKSPACE_DIRECTORY")
    if argv[1:] != ["--write"] or root is None:
        print("usage: bazel run //tools:approved_macros -- --write")
        return 2
    with open(os.path.join(root, "tools/approved_macros.txt")) as f:
        entries = parse(f.read())
    path = os.path.join(root, ".clang-tidy")
    with open(path) as f:
        text = f.read()
    with open(path, "w") as f:
        f.write(write(entries, text))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
