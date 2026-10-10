"""Applies one clang-tidy check to the given files, under the build's flags.

`bazel run //tools:style_fix -- --check=misc-include-cleaner [paths]`

The flags of each file are the ones the style aspect (tools/style_checks.bzl)
computes: its `_style/<target>/<file>.flags.txt`, one flag per line, relative
to the execution root. The fix runs clang-tidy with `--checks=-*,<check>
--fix --fix-errors` from that execution root (so the relative flags resolve),
on the workspace file, then clang-format over the lines the diff changed.
Both tools are the pinned LLVM's, found under the execution root too.

The execution root is the one Bazel uses for this workspace: the target of
`bazel-out` in the workspace, one level up (`.../execroot/_main`). `--execroot`
overrides it.
"""

import argparse
import glob
import os
import re
import subprocess
import sys

LLVM = "external/+llvm_distribution+dcfs_llvm/bin"
HUNK = re.compile(r"^@@ -\d+(?:,\d+)? \+(\d+)(?:,(\d+))? @@")


def changed_lines(diff_text):
    """The `--lines=A:B` ranges of the new side of a `git diff -U0` text."""
    ranges = []
    for line in diff_text.splitlines():
        match = HUNK.match(line)
        if not match:
            continue
        start = int(match.group(1))
        count = int(match.group(2)) if match.group(2) is not None else 1
        if count > 0:
            ranges.append("--lines=%d:%d" % (start, start + count - 1))
    return ranges


def read_flags(text):
    return [line for line in text.splitlines() if line]


def flags_file(execroot, relpath):
    """The aspect's flags file for a workspace-relative source path."""
    pattern = os.path.join(
        execroot, "bazel-out", "*", "bin", "**", "_style", "*", relpath + ".flags.txt")
    # The runfiles trees under bin/ hold copies of the same files: not these.
    found = sorted(f for f in glob.glob(pattern, recursive=True)
                   if ".runfiles" not in f)
    # A source compiled by several targets (the library and its test) has one
    # file per target: the library's, named as the source, is the one the
    # fix runs under; any other choice is refused.
    stem = os.path.splitext(os.path.basename(relpath))[0]
    # The target is the directory under _style/ that the source's path hangs
    # from: strip the source's own relative path and the separator before it.
    named = [f for f in found if os.path.basename(
        f[:-len(relpath + ".flags.txt") - 1]) == stem]
    if len(found) == 1 or len(named) == 1:
        return (found if len(found) == 1 else named)[0]
    sys.exit("style_fix: %d flags files for %s and none named %s alone; "
             "build the style_findings targets that cover it" %
             (len(found), relpath, stem))


def run(cmd, cwd):
    result = subprocess.run(cmd, cwd=cwd, check=False)
    if result.returncode != 0:
        sys.exit("style_fix: %s exited %d" % (cmd[0], result.returncode))


def fix_file(workspace, execroot, check, relpath):
    with open(flags_file(execroot, relpath), encoding="utf-8") as f:
        flags = read_flags(f.read())
    source = os.path.join(workspace, relpath)
    # clang-tidy exits 1 when it reports (and fixes) a finding: not a failure.
    tidy = subprocess.run(
        [os.path.join(execroot, LLVM, "clang-tidy"), "--quiet",
         "--checks=-*,%s" % check, "--fix", "--fix-errors", source, "--"] + flags,
        cwd=execroot, check=False)
    if tidy.returncode > 1:
        sys.exit("style_fix: clang-tidy crashed on %s (%d)" % (relpath, tidy.returncode))
    diff = subprocess.run(["git", "diff", "-U0", "HEAD", "--", relpath],
                          cwd=workspace, check=True, capture_output=True, text=True)
    lines = changed_lines(diff.stdout)
    if lines:
        run([os.path.join(execroot, LLVM, "clang-format"), "--style=file",
             "--assume-filename=" + relpath, "-i"] + lines + [source], workspace)


def main(argv):
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    parser.add_argument("--check", required=True)
    parser.add_argument("--execroot")
    parser.add_argument("paths", nargs="+")
    args = parser.parse_args(argv)
    workspace = os.environ.get("BUILD_WORKSPACE_DIRECTORY")
    if not workspace:
        sys.exit("style_fix: run it with `bazel run //tools:style_fix`")
    execroot = args.execroot or os.path.dirname(
        os.path.realpath(os.path.join(workspace, "bazel-out")))
    for relpath in args.paths:
        fix_file(workspace, execroot, args.check, relpath)


if __name__ == "__main__":
    main(sys.argv[1:])
