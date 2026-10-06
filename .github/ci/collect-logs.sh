#!/bin/bash
# Copies every test's log, result and guest console (test.outputs/) out of
# bazel-testlogs, a symlink into the output base that actions/upload-artifact
# would not follow, into ./ci-test-logs/<label>. Called by test.sh right
# after a failed `bazel test`, because bazel-testlogs points at the latest
# invocation's configuration only.
set -euo pipefail
label="${1:-all}"
out="$PWD/ci-test-logs/$label"
mkdir -p "$out"
cd "$(readlink -f bazel-testlogs)"
find . \( -name test.log -o -name test.xml -o -name outputs.zip -o -path '*/test.outputs/*' \) -type f -print0 |
	xargs -0 -r cp --parents -t "$out"
du -sh "$out"
