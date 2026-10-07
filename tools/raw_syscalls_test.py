"""Fails on a raw libc syscall call outside dcfs/syscalls.cc (docs/style.md).

Every syscall goes through dcfs/syscalls.h, which turns the failure into a
Status; this test scans the tree's C++ for a call of any name in
raw_syscalls_names.txt that is not qualified with `syscalls::`.

What it catches: `name(`, `::name(` and `std::name(`-less global calls in
code (comments and string/char literals are blanked first). What it
ignores: `syscalls::name(`, any `Scope::name(` or `.name(` or `->name(` (a
member or another namespace's function), and `__wrap_name`/`__real_name`
(the link-time fault fakes of the *_test.cc files define and call those:
`\\b` does not match inside a longer identifier). What it cannot catch: a
call through a macro or function pointer, a name not in the list, a raw
string literal containing a quote, and a call whose name is a local
function or lambda of the same spelling (a false positive: rename it).

Excluded files: dcfs/syscalls.cc (the wrappers) and dcfs/syscalls.h (their
declarations). tools/*.c are C programs and are not scanned.
"""

import collections
import re
import sys
import unittest

EXCLUDED = ("dcfs/syscalls.cc", "dcfs/syscalls.h")


def load_names(path):
    with open(path) as f:
        return [l.strip() for l in f if l.strip() and not l.startswith("#")]


def pattern(names):
    return re.compile(
        r"(?<![\w.>])((?:\w+::)*)(%s)\s*\(" % "|".join(map(re.escape, names))
    )


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
        if m.group(1) not in ("", "::"):
            continue
        found.append((s.count("\n", 0, m.start()) + 1, m.group(2)))
    return found


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

    def test_reports_the_line(self):
        self.assertEqual(find_raw_calls("a;\n\nclose(1);", self.pat),
                         [(3, "close")])


def main(argv):
    names_file, files = argv[0], argv[1:]
    pat = pattern(load_names(names_file))
    counts = collections.OrderedDict()
    lines = []
    for path in sorted(files):
        if path.endswith(EXCLUDED):
            continue
        with open(path) as f:
            hits = find_raw_calls(f.read(), pat)
        if hits:
            counts[path] = len(hits)
            lines += ["%s:%d: raw %s(): use syscalls::%s" % (path, l, n, n)
                      for l, n in hits]
    if counts:
        print("\n".join(lines))
        print("\nraw syscall calls per file:")
        for path, c in counts.items():
            print("  %4d  %s" % (c, path))
        print("  %4d  total" % sum(counts.values()))
        return 1
    return 0


if __name__ == "__main__":
    if len(sys.argv) > 1 and sys.argv[1] == "--scan":
        sys.exit(main(sys.argv[2:]))
    unittest.main()
