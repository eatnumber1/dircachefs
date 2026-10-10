"""Reads the clang-tidy and clang-query outputs of //tools:style_checks_test.

//tools:style_checks.bzl runs the two tools over our C++ as Bazel actions and
writes `<file>.tidy.txt` and `<file>.query.txt` per source file. This module
turns them into findings, each keyed by (file, function, check) -- never by
line, so an edit above a finding does not disturb the list -- and compares the
keys with tools/style_checks_allow.txt:

    path|function|check|count

The allowlist is the backlog of findings that existed when a check was
switched on. It only shrinks: a key with more findings than its count (or no
line) is a new finding and fails the test; a key with fewer is stale and fails
it too, so whoever fixes a finding takes the line out.

clang-tidy findings are lines `file:line:col: error: message [check]`
(.clang-tidy makes every finding an error). clang-query prints, per match of a
matcher with `.bind("rule")`, `file:line:col:{l1:c1-l2:c2}: note: "rule" binds
here` and the first source line of the node. The matcher named `function`
(tools/style_matchers/00_functions.query) lists every function definition of
ours with its range; the function of a finding is the innermost one whose
range holds its line, named by the identifier before the first `(` of the
definition's first line (`Backing::Open`), or that whole line when there is
none. A finding outside every function is keyed `<file scope>`.

Rules listed in REPORT_ONLY are preferences or waiting for another step: they
are counted and printed, never failed.

A tool that could not parse a file is a failure, and so is a
`clang-diagnostic-*` finding: the file does not compile cleanly under the
flags the build uses.
"""

import collections
import re

OURS = re.compile(r"^(dcfs|tools|bench)/")
FILE_SCOPE = "<file scope>"
FUNCTION_RULE = "function"

# Matcher rules that are reported and not enforced: a preference
# (status_uninitialized, docs/style.md 1.6a) or waiting for a step
# (pointer_without_nullability, 25.16).
REPORT_ONLY = frozenset({"status_uninitialized", "pointer_without_nullability"})

# The matchers (tools/style_matchers/<rule>.query), each with a known-bad and
# a known-good fixture.
MATCHER_RULES = (
    "banned_std",
    "capturing_mutating_lambda",
    "check_in_production",
    "dcfs_qualified_inside_dcfs",
    "happy_path_nested",
    "iterator_pair_algorithm",
    "pointer_without_nullability",
    "status_uninitialized",
)

TIDY_RE = re.compile(
    r"^(?P<file>[^\s:][^:]*):(?P<line>\d+):(?P<col>\d+): "
    r"(?:warning|error): (?P<message>.*) \[(?P<checks>[^\]]+)\]$")
QUERY_RE = re.compile(
    r"^(?P<file>[^\s:][^:]*):(?P<line>\d+):(?P<col>\d+):"
    r"\{(?P<first>\d+):\d+-(?P<last>\d+):\d+\}: "
    r"note: \"(?P<rule>[^\"]+)\" binds here$")
SOURCE_RE = re.compile(r"^\s*\d+ \|(?P<text>.*)$")
# A diagnostic line of clang itself, in either tool's output.
ERROR_RE = re.compile(r": (?:fatal )?error: |^Error while processing")
CALLEE_RE = re.compile(r"((?:operator\W+|~?\w+)(?:::(?:operator\W+|~?\w+))*)\s*\(")


def normalize(path):
    """The path relative to the workspace: no execroot prefix, no `./`."""
    marker = "execroot/_main/"
    if marker in path:
        path = path.split(marker, 1)[1]
    return path[2:] if path.startswith("./") else path


Finding = collections.namedtuple("Finding", "file line col rule message")


def function_name(first_line):
    """`Backing::Open` for `absl::Status Backing::Open(const Path &p) {`."""
    match = CALLEE_RE.search(first_line)
    return match.group(1) if match else first_line.strip()


def parse_tidy(text):
    """(findings, failures) of one clang-tidy output."""
    findings = []
    failures = []
    for line in text.splitlines():
        match = TIDY_RE.match(line)
        if match:
            for check in match.group("checks").split(","):
                if check == "-warnings-as-errors":
                    continue
                findings.append(
                    Finding(normalize(match.group("file")),
                            int(match.group("line")), int(match.group("col")),
                            check, match.group("message")))
        elif ERROR_RE.search(line):
            failures.append(line)
    return findings, failures


def parse_query(text):
    """(findings, functions, failures) of one clang-query output.

    functions is a list of (file, first line, last line, name)."""
    findings = []
    functions = []
    failures = []
    lines = text.splitlines()
    for number, line in enumerate(lines):
        match = QUERY_RE.match(line)
        if not match:
            if ERROR_RE.search(line):
                failures.append(line)
            continue
        path = normalize(match.group("file"))
        rule = match.group("rule")
        if rule == FUNCTION_RULE:
            source = SOURCE_RE.match(lines[number + 1])
            functions.append((path, int(match.group("first")),
                              int(match.group("last")),
                              function_name(source.group("text"))))
            continue
        # A matcher also binds the nodes it names inside itself (a range, a
        # captured variable); only the rule's own binding is a finding.
        if rule in MATCHER_RULES:
            findings.append(
                Finding(path, int(match.group("line")),
                        int(match.group("col")), rule, ""))
    return findings, functions, failures


def enclosing_function(functions, path, line):
    """The innermost function of `path` whose range holds `line`."""
    best = None
    for (file, first, last, name) in functions:
        if file == path and first <= line <= last:
            if best is None or first >= best[0]:
                best = (first, name)
    return best[1] if best else FILE_SCOPE


def read_outputs(paths):
    """Findings (deduplicated across the files that include one header),
    the failures, and the matches of the fixtures' `function` listing."""
    findings = set()
    functions = set()
    failures = []
    for path in paths:
        with open(path, encoding="utf-8", errors="replace") as f:
            text = f.read()
        if path.endswith(".tidy.txt"):
            found, bad = parse_tidy(text)
            more = []
        else:
            found, more, bad = parse_query(text)
        findings.update(f for f in found if OURS.match(f.file))
        functions.update(more)
        failures += ["%s: %s" % (path, line) for line in bad]
    failures += [
        "%s:%d: %s: the file does not compile cleanly under the build's flags"
        % (f.file, f.line, f.rule)
        for f in findings
        if f.rule.startswith("clang-diagnostic-")
    ]
    findings = [f for f in findings if not f.rule.startswith("clang-diagnostic-")]
    return sorted(findings), functions, sorted(failures)


def keyed(findings, functions):
    """{(file, function, check): count}."""
    counts = collections.Counter()
    for f in findings:
        counts[(f.file, enclosing_function(functions, f.file, f.line),
                f.rule)] += 1
    return counts


def parse_allowlist(text):
    """{(file, function, check): count}, for the lines of the allowlist."""
    allowed = {}
    for line in text.splitlines():
        line = line.strip()
        if not line or line.startswith("#"):
            continue
        # A function name may hold `|` (operator|), a path and a check not.
        path, rest = line.split("|", 1)
        function, check, count = rest.rsplit("|", 2)
        allowed[(path, function, check)] = int(count)
    return allowed


def format_line(key, count):
    return "%s|%s|%s|%d" % (key[0], key[1], key[2], count)


def compare(counts, allowed):
    """Problems: a key with more findings than allowed, and a stale line."""
    problems = []
    for key, count in sorted(counts.items()):
        known = allowed.get(key, 0)
        if count > known:
            problems.append(
                "new: %s (%d known) -- fix it; do not add it to "
                "tools/style_checks_allow.txt" % (format_line(key, count),
                                                   known))
    for key, known in sorted(allowed.items()):
        if counts.get(key, 0) < known:
            problems.append(
                "stale: %s now has %d: lower or delete the line (the list "
                "only shrinks)" % (format_line(key, known), counts.get(key, 0)))
    return problems


def split_report_only(counts):
    enforced = {k: v for k, v in counts.items() if k[2] not in REPORT_ONLY}
    report = {k: v for k, v in counts.items() if k[2] in REPORT_ONLY}
    return enforced, report


HIT_RE = re.compile(r"//\s*HIT(?:\s+([a-z][a-z0-9_-]*))?\s*$")
FIXTURE_RE = re.compile(
    r"^(?:.*/)?(?P<rule>[a-z_]+?)(?P<kind>_bad|_good|_exempt_test|_exempt)?\.cc$")


def fixture_problems(findings, sources):
    """Problems of the matchers' fixtures against what the tools found.

    `sources` is {path: text} of tools/style_matchers/**/*.cc. A line ending
    in `// HIT` (or `// HIT <check>` in tidy_bad.cc) is a finding of the
    file's rule (or of that check) the tools must report. A *_bad.cc file of a
    matcher must have exactly its marked reports for that rule; a *_good.cc
    or *_exempt*.cc file none of that rule; tidy_good.cc no finding at all;
    tidy_bad.cc at least the marked ones."""
    found = collections.defaultdict(set)
    for f in findings:
        found[f.file].add((f.line, f.rule))
    problems = []
    for path, text in sorted(sources.items()):
        match = FIXTURE_RE.match(path)
        rule, kind = match.group("rule"), match.group("kind") or ""
        marked = set()
        for number, line in enumerate(text.splitlines(), 1):
            hit = HIT_RE.search(line)
            if hit:
                marked.add((number, hit.group(1) or rule))
        actual = found[path]
        if rule == "tidy":
            if kind == "_good":
                problems += ["%s:%d: %s: tidy_good.cc must have no finding"
                             % (path, n, r) for (n, r) in sorted(actual)]
            else:
                problems += ["%s:%d: the fixture marks %s and the tool does "
                             "not report it" % (path, n, r)
                             for (n, r) in sorted(marked - actual)]
            continue
        if rule not in MATCHER_RULES:
            problems.append("%s: no matcher named %s" % (path, rule))
            continue
        reports = {(n, r) for (n, r) in actual if r == rule}
        if kind == "_bad":
            problems += ["%s:%d: %s does not report the marked line"
                         % (path, n, r) for (n, r) in sorted(marked - reports)]
            problems += ["%s:%d: %s reports a line that is not marked"
                         % (path, n, r) for (n, r) in sorted(reports - marked)]
        else:
            problems += ["%s:%d: %s reports in a file it must not"
                         % (path, n, r) for (n, r) in sorted(reports)]
    return problems


def matcher_file_problems(query_texts):
    """Every matcher file {name: text} is a known rule and begins with a
    comment that names the style section it enforces; every rule has a file."""
    problems = []
    for name, text in sorted(query_texts.items()):
        if name == "00_functions":
            continue
        if name not in MATCHER_RULES:
            problems.append("tools/style_matchers/%s.query is not in "
                            "MATCHER_RULES" % name)
        header = text.split("\nmatch ", 1)[0].split("\nlet ", 1)[0]
        if "docs/style.md" not in header:
            problems.append("tools/style_matchers/%s.query: the leading "
                            "comment must name the docs/style.md section it "
                            "enforces" % name)
    problems += ["tools/style_matchers/%s.query is missing" % rule
                 for rule in MATCHER_RULES if rule not in query_texts]
    return problems


def tidy_denials(text):
    """Problems with the deny-list of a .clang-tidy: every `-check` of Checks
    has a line `#   check: reason` above it."""
    match = re.search(r"^Checks:\s*(?:>-?\s*)?'?(.*?)'?\s*^\w", text,
                      re.MULTILINE | re.DOTALL)
    if not match:
        return [".clang-tidy has no Checks"]
    denied = re.findall(r"(?:^|[,\s])-([a-z][a-z0-9-]*)(?=[,\s]|$)",
                        match.group(1))
    problems = []
    for check in denied:
        if not re.search(r"^#\s+%s: \S" % re.escape(check), text, re.MULTILINE):
            problems.append(
                ".clang-tidy denies %s with no `#   %s: reason` line"
                % (check, check))
    return problems


def run(output_paths, allowlist_text, tidy_config_text):
    """(problems, report lines)."""
    findings, functions, failures = read_outputs(output_paths)
    counts = keyed(findings, functions)
    enforced, report = split_report_only(counts)
    problems = (failures + tidy_denials(tidy_config_text) +
                compare(enforced, parse_allowlist(allowlist_text)))
    per_rule = collections.Counter()
    for key, count in counts.items():
        per_rule[key[2]] += count
    lines = ["%6d %s%s" % (count, rule,
                           " (report only)" if rule in REPORT_ONLY else "")
             for rule, count in sorted(per_rule.items())]
    lines += ["report-only: " + format_line(k, v)
              for k, v in sorted(report.items())]
    return problems, lines
