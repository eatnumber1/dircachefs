"""Formats the tracked C and C++ files with the pinned clang-format.

`bazel run //tools:format` rewrites the files in place (clang-format -i);
`bazel run //tools:format -- --check` changes nothing and exits 1 when a
file would change (clang-format --dry-run -Werror). The file list is the
tracked C and C++ files of the workspace (`git ls-files`) minus third-party
code and the known-bad fixtures, so the run and //tools:format_test see the
same files.
"""

import argparse
import os
import re
import subprocess
import sys

# A known-bad fixture for //tools:format_test's self-check: not formatted on
# purpose, so it is never part of the list.
FIXTURE = "tools/format_bad_fixture.cc"

# clang-format's diagnostic for a file that would change: "path:line:col: ...".
DIAGNOSTIC = re.compile(r"^(.+?):\d+:\d+: (?:warning|error): ", re.MULTILINE)


def wanted(path):
    """Whether the formatter owns the tracked file at `path`."""
    if not path.endswith((".c", ".cc", ".h")):
        return False
    if path.startswith("third_party/") or path == FIXTURE:
        return False
    if path.startswith("tools/mutation/fixture"):
        return False
    # The known-bad clang-tidy and clang-query fixtures.
    if path.startswith("tools/style_matchers/") and path.endswith("_bad.cc"):
        return False
    return True


def tracked_files(root):
    """The formatter's files: `git ls-files` under `root`, filtered."""
    out = subprocess.run(
        ["git", "ls-files", "--", "*.c", "*.cc", "*.h"],
        cwd=root, capture_output=True, text=True, check=True).stdout
    return [path for path in out.splitlines() if wanted(path)]


def check(clang_format, style, root, files):
    """(exit status, files that would change, diagnostics) of --dry-run."""
    proc = subprocess.run(
        [clang_format, "--style=file:" + style, "--dry-run", "-Werror"]
        + files,
        cwd=root, capture_output=True, text=True, check=False)
    bad = sorted(set(DIAGNOSTIC.findall(proc.stderr)))
    return proc.returncode, bad, proc.stderr


def fix(clang_format, style, root, files):
    """Rewrites `files` in place; returns clang-format's exit status."""
    return subprocess.run(
        [clang_format, "--style=file:" + style, "-i"] + files,
        cwd=root, check=False).returncode


def main(argv):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--check", action="store_true")
    parser.add_argument("--root", default=os.environ.get(
        "BUILD_WORKSPACE_DIRECTORY", ""))
    parser.add_argument("--clang-format", required=True)
    parser.add_argument("--style", required=True)
    args = parser.parse_args(argv)
    clang_format = os.path.abspath(args.clang_format)
    style = os.path.abspath(args.style)
    root = os.path.abspath(args.root)
    files = tracked_files(root)
    if not args.check:
        return fix(clang_format, style, root, files)
    rc, bad, diagnostics = check(clang_format, style, root, files)
    sys.stderr.write(diagnostics)
    print(f"{len(bad)} of {len(files)} files need formatting")
    return rc


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
