#!/bin/sh
# Step 7.2: merge LLVM raw profiles and export them as lcov.
#
#   cov-lcov.sh <llvm-profdata> <llvm-cov> <profraw-dir> <out.lcov> <object>...
#
# <object> are the instrumented binaries that wrote the profiles. Files under
# external/, tests (*_test.cc) and testonly/ are left out (our code only). Used by run-qemu.sh on the profiles
# the guest ships, and by //test/qemu:coverage_pipeline_test.
set -eu

profdata=$1
cov=$2
raw=$3
out=$4
shift 4

if ! ls "$raw"/*.profraw >/dev/null 2>&1; then
	echo "cov-lcov.sh: no .profraw in $raw (no instrumented binary exited normally)" >&2
	: >"$out"
	exit 0
fi
merged="$raw/merged.profdata"
"$profdata" merge -sparse -o "$merged" "$raw"/*.profraw

first=$1
shift
objects=""
for o in "$@"; do
	objects="$objects -object=$o"
done
# The compiler records the paths relative to the execroot under
# /proc/self/cwd (Bazel's compilation directory); the lcov files of the
# source tree say `dcfs/status.cc`.
# shellcheck disable=SC2086 # objects is a list of -object= words
"$cov" export -format=lcov -instr-profile="$merged" \
	-ignore-filename-regex='(^|/)external/|_test\.cc$|/testonly/' $objects "$first" |
	sed 's|^SF:/proc/self/cwd/|SF:|' >"$out"
