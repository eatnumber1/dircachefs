# SPDX-License-Identifier: MIT
"""Every Tip of the Week number we cite is a published tip (plan step 25.32).

A number cited from memory that does not exist fails. Checked: docs/style.md,
docs/abseil-utilities.md, docs/design.md and the C++ (`.cc`, `.h`, `.c`) of
the tracked files. Forms: `TotW #103`, `TotW 103`, `totw/103`,
`https://abseil.io/tips/103` and `Tip of the Week #103`. The published
numbers are the index of //third_party/abseil_tips.
"""
import re
import subprocess

DOCS = ("docs/style.md", "docs/abseil-utilities.md", "docs/design.md")
CPP_EXTENSIONS = (".cc", ".h", ".c")
CITATION_RE = re.compile(
    r"(?:\btotw\s*[#/]?\s*|\btips?\s+of\s+the\s+week\s*#?\s*|"
    r"\babseil\.io/tips/)(\d+)\b(?![.-]\d)",
    re.I)


def published(index_path):
    """The set of published tip numbers in an index.tsv."""
    with open(index_path, encoding="utf-8") as f:
        return {int(line.split("\t")[0]) for line in f}


def checked_files(root):
    tracked = subprocess.run(["git", "ls-files"], cwd=root, check=True,
                             capture_output=True, text=True).stdout.split("\n")
    return [p for p in tracked if p in DOCS or p.endswith(CPP_EXTENSIONS)]


def findings(root, numbers):
    """`path:line: TotW N is not a published tip` for each bad citation."""
    found = []
    for path in checked_files(root):
        with open("%s/%s" % (root, path), encoding="utf-8",
                  errors="replace") as f:
            text = f.read()
        for match in CITATION_RE.finditer(text):
            if int(match.group(1)) not in numbers:
                line_number = text.count("\n", 0, match.start()) + 1
                found.append("%s:%d: TotW %s is not a published tip"
                             % (path, line_number, match.group(1)))
    return found
