# SPDX-License-Identifier: MIT
"""Repository-shape rules (plan step 26.13).

Each function takes the root of a source tree and returns a list of problems
(empty: the rule holds):

- third_party_readmes: every third_party/<name>/ directory has a README.md;
- guest_scripts_used: every test/qemu/guest/*.sh is used by a test in
  test/qemu/BUILD.bazel, unless it is `lib.sh`, `pjdfstest_lib.sh`,
  `init`, `systemd_install.sh` or `systemd_run.sh` (the last two are run
  by `init` and by the systemd guest's unit, not by a test target), or
  another guest script sources it (a helper library);
- guest_sleeps: a bare `sleep` in a test/qemu/guest script waits for a
  timer, not for the event it is after (docs/style.md, "No timers"): none
  outside `justified_sleep` in lib.sh, except the known ones counted per file
  in tools/repo_shape_sleeps.txt, a list that only shrinks (a count there
  that is higher than the file's, or lower, is a problem: new sleeps are
  refused, removed ones must be taken off);
- disabled_checks_listed: every `disabled NAME ...` check of the guest
  scripts is named in README.md's Limitations section;
- fixed_arrays: a local buffer sized once (`std::vector<T> v(n);`,
  `std::string s(n, '\0');`) in dcfs/*.h, dcfs/*.cc and tools/*.cc is an
  absl::FixedArray (docs/style.md, 1.2), except the known ones counted per
  file in tools/repo_shape_fixed_arrays.txt, a list that only shrinks;
- no_new_guest_shell: every `*.sh` under test/qemu/ is listed in
  tools/repo_shape_guest_shell.txt, a list that only shrinks (step 25.34:
  guest tests are C++, test/qemu/guest/guest.h; shell is thin glue);
- no_test_only_comments: no production source (dcfs/ and bench/, not
  *_test.cc and not testonly/) has a comment that gives tests as the reason
  for code, or a ForTest / _for_test name (docs/style.md, "No test-only
  things in production code", step 25.7);
- rule_index: every ratified rule is in exactly one rule index. A rule in
  docs/style.md, docs/design.md, docs/plan/process.md or CLAUDE.md is marked
  by an anchor `<a id="rule-NAME"></a>`; exactly one docs/rules/index-*.md
  links to `/<file>#rule-NAME` for every marker, every such link has its
  marker, and AGENTS.md links to every index (russ, 2026-10-18);
- agent_file_attributions: .claude/agents/*.md, .claude/skills/**/*.md and
  CLAUDE.md restate no rule with a "russ, 20NN-" attribution beyond the
  role-only ones counted in tools/repo_shape_attributions.txt.
- long_lines: no line over 80 columns in *.md, *.py, *.sh, *.bzl,
  BUILD.bazel or .bazelrc (not third_party/; C++ is clang-format's), except
  fenced code and table rows in Markdown, link definitions, a line made long
  by one unbreakable token, and the known ones counted per file in
  tools/repo_shape_long_lines.txt, a list that only shrinks (step 25.45);
- split_code_spans: no Markdown backtick code span runs over a line break
  (it renders with a space), except those counted in
  tools/repo_shape_split_spans.txt, a list that only shrinks.

`python3 -I tools/repo_shape.py [root]` prints the problems of every rule
above (the same list, `CHECKS`, as //tools:repo_shape_test) and exits 1 if
there are any (step 25.51).

(The fourth rule, commit subjects, needs git: .github/ci/commit_subjects.sh.)
"""

import os
import re
import subprocess
import sys

NOT_TESTS = frozenset({
    "lib.sh", "pjdfstest_lib.sh", "init", "systemd_install.sh",
    "systemd_run.sh"
})


def read(root, *parts):
    with open(os.path.join(root, *parts), encoding="utf-8") as f:
        return f.read()


def guest_scripts(root):
    guest = os.path.join(root, "test", "qemu", "guest")
    return sorted(n for n in os.listdir(guest) if n.endswith(".sh"))


def third_party_readmes(root):
    problems = []
    base = os.path.join(root, "third_party")
    for name in sorted(os.listdir(base)):
        if (os.path.isdir(os.path.join(base, name)) and
                not os.path.isfile(os.path.join(base, name, "README.md"))):
            problems.append(
                "third_party/%s/ has no README.md (docs/style.md, Bazel: a "
                "pin's README says what it is and how to update it)" % name)
    return problems


def guest_scripts_used(root):
    build = read(root, "test", "qemu", "BUILD.bazel")
    used = set()
    templates = []
    for path in re.findall(r'guest_script\s*=\s*"guest/([^"]+\.sh)"', build):
        used.add(path)
    for template in re.findall(r'guest_script\s*=\s*"guest/([^"]*%s[^"]*\.sh)"',
                               build):
        templates.append(re.compile(
            "^" + re.escape(template).replace("%s", "[a-z0-9_]+") + "$"))
    scripts = guest_scripts(root)
    sourced = set()
    for name in scripts:
        text = read(root, "test", "qemu", "guest", name)
        sourced.update(re.findall(
            r'^\s*\.\s+"\$\(dirname "\$0"\)/([A-Za-z0-9_.]+\.sh)"', text,
            re.MULTILINE))
    problems = []
    for name in scripts:
        if (name in NOT_TESTS or name in used or name in sourced or
                any(t.match(name) for t in templates)):
            continue
        problems.append(
            "test/qemu/guest/%s is used by no test in test/qemu/BUILD.bazel "
            "(a guest_script, or a library another guest script sources)"
            % name)
    return problems


SLEEPS_LIST = ("tools", "repo_shape_sleeps.txt")
# `sleep` as a command with an argument (a number, or an expansion or a
# quoted one), not the word in a list of commands or in a message.
SLEEP_RE = re.compile(r'(^|[\s;&|(`])sleep\s+["$0-9]')


def count_sleeps(text):
    """The bare sleeps of a shell script: not comments, not justified_sleep."""
    count = 0
    in_helper = False
    for line in text.splitlines():
        stripped = line.strip()
        if stripped.startswith("justified_sleep()"):
            in_helper = True
        elif in_helper and line.startswith("}"):
            in_helper = False
            continue
        if in_helper or stripped.startswith("#"):
            continue
        if SLEEP_RE.search(line.split(" #", 1)[0]):
            count += 1
    return count


def known_sleeps(root):
    """{path: (count, reason)} from tools/repo_shape_sleeps.txt."""
    known = {}
    for line in read(root, *SLEEPS_LIST).splitlines():
        line = line.strip()
        if not line or line.startswith("#"):
            continue
        head, _, reason = line.partition("|")
        path, count = head.split()
        known[path] = (int(count), reason.strip())
    return known


def guest_sleeps(root):
    known = known_sleeps(root)
    problems = []
    guest = os.path.join(root, "test", "qemu", "guest")
    actual = {}
    for name in sorted(os.listdir(guest)):
        if not (name.endswith(".sh") or name == "init"):
            continue
        path = "test/qemu/guest/" + name
        count = count_sleeps(read(root, "test", "qemu", "guest", name))
        if count:
            actual[path] = count
    for path, count in actual.items():
        allowed = known.get(path, (0, ""))[0]
        if count > allowed:
            problems.append(
                "%s has %d bare sleep(s), %d known: wait on the event "
                "(a pidfd, a lock, a blocking read on a fifo, `wait`), not on "
                "a timer (docs/style.md, \"No timers\")" % (path, count, allowed))
    for path, (count, _) in known.items():
        if actual.get(path, 0) < count:
            problems.append(
                "tools/repo_shape_sleeps.txt lists %d sleep(s) for %s, which "
                "has %d: lower it (the list only shrinks)" %
                (count, path, actual.get(path, 0)))
    return problems


GUEST_SHELL_LIST = ("tools", "repo_shape_guest_shell.txt")


def guest_shell_files(root):
    """The *.sh files under test/qemu/, as paths relative to the root."""
    found = []
    base = os.path.join(root, "test", "qemu")
    for directory, _, names in os.walk(base):
        for name in names:
            if name.endswith(".sh"):
                found.append(
                    os.path.relpath(os.path.join(directory, name), root))
    return sorted(found)


def no_new_guest_shell(root):
    listed = set(
        line.strip()
        for line in read(root, *GUEST_SHELL_LIST).splitlines()
        if line.strip() and not line.startswith("#"))
    actual = set(guest_shell_files(root))
    problems = []
    for path in sorted(actual - listed):
        problems.append(
            "%s is new shell under test/qemu/: write the test in C++ with "
            "the helpers of test/qemu/guest/guest.h (docs/style.md, "
            "Shell); the list tools/repo_shape_guest_shell.txt does not "
            "grow" % path)
    for path in sorted(listed - actual):
        problems.append(
            "tools/repo_shape_guest_shell.txt lists %s, which is gone: take "
            "it off (the list only shrinks)" % path)
    return problems


def disabled_check_names(root):
    names = set()
    for name in guest_scripts(root):
        names.update(re.findall(
            r"^[ \t]*disabled[ \t]+([A-Za-z0-9_.-]+)", read(
                root, "test", "qemu", "guest", name), re.MULTILINE))
    return sorted(names)


def limitations(readme):
    match = re.search(r"^## Limitations\n(.*?)(?=^## |\Z)", readme,
                      re.MULTILINE | re.DOTALL)
    return match.group(1) if match else ""


def disabled_checks_listed(root):
    section = limitations(read(root, "README.md"))
    return [
        "DISABLED_%s (test/qemu/guest) is not named in README.md's "
        "Limitations section" % name
        for name in disabled_check_names(root)
        if name not in section
    ]


# Phrases that give tests as the reason for code. Matched case-insensitively
# against comments (and, for the names, against code too). "The test suite"
# and "the testonly builds" are sentences about the suite, not reasons.
TEST_REASON = re.compile(
    r"\bonly (?:in|for|by|used (?:in|by|for)) (?:the |unit |these |our )?tests?"
    r"\b"
    r"|\bfor (?:the |unit |these |our )?tests?\b(?!\s+suite)"
    r"|\btest[- ]only\b"
    r"|\bfor testing\b"
    r"|\b(?:so|in order) (?:that )?(?:a |the |unit )?tests? "
    r"(?:can|may|could|are|is)\b"
    r"|\ba tests? that\b"
    r"|ForTest|_for_test",
    re.IGNORECASE)

# (path, text of the comment line) -> why that comment is not a violation.
# An entry needs a reason; none is needed today.
TEST_REASON_ALLOWLIST = {}

PRODUCTION_DIRS = ("dcfs", "bench")


def production_sources(root):
    for top in PRODUCTION_DIRS:
        for dirpath, dirnames, names in os.walk(os.path.join(root, top)):
            dirnames[:] = sorted(d for d in dirnames if d != "testonly")
            for name in sorted(names):
                if (name.endswith((".h", ".cc")) and
                        not name.endswith("_test.cc")):
                    full = os.path.join(dirpath, name)
                    yield os.path.relpath(full, root), full


def comment_text(line, in_block):
    """Returns (comment part of `line`, still inside a /* */ block)."""
    parts = []
    pos = 0
    while pos < len(line):
        if in_block:
            end = line.find("*/", pos)
            parts.append(line[pos:end] if end >= 0 else line[pos:])
            if end < 0:
                break
            in_block = False
            pos = end + 2
            continue
        slashes = line.find("//", pos)
        block = line.find("/*", pos)
        if slashes >= 0 and (block < 0 or slashes < block):
            parts.append(line[slashes:])
            break
        if block < 0:
            break
        in_block = True
        pos = block + 2
    return " ".join(parts), in_block


def no_test_only_comments(root, allowlist=None):
    if allowlist is None:
        allowlist = TEST_REASON_ALLOWLIST
    for key, reason in allowlist.items():
        if not reason.strip():
            raise ValueError("allowlist entry %r needs a reason" % (key,))
    problems = []
    for rel, full in production_sources(root):
        in_block = False
        with open(full, encoding="utf-8") as f:
            for number, line in enumerate(f, 1):
                comment, in_block = comment_text(line, in_block)
                names = re.findall(r"\w*(?:ForTest|_for_test)\w*", line)
                found = TEST_REASON.search(comment) or (
                    TEST_REASON.search(" ".join(names)))
                if not found:
                    continue
                if any(rel == path and snippet in line
                       for (path, snippet) in allowlist):
                    continue
                problems.append(
                    "%s:%d: %r gives tests as the reason for code "
                    "(docs/style.md, \"No test-only things in production "
                    "code\": make the seam a feature of the class, with a "
                    "production default, and describe it in production "
                    "terms)" % (rel, number, found.group(0)))
    return problems


FRIEND_OF_TESTONLY = re.compile(
    r"\bfriend\b[^;{]*\btestonly\s*::")


def no_testonly_friends(root):
    problems = []
    for rel, full in production_sources(root):
        with open(full, encoding="utf-8") as f:
            for number, line in enumerate(f, 1):
                code = line.split("//", 1)[0]
                if FRIEND_OF_TESTONLY.search(code):
                    problems.append(
                        "%s:%d: a production class names a testonly class as "
                        "a friend (docs/style.md, \"No test-only things in "
                        "production code\": expose what the test reads "
                        "through a real feature, such as a ProtocolEvents "
                        "hook)" % (rel, number))
    return problems


FIXED_ARRAYS_LIST = ("tools", "repo_shape_fixed_arrays.txt")
# `std::vector<T> name(expr);` (one argument: a comma outside brackets
# refuses it, so iterator pairs do not match) and `std::string name(expr,
# '\0');`. Both are a local buffer sized once (docs/style.md, 1.2).
VECTOR_SIZED_RE = re.compile(r"std::vector<.+>\s+\w+\((.*)\);")
STRING_FILLED_RE = re.compile(r"std::string\s+\w+\(.*,\s*'\\0'\);")


def has_top_level_comma(args):
    depth = 0
    for ch in args:
        if ch in "([{":
            depth += 1
        elif ch in ")]}":
            depth -= 1
        elif ch == "," and depth == 0:
            return True
    return False


def fixed_array_sites(text):
    """Line numbers of the local buffers sized once in a C++ source."""
    sites = []
    for number, line in enumerate(text.splitlines(), 1):
        code = line.split("//", 1)[0]
        # A local is indented; a declaration at namespace scope is not one.
        if not code.startswith((" ", "\t")):
            continue
        vector = VECTOR_SIZED_RE.search(code)
        if ((vector and not has_top_level_comma(vector.group(1))) or
                STRING_FILLED_RE.search(code)):
            sites.append(number)
    return sites


def fixed_array_files(root):
    """dcfs/*.h, dcfs/*.cc and tools/*.cc, not *_test.cc (testonly/ is a
    subdirectory and is not listed)."""
    for top, exts in (("dcfs", (".h", ".cc")), ("tools", (".cc",))):
        for name in sorted(os.listdir(os.path.join(root, top))):
            if (name.endswith(exts) and not name.endswith("_test.cc") and
                    os.path.isfile(os.path.join(root, top, name))):
                yield "%s/%s" % (top, name)


def known_fixed_arrays(root):
    """{path: (count, reason)} from tools/repo_shape_fixed_arrays.txt."""
    known = {}
    for line in read(root, *FIXED_ARRAYS_LIST).splitlines():
        line = line.strip()
        if not line or line.startswith("#"):
            continue
        head, _, reason = line.partition("|")
        path, count = head.split()
        known[path] = (int(count), reason.strip())
    return known


def fixed_arrays(root):
    known = known_fixed_arrays(root)
    problems = []
    actual = {}
    for path in fixed_array_files(root):
        count = len(fixed_array_sites(read(root, path)))
        if count:
            actual[path] = count
    for path, count in actual.items():
        allowed = known.get(path, (0, ""))[0]
        if count > allowed:
            problems.append(
                "%s has %d local buffer(s) sized once, %d known: make it an "
                "absl::FixedArray (docs/style.md, 1.2: a buffer whose size "
                "is known when it is made)" % (path, count, allowed))
    for path, (count, _) in known.items():
        if actual.get(path, 0) != count:
            problems.append(
                "tools/repo_shape_fixed_arrays.txt lists %d buffer(s) for "
                "%s, which has %d: set the count to the file's (the list "
                "only shrinks)" % (count, path, actual.get(path, 0)))
    return problems


PROJECT_PREFIX_RE = re.compile(r"\bDcfs[A-Z]")
# Strings (with escapes), block comments and line comments, replaced by
# spaces (newlines kept, so line numbers hold). One pass, so a // inside a
# string is not a comment.
COMMENT_OR_STRING_RE = re.compile(
    r'"(?:\\.|[^"\\\n])*"|\'(?:\\.|[^\'\\\n])*\'|/\*.*?\*/|//[^\n]*', re.DOTALL)


def code_only(text):
    return COMMENT_OR_STRING_RE.sub(
        lambda m: re.sub(r"[^\n]", " ", m.group(0)), text)


def project_prefix_sites(text):
    """Line numbers of identifiers that begin with `Dcfs` + capital, in code
    (not comments, not strings)."""
    return [number for number, line in enumerate(code_only(text).splitlines(), 1)
            if PROJECT_PREFIX_RE.search(line)]


def project_prefix(root):
    problems = []
    for dirpath, _, names in os.walk(os.path.join(root, "dcfs")):
        for name in sorted(names):
            if not name.endswith((".h", ".cc")):
                continue
            rel = os.path.relpath(os.path.join(dirpath, name), root)
            for number in project_prefix_sites(read(root, rel)):
                problems.append(
                    "%s:%d: an identifier begins with `Dcfs` (docs/style.md, "
                    "1.3: a name says what distinguishes the thing and never "
                    "repeats the namespace; a project-name word is a contrast "
                    "with something that is not dcfs, and is said in a "
                    "comment)" % (rel, number))
    return problems


ABSL_MACRO_RE = re.compile(r"\bABSL_(RETURN_IF_ERROR|ASSIGN_OR_RETURN)\(")
ABSL_MACROS_LIST = ("tools", "repo_shape_absl_macros.txt")


def absl_macro_sites(text):
    """Line numbers of the long Status macros in code (not comments)."""
    return [number for number, line in enumerate(code_only(text).splitlines(), 1)
            if ABSL_MACRO_RE.search(line)]


def absl_macro_files(root):
    """Every .h and .cc under dcfs/, bench/, tools/ and test/qemu/guest/
    (fixtures included)."""
    for top in ("dcfs", "bench", "tools",
                os.path.join("test", "qemu", "guest")):
        for dirpath, _, names in os.walk(os.path.join(root, top)):
            for name in sorted(names):
                if name.endswith((".h", ".cc")):
                    yield os.path.relpath(os.path.join(dirpath, name), root)


def known_absl_macros(root):
    """{path: (count, reason)} from tools/repo_shape_absl_macros.txt."""
    known = {}
    for line in read(root, *ABSL_MACROS_LIST).splitlines():
        line = line.strip()
        if not line or line.startswith("#"):
            continue
        head, _, reason = line.partition("|")
        path, count = head.split()
        known[path] = (int(count), reason.strip())
    return known


def absl_prefixed_status_macros(root):
    """The long ABSL_ Status macros are refused (docs/style.md, 1.6): the
    short names come from //dcfs:status. A file's count must equal the list's
    count, and the list only shrinks."""
    known = known_absl_macros(root)
    problems = []
    for path in absl_macro_files(root):
        count = len(absl_macro_sites(read(root, path)))
        allowed = known.get(path, (0, ""))[0]
        if count != allowed:
            problems.append(
                "%s has %d ABSL_RETURN_IF_ERROR / ABSL_ASSIGN_OR_RETURN use(s), "
                "%d listed in tools/repo_shape_absl_macros.txt: use the short "
                "RETURN_IF_ERROR / ASSIGN_OR_RETURN (docs/style.md, 1.6); a "
                "listed count must equal the file's" % (path, count, allowed))
    return problems


NODISCARD_STATUS_RE = re.compile(
    r"(\[\[nodiscard\]\]|ABSL_MUST_USE_RESULT)\s+absl::Status(Or<|\s)")


def nodiscard_on_status(root):
    """`[[nodiscard]]` before absl::Status or StatusOr is refused: both types
    are must-use already (docs/style.md, 1.2; 25.17). A function returning an
    owned descriptor keeps its attribute: that type is not must-use."""
    problems = []
    for path in absl_macro_files(root):
        lines = code_only(read(root, path)).splitlines()
        for number, line in enumerate(lines, 1):
            if NODISCARD_STATUS_RE.search(line):
                problems.append(
                    "%s:%d: [[nodiscard]] on a function returning absl::Status "
                    "or StatusOr is redundant (docs/style.md, 1.2: the type "
                    "is already must-use); remove it" % (path, number))
    return problems


SPDX_EXTENSIONS = (".cc", ".h", ".c", ".py", ".sh", ".bzl", ".bazel", ".tla",
                   ".java", ".tpl", ".sql", ".yml", ".yaml")
SPDX_NAMES = frozenset({"BUILD", "BUILD.bazel", "MODULE.bazel", ".bazelrc",
                        ".clang-format", ".clang-tidy"})
SPDX_PATHS = frozenset({"test/qemu/guest/init"})
# Sample and known-bad inputs of the checks, not code of ours.
SPDX_FIXTURES = re.compile(
    r"(^|/)[^/]*(testdata|fixtures)[^/]*/|^tools/style_matchers/"
    r"|^tools/mutation/fixture")
SPDX_ID_RE = re.compile(r"SPDX-License-Identifier:\s*(\S+)")
SPDX_ALLOWED_LIST = ("tools", "licenses_allowed.txt")
THIRD_PARTY_LIST = ("tools", "third_party_files.txt")
COPYLEFT = ("GPL-", "LGPL-")


def spdx_wanted(path):
    """The files that carry an SPDX header (docs/licenses.md, step 7.8)."""
    if SPDX_FIXTURES.search(path):
        return False
    return (path.endswith(SPDX_EXTENSIONS) or
            os.path.basename(path) in SPDX_NAMES or path in SPDX_PATHS)


def spdx_header(text):
    """The licence id on line 1 (line 2 after a shebang), or None."""
    lines = text.splitlines()
    if lines and lines[0].startswith("#!"):
        lines = lines[1:]
    match = SPDX_ID_RE.search(lines[0]) if lines else None
    return match.group(1) if match else None


def table_rows(root, parts):
    """The rows of a `a | b | ...` table; `#` lines and blanks are skipped."""
    rows = []
    for line in read(root, *parts).splitlines():
        line = line.strip()
        if line and not line.startswith("#"):
            rows.append([field.strip() for field in line.split("|")])
    return rows


def spdx_licenses(root, tracked=None):
    """Every file we wrote has an SPDX header whose licence is allowed, and
    a licence other than MIT is listed in tools/third_party_files.txt with
    the same licence; a GPL or LGPL one only with role test-only. `tracked`
    is git's list (`git ls-files`) unless given."""
    if tracked is None:
        tracked = subprocess.run(
            ["git", "ls-files"], cwd=root, capture_output=True, text=True,
            check=True).stdout.splitlines()
    allowed = {row[0] for row in table_rows(root, SPDX_ALLOWED_LIST)}
    listed = {row[0]: (row[1], row[2])
              for row in table_rows(root, THIRD_PARTY_LIST)}
    problems = []
    for path in sorted(p for p in tracked if spdx_wanted(p)):
        licence = spdx_header(read(root, path))
        if licence is None:
            problems.append(
                "%s has no SPDX-License-Identifier on line 1 (line 2 after a "
                "shebang): docs/licenses.md, step 7.8" % path)
            continue
        if licence not in allowed:
            problems.append(
                "%s is under %s, which tools/licenses_allowed.txt does not "
                "allow" % (path, licence))
            continue
        if licence == "MIT":
            continue
        if path not in listed:
            problems.append(
                "%s is %s but is not listed in tools/third_party_files.txt" %
                (path, licence))
            continue
        listed_licence, role = listed[path]
        if listed_licence != licence:
            problems.append(
                "%s is %s, listed as %s in tools/third_party_files.txt" %
                (path, licence, listed_licence))
        elif licence.startswith(COPYLEFT) and role != "test-only":
            problems.append(
                "%s is %s and must have role test-only in "
                "tools/third_party_files.txt" % (path, licence))
    return problems


RULE_FILES = ("docs/style.md", "docs/design.md", "docs/plan/process.md",
              "CLAUDE.md")
RULE_MARKER = re.compile(r'<a id="rule-([a-z0-9-]+)"></a>')
RULE_INDEX_LINK = re.compile(r"\]\(/?(docs/rules/index-[a-z0-9-]+\.md)\)")
RULE_LINK = re.compile(r"\]\(([^)#\s]+)#rule-([a-z0-9-]+)\)")


def rule_index(root):
    marked = {}
    problems = []
    for path in RULE_FILES:
        for number, line in enumerate(read(root, path).splitlines(), 1):
            for name in RULE_MARKER.findall(line):
                if (path, name) in marked:
                    problems.append(
                        "%s marks rule %s twice (lines %d and %d): a rule's "
                        "marker is unique in its file" %
                        (path, name, marked[(path, name)], number))
                marked[(path, name)] = number
    indexes = rule_indexes(root)
    in_indexes = {}
    for index in indexes:
        for target, name in RULE_LINK.findall(read(root, index)):
            if not target.startswith("/"):
                problems.append(
                    "%s links to %s#rule-%s with a relative path: use the "
                    "repo-absolute /path" % (index, target, name))
                continue
            path = target[1:]
            if (path, name) not in marked:
                problems.append(
                    "%s links to %s#rule-%s, which no file marks: fix or "
                    "remove the entry" % (index, path, name))
                continue
            in_indexes.setdefault((path, name), set()).add(index)
    for (path, name), number in sorted(marked.items()):
        found = in_indexes.get((path, name), set())
        if not found:
            problems.append(
                "%s marks rule %s (line %d) but no docs/rules/index-*.md "
                "links to it: add a line to the index it belongs to" %
                (path, name, number))
        elif len(found) > 1:
            problems.append(
                "rule %s of %s is in %d indexes (%s): a rule is in exactly "
                "one" % (name, path, len(found), ", ".join(sorted(found))))
    linked_indexes = set(RULE_INDEX_LINK.findall(read(root, "AGENTS.md")))
    for index in indexes:
        if index not in linked_indexes:
            problems.append(
                "%s is not linked from AGENTS.md: add it to the table of "
                "what to read" % index)
    return problems


def rule_indexes(root):
    directory = os.path.join(root, "docs", "rules")
    if not os.path.isdir(directory):
        return []
    return sorted("docs/rules/" + name for name in os.listdir(directory)
                  if name.startswith("index-") and name.endswith(".md"))


ATTRIBUTIONS_LIST = ("tools", "repo_shape_attributions.txt")
ATTRIBUTION_RE = re.compile(r"russ\s*[,(]\s*20\d\d-")


def agent_file_attributions(root):
    """A rule is homed once; agent files and CLAUDE.md point to it.

    A "russ, 20NN-" attribution in .claude/agents/*.md, .claude/skills/**/*.md
    or CLAUDE.md is a
    rule restated (or homed) outside style.md, design.md and process.md. The
    known ones are counted per file in tools/repo_shape_attributions.txt,
    each with the reason it is role-only; the list only shrinks.
    """
    known = {}
    problems = []
    for line in read(root, *ATTRIBUTIONS_LIST).splitlines():
        line = line.strip()
        if not line or line.startswith("#"):
            continue
        head, _, reason = line.partition("|")
        fields = head.split()
        if len(fields) != 2 or not fields[1].isdigit():
            problems.append(
                "tools/repo_shape_attributions.txt: cannot read %r: write "
                "'path count | reason'" % line)
            continue
        known[fields[0]] = int(fields[1])
    paths = ["CLAUDE.md"]
    agents = os.path.join(root, ".claude", "agents")
    if os.path.isdir(agents):
        paths += [".claude/agents/" + name
                  for name in sorted(os.listdir(agents))
                  if name.endswith(".md")]
    skills = os.path.join(root, ".claude", "skills")
    for directory, _, names in sorted(os.walk(skills)):
        paths += [
            os.path.relpath(os.path.join(directory, name), root)
            for name in sorted(names) if name.endswith(".md")
        ]
    for path in sorted(set(known) - set(paths)):
        problems.append(
            "tools/repo_shape_attributions.txt lists %s, which is gone: "
            "take it off (the list only shrinks)" % path)
    for path in paths:
        count = len(ATTRIBUTION_RE.findall(read(root, path)))
        if count > known.get(path, 0):
            problems.append(
                "%s has %d 'russ, 20NN-' attribution(s), %d known: a rule "
                "belongs in docs/style.md, docs/design.md or "
                "docs/plan/process.md with a marker; point to it here" %
                (path, count, known.get(path, 0)))
        elif count < known.get(path, 0):
            problems.append(
                "tools/repo_shape_attributions.txt lists %d for %s, which "
                "has %d: set the count to the file's (the list only "
                "shrinks)" % (known[path], path, count))
    return problems


LONG_LINES_LIST = ("tools", "repo_shape_long_lines.txt")
SPLIT_SPANS_LIST = ("tools", "repo_shape_split_spans.txt")
MAX_COLUMNS = 80
# An unbreakable token (a URL, a path in backticks) this long is the only
# reason for a line to be long when the rest of the line fits.
UNBREAKABLE_TOKEN = 20
FENCE_RE = re.compile(r"^\s*(```|~~~)")
LINK_DEFINITION_RE = re.compile(r"^\s*\[[^\]]+\]:\s")


def wrapped_text(path):
    """The files whose lines are held to 80 columns (C and C++ are
    clang-format's; third_party/ is not ours)."""
    if path.startswith("third_party/"):
        return False
    return (path.endswith((".md", ".py", ".sh", ".bzl")) or
            os.path.basename(path) in ("BUILD.bazel", ".bazelrc"))


def tracked_files(root, tracked):
    if tracked is not None:
        return tracked
    return subprocess.run(
        ["git", "ls-files"], cwd=root, capture_output=True, text=True,
        check=True).stdout.splitlines()


def unfenced_lines(text, markdown):
    """(number, line, in_prose) of each line; in_prose is False inside a
    Markdown fence (always True outside Markdown)."""
    fenced = False
    for number, line in enumerate(text.splitlines(), 1):
        if markdown and FENCE_RE.match(line):
            fenced = not fenced
            yield number, line, False
        else:
            yield number, line, not fenced


def long_line(line, markdown):
    """Over 80 columns and not excused by one unbreakable token."""
    if len(line) <= MAX_COLUMNS:
        return False
    if markdown and (line.startswith("|") or LINK_DEFINITION_RE.match(line)):
        return False
    longest = max(line.split(), key=len, default="")
    return not (len(longest) >= UNBREAKABLE_TOKEN and
                len(line) - len(longest) <= MAX_COLUMNS)


def count_long_lines(text, markdown):
    return sum(
        1 for _, line, prose in unfenced_lines(text, markdown)
        if (prose or not markdown) and long_line(line, markdown))


def known_counts(root, parts):
    """{path: count} from an allowlist of `path count` lines."""
    known = {}
    for line in read(root, *parts).splitlines():
        line = line.strip()
        if line and not line.startswith("#"):
            path, count = line.split()[:2]
            known[path] = int(count)
    return known


def compare_counts(actual, known, list_name, what):
    problems = []
    for path in sorted(actual):
        allowed = known.get(path, 0)
        if actual[path] > allowed:
            problems.append("%s has %d %s, %d known: fix them (the list %s "
                            "does not grow)" %
                            (path, actual[path], what, allowed, list_name))
    for path in sorted(known):
        if actual.get(path, 0) < known[path]:
            problems.append(
                "%s lists %d for %s, which has %d: lower it or take the "
                "entry off (the list only shrinks)" %
                (list_name, known[path], path, actual.get(path, 0)))
    return problems


def long_lines(root, tracked=None):
    """No line over 80 columns in prose, Python, shell and Bazel files,
    except the known ones counted per file in tools/repo_shape_long_lines.txt
    (a list that only shrinks). Exempt: fenced code and tables in Markdown,
    link definitions, and a line made long only by one unbreakable token."""
    actual = {}
    for path in sorted(p for p in tracked_files(root, tracked)
                       if wrapped_text(p)):
        count = count_long_lines(read(root, path), path.endswith(".md"))
        if count:
            actual[path] = count
    return compare_counts(actual, known_counts(root, LONG_LINES_LIST),
                          "tools/repo_shape_long_lines.txt",
                          "line(s) over 80 columns")


def split_span_lines(text):
    """The first line numbers of the backtick code spans that run over a
    line break within a paragraph (such a span renders with a space)."""
    found = []
    paragraph = []
    for number, line, prose in list(unfenced_lines(text, True)) + [
            (0, "", True)]:
        if prose and line.strip() and not line.startswith("|"):
            paragraph.append((number, line))
            continue
        joined = "\n".join(line for _, line in paragraph)
        for match in re.finditer(r"(`+)(?!`)(.+?)(?<!`)\1(?!`)", joined,
                                 re.DOTALL):
            if "\n" in match.group(2):
                offset = joined.count("\n", 0, match.start())
                found.append(paragraph[offset][0])
        paragraph = []
    return found


def split_code_spans(root, tracked=None):
    """No Markdown backtick code span is split across a line break (it
    renders with a space), except the known ones in
    tools/repo_shape_split_spans.txt."""
    actual = {}
    for path in sorted(p for p in tracked_files(root, tracked)
                       if p.endswith(".md") and wrapped_text(p)):
        count = len(split_span_lines(read(root, path)))
        if count:
            actual[path] = count
    return compare_counts(actual, known_counts(root, SPLIT_SPANS_LIST),
                          "tools/repo_shape_split_spans.txt",
                          "split code span(s)")


# The rules that read git's list of tracked files.
TRACKED_CHECKS = (spdx_licenses, long_lines, split_code_spans)

# Every rule, once: `all_problems` runs them, `//tools:repo_shape_test`
# asserts each is clean and the command line (`python3 -I
# tools/repo_shape.py`) prints their problems, so the three cannot differ
# (step 25.51).
CHECKS = (
    third_party_readmes, guest_scripts_used, guest_sleeps,
    no_new_guest_shell, fixed_arrays, absl_prefixed_status_macros,
    nodiscard_on_status, project_prefix, disabled_checks_listed,
    no_test_only_comments, no_testonly_friends, rule_index,
    agent_file_attributions) + TRACKED_CHECKS


def all_problems(root):
    return [problem for check in CHECKS for problem in check(root)]


def main(argv):
    """Print every problem of the tree at argv[1] (default: the one this
    file is in); exit status 1 if there are any."""
    root = (argv[1] if len(argv) > 1 else os.path.dirname(
        os.path.dirname(os.path.realpath(__file__))))
    problems = all_problems(root)
    for problem in problems:
        print(problem)
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
