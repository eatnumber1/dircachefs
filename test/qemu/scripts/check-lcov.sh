#!/bin/sh
# Step 7.2 gate: an lcov file (a test's coverage.dat or the combined report)
# must be non-empty and show at least one executed line of <source>.
#
#   check-lcov.sh <file.lcov> <source-path> [--zero <function>]
#
# --zero: also require <function> in <source> to be reported with 0 hits (a
# known-uncovered function: the report must show what was NOT run too).
# Self-check: //test/qemu:coverage_pipeline_test.
set -eu

lcov=$1
source=$2
zero=""
if [ "${3:-}" = "--zero" ]; then
	zero=$4
fi

if [ ! -s "$lcov" ]; then
	echo "check-lcov.sh: FAIL: $lcov is empty or missing" >&2
	exit 1
fi

# The record of <source>: from its SF: line to end_of_record.
record=$(awk -v src="$source" '
	$0 == "SF:" src { on = 1 }
	on { print }
	on && /^end_of_record/ { exit }
' "$lcov")
if [ -z "$record" ]; then
	echo "check-lcov.sh: FAIL: no SF:$source record in $lcov" >&2
	exit 1
fi
hit=$(printf '%s\n' "$record" | sed -n 's/^LH:\([0-9]*\)$/\1/p' | head -n 1)
if [ "${hit:-0}" -le 0 ]; then
	echo "check-lcov.sh: FAIL: $source has no executed line (LH:${hit:-none})" >&2
	exit 1
fi
if [ -n "$zero" ]; then
	hits=$(printf '%s\n' "$record" | sed -n 's/^FNDA:\([0-9]*\),\(.*\)$/\1 \2/p' | awk -v f="$zero" 'index($2, f) { print $1; exit }')
	if [ -z "$hits" ]; then
		echo "check-lcov.sh: FAIL: function $zero is not in the $source record" >&2
		exit 1
	fi
	if [ "$hits" -ne 0 ]; then
		echo "check-lcov.sh: FAIL: function $zero has $hits hits, expected 0" >&2
		exit 1
	fi
fi
echo "check-lcov.sh: ok: $source: LH:$hit"
