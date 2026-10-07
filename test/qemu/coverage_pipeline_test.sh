#!/bin/sh
# Self-check of the coverage pipeline (step 7.2), on the host (it needs
# neither root nor a kernel): an instrumented program runs, its profile goes
# through scripts/cov-lcov.sh exactly as run-qemu.sh does, and the lcov must
# show the function that ran with hits and the one that did not with none
# (scripts/check-lcov.sh --zero). A report that left out what was not run,
# or an empty one, would fail here.
#
#   coverage_pipeline_test.sh <cov-lcov.sh> <check-lcov.sh> <llvm-profdata>
#       <llvm-cov> <fixture>
set -eu

cov_lcov=$1
check=$2
profdata=$3
llvm_cov=$4
fixture=$5

dir="${TEST_TMPDIR:-$(mktemp -d)}/prof"
mkdir -p "$dir"
LLVM_PROFILE_FILE="$dir/%m.profraw" "$fixture" >/dev/null
"$cov_lcov" "$profdata" "$llvm_cov" "$dir" "$dir/out.lcov" "$fixture"

src=$(sed -n 's|^SF:.*\(test/qemu/testdata/cov_fixture\.cc\)$|\1|p' "$dir/out.lcov" | head -n 1)
if [ -z "$src" ]; then
	echo "FAIL: no cov_fixture.cc in the lcov:" >&2
	cat "$dir/out.lcov" >&2
	exit 1
fi
"$check" "$dir/out.lcov" "$src" --zero Uncovered
# And the gate rejects what it should: an empty file, a file without the
# source, and a covered function claimed to be uncovered.
: >"$dir/empty.lcov"
if "$check" "$dir/empty.lcov" "$src" 2>/dev/null; then
	echo "FAIL: an empty lcov was accepted" >&2
	exit 1
fi
if "$check" "$dir/out.lcov" no/such/file.cc 2>/dev/null; then
	echo "FAIL: a missing source was accepted" >&2
	exit 1
fi
if "$check" "$dir/out.lcov" "$src" --zero Covered 2>/dev/null; then
	echo "FAIL: a function with hits was accepted as uncovered" >&2
	exit 1
fi
