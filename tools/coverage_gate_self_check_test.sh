#!/bin/sh
# Self-check of the coverage gate (step 8.1, 26.1 rule): the real
# coverage_gate.sh over canned lcovs against a canned baseline (lines 80.00,
# branches 50.00): below it fails with the numbers, equal and within the 0.1
# tolerance pass, 0.1 above passes with a NOTE naming the new baseline (a rise
# must not turn a push red) and appends it to $GITHUB_STEP_SUMMARY, a drop
# on either metric fails, bench/ and tools/ records and headers do not count.
set -eu

gate=$1
dir=$2

expect() { # expect pass|fail <lcov> [pattern]
	want=$1
	file=$dir/$2
	if out=$("$gate" "$file" "$dir/baseline.txt" 2>&1); then got=pass; else got=fail; fi
	if [ "$got" != "$want" ]; then
		echo "FAIL: $2: expected $want, got $got:" >&2
		echo "$out" >&2
		exit 1
	fi
	if [ -n "${3:-}" ] && ! echo "$out" | grep -q -- "$3"; then
		echo "FAIL: $2: output lacks '$3':" >&2
		echo "$out" >&2
		exit 1
	fi
}

expect fail below.lcov "lines 79.90 (baseline 80.00)"
expect fail below_branches.lcov "branches 49.90 (baseline 50.00)"
expect pass equal.lcov "ok: lines 80.00"
expect pass within_tolerance.lcov "ok: lines 80.05"
expect pass above_lines.lcov "NOTE: coverage rose.*raise dcfs/coverage_baseline.txt to 80.10 / 50.00"
expect pass above_branches.lcov "NOTE: coverage rose.*raise dcfs/coverage_baseline.txt to 80.00 / 50.10"
# One metric up by 0.2 and the other down: a drop fails, and the note is
# still printed for the rise.
expect fail above_both_low_one.lcov "fell below the baseline"
expect fail above_both_low_one.lcov "raise dcfs/coverage_baseline.txt to 80.20 / 50.00"
# The note also goes to the CI job summary.
summary=$(mktemp)
GITHUB_STEP_SUMMARY=$summary "$gate" "$dir/above_lines.lcov" "$dir/baseline.txt" >/dev/null 2>&1
if ! grep -q "to 80.10 / 50.00" "$summary"; then
	echo "FAIL: the NOTE is not in GITHUB_STEP_SUMMARY" >&2
	exit 1
fi
rm -f "$summary"
# A branch count of 2^32-1 is llvm-cov's reading of a negative difference of
# two counters (step 26.14c, docs/coverage.md), not a count: the gate fails
# naming the branch instead of counting it as taken; a large real count below
# 2^31 is a count.
expect fail artifact_count.lcov "dcfs/a.cc:863 branch 1.2 count 4294967295"
expect pass large_count.lcov "ok: lines 80.00"
# bench/ and tools/ are reported, not gated; dcfs headers do not count.
expect pass equal.lcov "bench: lines 1/100"
expect pass equal.lcov "tools: lines 2/100"
