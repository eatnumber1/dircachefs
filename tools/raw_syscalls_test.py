"""Fails on a raw libc syscall call outside dcfs/syscalls.cc (docs/style.md).

Every syscall goes through dcfs/syscalls.h, which turns the failure into a
Status; this test scans the tree's C++ for a call of any name in
raw_syscalls_names.txt that is not qualified with `syscalls::`.

What it catches: `name(`, `::name(`, `std::name(`, `(name)(` and `x>name(`
in code (comments and string/char literals are blanked first; a `'` inside a
number is a digit separator, not a char literal), and any use of
`std::filesystem`, `std::ifstream`, `std::ofstream` and `std::fstream`.
What it ignores: `syscalls::name(`, any other `Scope::name(`, `.name(` and
`->name(` (a member or another namespace's function), and
`__wrap_name`/`__real_name` (the link-time fault fakes of the *_test.cc
files define and call those: the negative lookbehind on a word character
keeps the pattern from matching inside a longer identifier). `remove` is not
a name (it would flag the `std::remove` algorithm); the file calls are
`unlink` and `unlinkat`. What it cannot catch: a call through a macro or
function pointer, a name not in the list, a stream or filesystem type named
without `std::` (a using-declaration), a raw string literal containing a
quote, and a call whose name is a local function or lambda of the same
spelling (a false positive: rename it).

Scanned: the C++ of dcfs/, dcfs/testonly/ and bench/. Excluded:
dcfs/syscalls.cc, dcfs/syscalls_backing.cc and dcfs/syscalls_process.cc (the
wrappers) and their headers.
Not scanned: tools/fhtest.c and tools/testutil.c (C programs run in the
guest) and tools/banned_symbols_fixture.cc (it has banned calls on purpose).
"""

import re
import sys
import unittest

EXCLUDED = (
    "dcfs/syscalls.cc",
    "dcfs/syscalls.h",
    "dcfs/syscalls_backing.cc",
    "dcfs/syscalls_backing.h",
    "dcfs/syscalls_process.cc",
    "dcfs/syscalls_process.h",
)


def load_names(path):
    with open(path) as f:
        return [l.strip() for l in f if l.strip() and not l.startswith("#")]


def pattern(names):
    alt = "|".join(map(re.escape, names))
    # group 1 the qualifier, group 2 the name; `(name)(` has the same groups.
    return re.compile(
        r"(?<![\w.])(?<!->)((?:\w+::)*)(%s)\s*\(|"
        r"(?<![\w.])(?<!->)\(\s*((?:\w+::)*)(%s)\s*\)\s*\(" % (alt, alt)
    )


QUALIFIERS = ("", "std::")

# std::filesystem and the file streams open and walk files behind
# syscalls.h's back.
LIBRARY_IO = re.compile(r"\bstd::(filesystem|ifstream|ofstream|fstream)\b")


def is_digit_separator(src, i):
    """True when the `'` at src[i] sits inside a numeric literal (1'000)."""
    j = i
    while j > 0 and (src[j - 1].isalnum() or src[j - 1] in "_'"):
        j -= 1
    return j < i and src[j].isdigit() and i + 1 < len(src) and src[i + 1].isalnum()


def blank(src):
    """Removes comments and the contents of string and char literals."""
    out = []
    i, n = 0, len(src)
    while i < n:
        c = src[i]
        if src.startswith("//", i):
            j = src.find("\n", i)
            i = n if j < 0 else j
        elif src.startswith("/*", i):
            j = src.find("*/", i + 2)
            j = n if j < 0 else j + 2
            out.append(re.sub(r"[^\n]", "", src[i:j]))
            i = j
        elif c == "'" and is_digit_separator(src, i):
            out.append(c)
            i += 1
        elif c in "\"'":
            j = i + 1
            while j < n and src[j] != c:
                j += 2 if src[j] == "\\" else 1
            out.append(c + c)
            i = j + 1
        else:
            out.append(c)
            i += 1
    return "".join(out)


def find_raw_calls(src, pat):
    """Returns [(line, name)] of the raw calls in `src`."""
    s = blank(src)
    found = []
    for m in pat.finditer(s):
        qualifier = m.group(1) if m.group(2) else m.group(3)
        name = m.group(2) or m.group(4)
        if qualifier not in QUALIFIERS:
            continue
        found.append((s.count("\n", 0, m.start()) + 1, name))
    for m in LIBRARY_IO.finditer(s):
        found.append((s.count("\n", 0, m.start()) + 1, "std::" + m.group(1)))
    return sorted(found)


class FinderTest(unittest.TestCase):
    pat = pattern(["open", "mkdir", "close"])

    def calls(self, src):
        return [n for _, n in find_raw_calls(src, self.pat)]

    def test_flags_raw_calls(self):
        self.assertEqual(self.calls("int fd = open(p, 0);"), ["open"])
        self.assertEqual(self.calls("::mkdir(p, 0700);"), ["mkdir"])
        self.assertEqual(self.calls("if (close (fd) != 0) {}"), ["close"])

    def test_allows_wrappers_members_and_fakes(self):
        for src in (
            "syscalls::open(a, b);",
            "x.open(a);",
            "x->close();",
            "Foo::open(a);",
            "__wrap_open(a); __real_close(fd);",
            "int reopen(int fd);",
        ):
            self.assertEqual(self.calls(src), [], src)

    def test_ignores_comments_and_strings(self):
        for src in (
            "// open(x)\n",
            "/* mkdir(x) */",
            'LOG(INFO) << "open(x)";',
            "char c = '(';",
        ):
            self.assertEqual(self.calls(src), [], src)

    def test_flags_std_parenthesized_and_after_angle(self):
        self.assertEqual(self.calls("std::close(fd);"), ["close"])
        self.assertEqual(self.calls("::std::close(fd);"), ["close"])
        self.assertEqual(self.calls("int rc = (open)(p, 0);"), ["open"])
        self.assertEqual(self.calls("if (a>close(fd)) {}"), ["close"])
        self.assertEqual(self.calls("p = x->close(1);"), [])
        self.assertEqual(self.calls("(syscalls::open)(p);"), [])

    def test_digit_separators_are_not_char_literals(self):
        self.assertEqual(self.calls("int n = 1'000'000; close(fd);"),
                         ["close"])
        self.assertEqual(self.calls("auto m = 0x1'F; open(p);"), ["open"])
        # A real char literal still hides what follows on its line.
        self.assertEqual(self.calls("char c = 'a'; // open(x)"), [])
        self.assertEqual(self.calls("f('\\''); close(1);"), ["close"])

    def test_flags_std_filesystem_and_file_streams(self):
        for src, name in (
            ("auto n = std::filesystem::file_size(p);", "std::filesystem"),
            ("namespace fs = std::filesystem;", "std::filesystem"),
            ("std::ifstream in(path);", "std::ifstream"),
            ("std::ofstream out(path);", "std::ofstream"),
            ("std::fstream f(path);", "std::fstream"),
        ):
            self.assertEqual(
                [n for _, n in find_raw_calls(src, self.pat)], [name], src)
        self.assertEqual(self.calls("std::string fstream_name;"), [])
        self.assertEqual(self.calls("// std::ifstream in(p);"), [])

    def test_reports_the_line(self):
        self.assertEqual(find_raw_calls("a;\n\nclose(1);", self.pat),
                         [(3, "close")])


def main(argv):
    names_file, files = argv[0], argv[1:]
    pat = pattern(load_names(names_file))
    lines = []
    for path in sorted(files):
        if path.endswith(EXCLUDED):
            continue
        with open(path) as f:
            hits = find_raw_calls(f.read(), pat)
        lines += ["%s:%d: raw %s: use the syscalls:: wrappers" % (path, l, n)
                  for l, n in hits]
    if lines:
        print("\n".join(lines))
        print("%d raw syscall calls: use the dcfs::syscalls wrappers "
              "(docs/style.md 1.5)" % len(lines))
        return 1
    return 0


if __name__ == "__main__":
    if len(sys.argv) > 1 and sys.argv[1] == "--scan":
        sys.exit(main(sys.argv[2:]))
    unittest.main()
