# The pjdfstest wrapper's gate functions (step 26.1), sourced by
# guest/pjdfstest.sh after lib.sh and by test/qemu/pjdfstest_suite_sane_test.sh
# on the host, which feeds them canned results to prove they reject.
#
# A results file has one "<relative-.t-path>:<n>:ok" or "...:notok" line per
# TAP result, as run_suite writes it.

# tap_results REL: reads one .t file's TAP output on stdin and prints the
# "REL:<n>:ok" / "REL:<n>:notok" lines for its result lines; every other line
# (the "1..N" plan, diagnostics) is dropped.
tap_results() {
	awk -v rel="$1" '
		/^ok [0-9]+/     { print rel ":" $2 ":ok" }
		/^not ok [0-9]+/ { print rel ":" $3 ":notok" }
	'
}

# pjdfstest_suite_sane FSTYPE DCFS_RESULTS BACKING_RESULTS: the
# pjdfstest-suite-sane check on the two results files.
#
# A run in which almost everything fails on the raw backing filesystem
# proves nothing: every dcfs failure is then also a backing failure and is
# filtered out as "not dcfs's fault". That is how a busybox without `tail -1`
# (pjdfstest's misc.sh expect() pipes through it) made 8570 of 8827 checks
# fail on both sides while the test passed. On a healthy guest the raw
# filesystem fails only the 28-66 known TODO checks (the backing_failures
# files), well under 1%; require under 5%, that something ran at all, and
# that both runs ran the same number of checks. Prints the verdict line
# (pass or fail, from lib.sh) and returns non-zero when it failed.
pjdfstest_suite_sane() {
	ps_fstype=$1
	ps_total_dcfs=$(wc -l <"$2")
	ps_total_backing=$(wc -l <"$3")
	ps_backing_failed=$(awk -F: '$3 == "notok" { print $1 ":" $2 }' "$3" | sort -u | wc -l)
	if [ "$ps_total_backing" -gt 0 ] && [ $((ps_backing_failed * 20)) -lt "$ps_total_backing" ] &&
		[ "$ps_total_dcfs" -eq "$ps_total_backing" ]; then
		pass pjdfstest-suite-sane
	else
		fail pjdfstest-suite-sane "$ps_backing_failed of $ps_total_backing checks failed directly on $ps_fstype ($ps_total_dcfs ran through dcfs); the pjdfstest tooling in the guest is broken, so no dcfs failure can be told apart from it (see pjdfstest.sh's note and /tmp/pjd-stderr.log)"
		return 1
	fi
}
