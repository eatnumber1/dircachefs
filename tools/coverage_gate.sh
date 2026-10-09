#!/bin/sh
# Step 8.1: the coverage gate (CI coverage job only).
#
#   coverage_gate.sh <coverage.lcov> <baseline file>
#
# Computes the line and branch coverage of dcfs/*.cc from the published lcov
# (headers, bench/ and tools/ are printed, not gated) and compares it with the
# committed baseline (dcfs/coverage_baseline.txt: `lines 92.40`, `branches
# 73.30`):
#   - below the baseline on either: FAIL with the numbers;
#   - above it by 0.1 or more on either: pass, with a NOTE (also appended to
#     $GITHUB_STEP_SUMMARY when set) naming the values to put in the baseline
#     in a later commit (the baseline only moves up; a rise is good news and
#     must not turn a push red; 0.1 is the tolerance for noise);
#   - otherwise pass.
# Self-check: //tools:coverage_gate_self_check_test.
set -eu

lcov=$1
baseline=$2

base_of() {
	awk -v k="$1" '$1 == k { print $2 }' "$baseline"
}
base_lines=$(base_of lines)
base_branches=$(base_of branches)
if [ -z "$base_lines" ] || [ -z "$base_branches" ]; then
	echo "coverage_gate.sh: $baseline needs a 'lines' and a 'branches' value" >&2
	exit 2
fi

# A branch count of 2^31 or more is not a count: llvm-cov computes a branch's
# count as a difference of region counters, and counters that lost an update
# (docs/coverage.md, "Known coverage artifacts") give a negative difference,
# which it prints as 4294967295. Such a branch would count as taken, so the
# report cannot be trusted: name every one and fail.
artifacts=$(awk '
	/^SF:/ { sf = substr($0, 4) }
	/^BRDA:/ { split(substr($0, 6), f, ",")
		if (f[4] != "-" && f[4] + 0 >= 2147483648)
			print sf ":" f[1] " branch " f[2] "." f[3] " count " f[4] }
' "$lcov")
if [ -n "$artifacts" ]; then
	echo "coverage_gate.sh: FAIL: branch counts that are counter-underflow artifacts, not counts (docs/coverage.md); rerun the coverage job:" >&2
	echo "$artifacts" >&2
	exit 1
fi

# "<scope> <LH> <LF> <BRH> <BRF>" per scope: the sums over the records whose
# SF: is under the scope's prefix (dcfs/*.cc only for the gated one).
totals=$(awk '
	/^SF:/ { sf = substr($0, 4); scope = ""
		if (sf ~ /^dcfs\/.*\.cc$/) scope = "dcfs"
		else if (sf ~ /^bench\//) scope = "bench"
		else if (sf ~ /^tools\//) scope = "tools" }
	scope != "" && /^LH:/ { v[scope, "lh"] += substr($0, 4) }
	scope != "" && /^LF:/ { v[scope, "lf"] += substr($0, 4) }
	scope != "" && /^BRH:/ { v[scope, "brh"] += substr($0, 5) }
	scope != "" && /^BRF:/ { v[scope, "brf"] += substr($0, 5) }
	END { n = split("dcfs bench tools", names, " ")
		for (i = 1; i <= n; i++) { s = names[i]
			print s, v[s, "lh"] + 0, v[s, "lf"] + 0, v[s, "brh"] + 0, v[s, "brf"] + 0 } }
' "$lcov")

pct() { awk -v h="$1" -v f="$2" 'BEGIN { if (f == 0) print "0.00"; else printf "%.2f", 100 * h / f }'; }
floor2() { awk -v h="$1" -v f="$2" 'BEGIN { if (f == 0) print "0.00"; else printf "%.2f", int(10000 * h / f) / 100 }'; }

fail=0
dcfs_lh=0 dcfs_lf=0 dcfs_brh=0 dcfs_brf=0
echo "$totals" | while read -r scope lh lf brh brf; do
	echo "coverage_gate.sh: $scope: lines $lh/$lf = $(pct "$lh" "$lf")%, branches $brh/$brf = $(pct "$brh" "$brf")%$([ "$scope" = dcfs ] || echo ' (reported, not gated)')"
done
set -- $(echo "$totals" | awk '$1 == "dcfs" { print $2, $3, $4, $5 }')
dcfs_lh=$1 dcfs_lf=$2 dcfs_brh=$3 dcfs_brf=$4
if [ "$dcfs_lf" -eq 0 ]; then
	echo "coverage_gate.sh: FAIL: no dcfs/*.cc line in $lcov" >&2
	exit 1
fi
lines=$(pct "$dcfs_lh" "$dcfs_lf")
branches=$(pct "$dcfs_brh" "$dcfs_brf")

cmp() { # cmp A B: -1, 0, 1 at two decimals
	awk -v a="$1" -v b="$2" 'BEGIN { x = int(a * 100 + 0.5); y = int(b * 100 + 0.5); print (x < y) ? -1 : (x > y) ? 1 : 0 }'
}
raised() { # raised A B: 1 when A is at least 0.1 above B
	awk -v a="$1" -v b="$2" 'BEGIN { print (int(a * 100 + 0.5) - int(b * 100 + 0.5) >= 10) ? 1 : 0 }'
}

if [ "$(cmp "$lines" "$base_lines")" -lt 0 ] || [ "$(cmp "$branches" "$base_branches")" -lt 0 ]; then
	echo "coverage_gate.sh: FAIL: dcfs/*.cc coverage fell below the baseline: lines $lines (baseline $base_lines), branches $branches (baseline $base_branches)" >&2
	fail=1
fi
if [ "$(raised "$lines" "$base_lines")" -eq 1 ] || [ "$(raised "$branches" "$base_branches")" -eq 1 ]; then
	new_lines=$base_lines
	new_branches=$base_branches
	[ "$(raised "$lines" "$base_lines")" -eq 1 ] && new_lines=$(floor2 "$dcfs_lh" "$dcfs_lf")
	[ "$(raised "$branches" "$base_branches")" -eq 1 ] && new_branches=$(floor2 "$dcfs_brh" "$dcfs_brf")
	note="coverage_gate.sh: NOTE: coverage rose (lines $lines, branches $branches; baseline $base_lines / $base_branches): raise dcfs/coverage_baseline.txt to $new_lines / $new_branches"
	echo "$note"
	if [ -n "${GITHUB_STEP_SUMMARY:-}" ]; then
		echo "$note" >>"$GITHUB_STEP_SUMMARY"
	fi
fi
if [ "$fail" -eq 0 ]; then
	echo "coverage_gate.sh: ok: lines $lines (baseline $base_lines), branches $branches (baseline $base_branches)"
fi
exit "$fail"
