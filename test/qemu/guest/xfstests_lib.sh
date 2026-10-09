# The xfstests wrapper's gate and list functions (step 17.1), sourced by
# guest/xfstests.sh after lib.sh and by test/qemu/xfstests_gate_test.sh on the
# host, which feeds them canned results and lists to prove they reject.
#
# A results file has one "generic/NNN <status> <seconds>" line per test run,
# status being pass, fail, notrun or timeout. The lists
# (guest/xfstests.<fstype>.expected_failures and guest/xfstests.excluded) have
# one "generic/NNN <reason>" line per test; blank lines and lines starting with
# "#" are ignored. The group list is xfstests' tests/generic/group.list
# ("NNN group group..." per test).

# xfstests_list_ids FILE: the test ids of a list file, one per line, sorted.
xfstests_list_ids() {
	awk '/^[ \t]*(#|$)/ { next } { print $1 }' "$1" | sort
}

# xfstests_group_ids GROUPLIST: "generic/NNN" for each line of the group list
# (or the line's first field when it has a directory already), sorted.
xfstests_group_ids() {
	awk '/^[ \t]*(#|$)/ { next }
		{ if ($1 ~ /\//) print $1; else print "generic/" $1 }' "$1" | sort
}

# xfstests_select GROUPLIST GROUP EXCLUDED: the ids of the tests in group
# GROUP (a word on their group-list line) that EXCLUDED does not list.
xfstests_select() {
	awk -v group="$2" '
		FILENAME == ARGV[1] { if ($0 !~ /^[ \t]*(#|$)/) excluded[$1] = 1; next }
		/^[ \t]*(#|$)/ { next }
		{
			for (i = 2; i <= NF; i++) if ($i == group) {
				id = ($1 ~ /\//) ? $1 : "generic/" $1
				if (!(id in excluded)) print id
				break
			}
		}' "$3" "$1" | sort
}

# xfstests_shard N K: the lines of stdin at positions N, N+K, N+2K, ... (N is
# 1..K): shard N of K.
xfstests_shard() {
	awk -v n="$1" -v k="$2" '(NR - 1) % k == n - 1'
}

# xfstests_lists_valid EXPECTED EXCLUDED GROUPLIST: the xfstests-lists check
# on the two list files. Every entry has a reason and names a test of the
# group list, none appears twice in its file, and no test is in both.
xfstests_lists_valid() {
	xl_problems=$(awk '
		FILENAME == ARGV[1] {
			id = ($1 ~ /\//) ? $1 : "generic/" $1
			known[id] = 1
			next
		}
		/^[ \t]*(#|$)/ { next }
		{
			which = (FILENAME == ARGV[2]) ? 1 : 2
			if (NF < 2) printf "no reason: %s; ", $1
			if (($1, which) in seen) printf "listed twice: %s; ", $1
			seen[$1, which] = 1
			if (!($1 in known)) printf "not a test in the group list: %s; ", $1
			if (which == 1) expected[$1] = 1
			else excluded[$1] = 1
		}
		END {
			for (id in expected) if (id in excluded)
				printf "both expected to fail and excluded: %s; ", id
		}' "$3" "$1" "$2")
	if [ -z "$xl_problems" ]; then
		pass xfstests-lists
	else
		fail xfstests-lists "$xl_problems"
		return 1
	fi
}

# xfstests_gate RESULTS EXPECTED SHARD: the xfstests-expected-failures check.
# SHARD is the file of this guest's test ids. It fails on a failing (or timed
# out) test the list does not name, on a listed test that passed or did not
# run, on a test with no result, and on a run in which fewer than a quarter
# of the tests ran at all (a broken runtime reports everything as not run,
# which would otherwise pass). Prints the verdict (pass or fail, from lib.sh)
# and returns non-zero when it failed.
xfstests_gate() {
	xg_problems=$(awk '
		FILENAME == ARGV[2] {
			if ($0 !~ /^[ \t]*(#|$)/) listed[$1] = 1
			next
		}
		FILENAME == ARGV[3] { if ($0 !~ /^[ \t]*(#|$)/) { shard[++n] = $1 } next }
		{ status[$1] = $2; total++ }
		END {
			if (total == 0) { print "no results"; exit }
			for (i = 1; i <= n; i++) {
				id = shard[i]
				s = (id in status) ? status[id] : "missing"
				if (s == "pass" || s == "fail" || s == "timeout") ran++
				if (s == "missing") printf "no result: %s; ", id
				else if ((s == "fail" || s == "timeout") && !(id in listed))
					printf "unexpected failure: %s (%s); ", id, s
				else if (s == "pass" && (id in listed))
					printf "listed as failing but passed: %s; ", id
				else if (s == "notrun" && (id in listed))
					printf "listed as failing but did not run: %s; ", id
			}
			if (ran * 8 < n) printf "only %d of %d tests ran; ", ran, n
		}' "$1" "$2" "$3")
	if [ -z "$xg_problems" ]; then
		pass xfstests-expected-failures
	else
		fail xfstests-expected-failures "$xg_problems"
		return 1
	fi
}
