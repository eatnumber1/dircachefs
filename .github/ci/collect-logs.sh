#!/bin/bash
# Copies every test's log, result and guest console (test.outputs/) out of
# bazel-testlogs, a symlink into the output base that actions/upload-artifact
# would not follow, into ./ci-test-logs/<label>. Called by test.sh right
# after a failed `bazel test`, because bazel-testlogs points at the latest
# invocation's configuration only. Bazel leaves the outputs read-only (files
# 0444 in directories 0555); `cp --parents` gives each directory it makes the
# source's mode once it has put a file in, so the second file of a directory
# could not be created (step 26.22: 157 files of the coverage job's artifact).
# --no-preserve=mode copies with our own modes.
set -euo pipefail
label="${1:-all}"
out="$PWD/ci-test-logs/$label"
mkdir -p "$out"
cd "$(readlink -f bazel-testlogs)"
find . \( -name test.log -o -name test.xml -o -name outputs.zip -o -path '*/test.outputs/*' \) -type f -print0 |
	xargs -0 -r cp --no-preserve=mode --parents -t "$out"
du -sh "$out"
