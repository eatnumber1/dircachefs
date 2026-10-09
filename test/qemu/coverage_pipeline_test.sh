#!/bin/sh
# Self-check of the coverage pipeline (step 7.2), on the host (it needs
# neither root nor a kernel): an instrumented program runs, its profile goes
# through scripts/cov-lcov.sh exactly as run-qemu.sh does, and the lcov must
# show the function that ran with hits and the one that did not with none
# (scripts/check-lcov.sh --zero). A report that left out what was not run,
# or an empty one, or a truncated profile taken for good, would fail here.
#
#   coverage_pipeline_test.sh <cov-lcov.sh> <check-lcov.sh> <llvm-profdata>
#       <llvm-cov> <fixture> <fork-fixture>
set -eu

cov_lcov=$1
check=$2
profdata=$3
llvm_cov=$4
fixture=$5
fork_fixture=$6

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

# A profile cut short (a process killed while writing it) must fail the step
# and name the file, not silently drop the coverage of its binary: one good
# and one truncated profile together fail too.
bad="$dir/bad"
mkdir -p "$bad"
cp "$dir"/*.profraw "$bad/good.profraw"
cp "$dir"/*.profraw "$bad/cut.profraw"
size=$(wc -c <"$bad/cut.profraw")
truncate -s $((size / 2)) "$bad/cut.profraw"
if "$cov_lcov" "$profdata" "$llvm_cov" "$bad" "$bad/out.lcov" "$fixture" 2>"$bad/err"; then
	echo "FAIL: a truncated profile was accepted" >&2
	exit 1
fi
if ! grep -q "cut.profraw" "$bad/err"; then
	echo "FAIL: the error does not name the truncated profile:" >&2
	cat "$bad/err" >&2
	exit 1
fi

# Step 26.14f: a fork under continuous-mode profiling (the guests' mode: the
# processes share the profile's counters). A function that returns in both
# processes makes llvm-cov derive a negative count; cov-lcov.sh must refuse
# the report and name the line, not hand an artifact on to the gate (a count
# of 4294967295 would pass for a taken branch).
fork="$dir/fork"
mkdir -p "$fork"
LLVM_PROFILE_FILE="$fork/%m%c.profraw" "$fork_fixture" twice
if "$cov_lcov" "$profdata" "$llvm_cov" "$fork" "$fork/out.lcov" "$fork_fixture" 2>"$fork/err"; then
	echo "FAIL: a report with a counter-underflow artifact was accepted" >&2
	exit 1
fi
if ! grep -q "cov_fork_fixture.cc:" "$fork/err"; then
	echo "FAIL: the error does not name the artifact's source line:" >&2
	cat "$fork/err" >&2
	exit 1
fi

# The same fork through dcfs::ForkSplit (which returns in the child only)
# gives a report cov-lcov.sh accepts, and Parent() and Child() each ran once.
split="$dir/split"
mkdir -p "$split"
LLVM_PROFILE_FILE="$split/%m%c.profraw" "$fork_fixture" split
"$cov_lcov" "$profdata" "$llvm_cov" "$split" "$split/out.lcov" "$fork_fixture"
for fn in Parent Child; do
	hits=$(sed -n 's/^FNDA:\([0-9]*\),\(.*\)$/\1 \2/p' "$split/out.lcov" | awk -v f="$fn" 'index($2, f) { print $1; exit }')
	if [ "${hits:-}" != 1 ]; then
		echo "FAIL: $fn ran once but the lcov says ${hits:-nothing}:" >&2
		cat "$split/out.lcov" >&2
		exit 1
	fi
done
