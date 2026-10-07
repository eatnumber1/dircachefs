#!/bin/sh
# Self-check of the coverage gate (step 8.1, 26.1 rule): the real
# coverage_gate.sh over canned lcovs against a canned baseline (lines 80.00,
# branches 50.00): below it fails with the numbers, equal and within the 0.1
# tolerance pass, 0.1 above fails with the line to put in the baseline,
# bench/ and tools/ records and headers do not count.
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
expect fail above_lines.lcov "raise dcfs/coverage_baseline.txt to 80.10 / 50.00 in this commit"
expect fail above_branches.lcov "raise dcfs/coverage_baseline.txt to 80.00 / 50.10 in this commit"
# One metric up by 0.2 and the other down: both messages.
expect fail above_both_low_one.lcov "fell below the baseline"
expect fail above_both_low_one.lcov "raise dcfs/coverage_baseline.txt to 80.20 / 50.00"
# bench/ and tools/ are reported, not gated; dcfs headers do not count.
expect pass equal.lcov "bench: lines 1/100"
expect pass equal.lcov "tools: lines 2/100"
