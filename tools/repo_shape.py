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
- no_test_only_comments: no production source (dcfs/ and bench/, not
  *_test.cc and not testonly/) has a comment that gives tests as the reason
  for code, or a ForTest / _for_test name (docs/style.md, "No test-only
  things in production code", step 25.7).

(The fourth rule, commit subjects, needs git: .github/ci/commit_subjects.sh.)
"""

import os
import re

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


QUALIFIER_RE = re.compile(r"\bdcfs::")
# A namespace is declared by its full name (`namespace dcfs::backing {`):
# that spelling is not a use of a name inside dcfs.
NAMESPACE_DECL_RE = re.compile(r"^\s*namespace\s+dcfs::")
NAMESPACE_OPEN_RE = re.compile(r"^namespace dcfs(::\w+)*\s*\{")
NAMESPACE_CLOSE_RE = re.compile(r"^\}\s*//\s*namespace dcfs(::\w+)*\s*$")


def namespace_qualifier_sites(text):
    """Line numbers of `dcfs::` in code inside namespace dcfs, outside
    #define (and its backslash-continued lines), not counting a namespace
    declaration. Outside it (before the namespace opens, or after it
    closes: main() and the global-scope tests) the qualifier is needed."""
    sites = []
    in_define = False
    depth = 0
    for number, line in enumerate(text.splitlines(), 1):
        if NAMESPACE_OPEN_RE.match(line):
            depth += 1
        define = in_define or line.lstrip().startswith("#define")
        in_define = define and line.rstrip().endswith("\\")
        code = line.split("//", 1)[0]
        if (depth > 0 and not define and not NAMESPACE_DECL_RE.match(code) and
                QUALIFIER_RE.search(code)):
            sites.append(number)
        if NAMESPACE_CLOSE_RE.match(line):
            depth = max(depth - 1, 0)
    return sites


def namespace_qualifiers(root):
    problems = []
    for dirpath, _, names in os.walk(os.path.join(root, "dcfs")):
        for name in sorted(names):
            if not name.endswith((".h", ".cc")):
                continue
            rel = os.path.relpath(os.path.join(dirpath, name), root)
            for number in namespace_qualifier_sites(read(root, rel)):
                problems.append(
                    "%s:%d: `dcfs::` qualifies a name inside namespace dcfs "
                    "(docs/style.md, 1.3: call it unqualified; a macro body "
                    "or a real ambiguity, named in a comment, is the "
                    "exception)" % (rel, number))
    return problems


def all_problems(root):
    return (third_party_readmes(root) + guest_scripts_used(root) +
            guest_sleeps(root) + disabled_checks_listed(root) +
            no_test_only_comments(root) + no_testonly_friends(root) +
            fixed_arrays(root) + namespace_qualifiers(root))
