# The xfstests wrapper's gate and list functions (step 17.1), sourced by
# guest/xfstests.sh after lib.sh and by test/qemu/xfstests_gate_test.sh on the
# host, which feeds them canned results and lists to prove they reject.
#
# A results file has one "generic/NNN <status> <seconds> [<reason>]" line per
# test run, status being pass, fail, notrun or timeout; the reason of a "not
# run" test is the first line of its .notrun file, cut to 100 characters with
# every run of digits replaced by N (xfstests_reason), so that a size or a
# pid in the text does not change it.
#
# The lists, one "generic/NNN <reason>" line per test (blank lines and lines
# starting with "#" are ignored):
#   xfstests.<fstype>.expected_failures  tests that fail through dcfs, why
#   xfstests.<fstype>.notrun             tests that are "not run", with the
#                                        reason xfstests gives (generated from
#                                        a run: test/qemu/README.md, "xfstests")
#   xfstests.excluded, xfstests.<fstype>.excluded
#                                        tests that are not run at all, why
# The group list is xfstests' tests/generic/group.list ("NNN group group..."
# per test).

# xfstests_reason TEXT: the first line of TEXT in the form the results and the
# notrun lists keep it.
xfstests_reason() {
	printf '%s\n' "$1" | head -n 1 | sed 's/[0-9][0-9]*/N/g; s/[ \t][ \t]*/ /g' | cut -c1-100
}

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

# xfstests_lists_valid EXPECTED EXCLUDED GROUPLIST NOTRUN SELECTED: the
# xfstests-lists check on the list files (EXCLUDED is the global and the
# file system's lists in one file; SELECTED is the file of the ids that run).
# Every entry has a reason and names a test of the group list, none appears
# twice in its file, no test is in two of the three lists, and every expected
# failure and every "not run" test is one that runs (a stale entry names a
# test that was excluded, or is gone).
xfstests_lists_valid() {
	xl_problems=$(awk '
		FILENAME == ARGV[1] {
			id = ($1 ~ /\//) ? $1 : "generic/" $1
			known[id] = 1
			next
		}
		FILENAME == ARGV[5] { if ($0 !~ /^[ \t]*(#|$)/) selected[$1] = 1; next }
		/^[ \t]*(#|$)/ { next }
		{
			which = (FILENAME == ARGV[2]) ? 1 : (FILENAME == ARGV[3]) ? 2 : 3
			if (NF < 2) printf "no reason: %s; ", $1
			if (($1, which) in seen) printf "listed twice: %s; ", $1
			seen[$1, which] = 1
			if (!($1 in known)) printf "not a test in the group list: %s; ", $1
			if (which == 1) expected[$1] = 1
			else if (which == 2) excluded[$1] = 1
			else notrun[$1] = 1
		}
		END {
			for (id in expected) {
				if (id in excluded) printf "both expected to fail and excluded: %s; ", id
				if (id in notrun) printf "both expected to fail and not run: %s; ", id
				if (!(id in selected) && !(id in excluded)) printf "expected to fail but not run by this set: %s; ", id
			}
			for (id in notrun) {
				if (id in excluded) printf "both not run and excluded: %s; ", id
				if (!(id in selected) && !(id in excluded)) printf "listed as not run but not run by this set: %s; ", id
			}
		}' "$3" "$1" "$2" "$4" "$5")
	if [ -z "$xl_problems" ]; then
		pass xfstests-lists
	else
		fail xfstests-lists "$xl_problems"
		return 1
	fi
}

# xfstests_gate RESULTS EXPECTED SHARD NOTRUN: the xfstests-expected-failures
# check. SHARD is the file of this guest's test ids. It fails on:
#   - a failing or timed-out test the expected-failure list does not name;
#   - a listed test that passed, did not run, or timed out (a listed failure is
#     deterministic, and a test that cannot finish does not fail);
#   - a test that is "not run" and not on the notrun list, or on it with
#     another reason (xfstests turns a broken probe of O_DIRECT, fallocate,
#     xattrs, ACLs, SEEK_HOLE, statx, ... into "not run", so a test that used
#     to pass must not slip into it unseen), and a listed "not run" test that
#     now ran;
#   - a test with no result;
#   - a run in which fewer than an eighth of the tests ran at all (about a
#     fifth of them do, the rest being "not run" for what FUSE cannot do; a
#     broken runtime reports everything as not run).
# Prints the verdict (pass or fail, from lib.sh) and returns non-zero when it
# failed.
xfstests_gate() {
	xg_problems=$(awk '
		function rest(from,   r, i) {
			r = $from
			for (i = from + 1; i <= NF; i++) r = r " " $i
			return r
		}
		FILENAME == ARGV[2] {
			if ($0 !~ /^[ \t]*(#|$)/) listed[$1] = 1
			next
		}
		FILENAME == ARGV[3] { if ($0 !~ /^[ \t]*(#|$)/) { shard[++n] = $1 } next }
		FILENAME == ARGV[4] {
			if ($0 !~ /^[ \t]*(#|$)/) notrun[$1] = rest(2)
			next
		}
		{ status[$1] = $2; reason[$1] = rest(4); total++ }
		END {
			if (total == 0) { print "no results"; exit }
			for (i = 1; i <= n; i++) {
				id = shard[i]
				s = (id in status) ? status[id] : "missing"
				if (s == "pass" || s == "fail" || s == "timeout") ran++
				if (s == "missing") printf "no result: %s; ", id
				else if (s == "timeout" && (id in listed))
					printf "listed test timed out: %s; ", id
				else if ((s == "fail" || s == "timeout") && !(id in listed))
					printf "unexpected failure: %s (%s); ", id, s
				else if (s == "pass" && (id in listed))
					printf "listed as failing but passed: %s; ", id
				else if (s == "notrun" && (id in listed))
					printf "listed as failing but did not run: %s; ", id
				else if (s == "notrun" && !(id in notrun))
					printf "unexpected not run: %s (%s); ", id, reason[id]
				else if (s == "notrun" && notrun[id] != reason[id])
					printf "not-run reason changed: %s (was \"%s\", now \"%s\"); ", id, notrun[id], reason[id]
				if ((s == "pass" || s == "fail" || s == "timeout") && (id in notrun))
					printf "listed as not run but ran: %s (%s); ", id, s
			}
			if (ran * 8 < n) printf "only %d of %d tests ran; ", ran, n
		}' "$1" "$2" "$3" "$4")
	if [ -z "$xg_problems" ]; then
		pass xfstests-expected-failures
	else
		fail xfstests-expected-failures "$xg_problems"
		return 1
	fi
}
