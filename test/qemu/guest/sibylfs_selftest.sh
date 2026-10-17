#!/bin/sh
# SPDX-License-Identifier: MIT
# dcfs step 12.18b: the checks of the differential test's own parts
# (guest/sibylfs_lib.sh), with the real SibylFS tools and no dcfs. The wrapper
# must turn the checker's verdict line into an exit status and tell "rejected"
# from "no verdict"; the script chooser must pick what its spec names; the
# gates must pass what they should and fail what they should.
#
# Run as /tests/sibylfs_selftest.sh by guest/init when booted with
# dcfs_test=sibylfs_selftest.sh.
FAILED=0
. "$(dirname "$0")/lib.sh"
. "$(dirname "$0")/sibylfs_lib.sh"
require_commands awk cat find grep mkdir rm sed sort wc

WORK=/tmp/sibylfs-selftest
SIBYLFS_TMP=$WORK/tmp
SIBYLFS_LOG=$WORK/posix.log
mkdir -p "$WORK" "$SIBYLFS_TMP"
sibylfs_install_account_commands

# expect_status NAME WANT GOT: a check that a status is the wanted one.
expect_status() {
	if [ "$3" -eq "$2" ]; then
		pass "$1"
	else
		fail "$1" "status $3, wanted $2"
	fi
}

# --- sibylfs_check: the verdict line becomes the exit status ----------------

printf '%s\n' '@type trace' 'mkdir "/foo" 0o777' 'Tau' 'RV_none' >"$WORK/accepted.trace"
sibylfs_check "$WORK/accepted.trace" "$WORK/accepted.out"
expect_status check-accepted 0 $?

# The same directory made twice: the second mkdir must fail with EEXIST.
printf '%s\n' '@type trace' 'mkdir "/foo" 0o777' 'Tau' 'RV_none' \
	'mkdir "/foo" 0o777' 'Tau' 'RV_none' >"$WORK/rejected.trace"
sibylfs_check "$WORK/rejected.trace" "$WORK/rejected.out"
expect_status check-rejected 1 $?
if grep -q 'allowed are only: EEXIST' "$WORK/rejected.out"; then
	pass check-rejected-keeps-the-reason
else
	fail check-rejected-keeps-the-reason "the checker's output has no 'allowed are only: EEXIST' line: $(cat "$WORK/rejected.out")"
fi

printf '%s\n' 'this is not a trace' >"$WORK/garbage.trace"
sibylfs_check "$WORK/garbage.trace" "$WORK/garbage.out"
expect_status check-no-verdict 2 $?

# --- the tools end to end: a real run is accepted, a doctored one rejected ---

printf '%s\n' '@type script' 'mkdir "/d1" 0o777' 'open_close "/d1/f1.txt" [O_CREAT;O_RDWR] 0o666' \
	'unlink "/d1/f1.txt"' 'rmdir "/d1"' >"$WORK/run.script"
sibylfs_run "$WORK/run.script" "$WORK/root1" "$WORK/run.trace"
expect_status run-script 0 $?
sibylfs_check "$WORK/run.trace" "$WORK/run.out"
expect_status run-accepted 0 $?
# The mkdir of d1 is the first result; claim it failed.
awk '!done && $1 == "RV_none" { print "        EEXIST"; done = 1; next } { print }' \
	"$WORK/run.trace" >"$WORK/doctored.trace"
sibylfs_check "$WORK/doctored.trace" "$WORK/doctored.out"
expect_status run-doctored-rejected 1 $?

# A script that names a user and a group runs: fs_test_posix finds the account
# commands, and forgetting its accounts afterwards leaves none.
printf '%s\n' '@type script' 'Pid 2 -> create (User_id 1) (Group_id 1)' \
	'Pid 2 -> mkdir "d" 0o777' 'stat "d"' >"$WORK/users.script"
sibylfs_run "$WORK/users.script" "$WORK/root2" "$WORK/users.trace"
expect_status run-users-script 0 $?
useradd -s /bin/false fs_user_7
groupadd fs_group_7
sibylfs_forget_accounts
if grep -q '^fs_user_7:' /etc/passwd || grep -q '^fs_group_7:' /etc/group; then
	fail forget-accounts "fs_user_7 or fs_group_7 is still there: $(grep fs_ /etc/passwd /etc/group)"
else
	pass forget-accounts
fi

# --- sibylfs_scripts ---------------------------------------------------------

SUITE=$WORK/suite
mkdir -p "$SUITE/top/test-suite/g.tmpl" "$SUITE/top/test-suite/open"
for f in open/g0 open/g1 open/g2 open/g3 open/g4 open/g5 open/adhoc_a open/adhoc_b \
	adhoc_c open/g6-check g.tmpl/g7; do
	: >"$SUITE/top/test-suite/$f-int.trace"
done
: >"$SUITE/top/test-suite/open/readme.txt"
want_scripts() {
	ws_name=$1
	ws_spec=$2
	shift 2
	ws_got=$(sibylfs_scripts "$SUITE" "$ws_spec" | sed 's|.*/test-suite/||; s|-int.trace$||' | tr '\n' ' ')
	if [ "$ws_got" = "$*" ] || [ "$ws_got" = "$* " ]; then
		pass "$ws_name"
	else
		fail "$ws_name" "got '$ws_got', wanted '$*'"
	fi
}
want_scripts scripts-adhoc adhoc adhoc_c open/adhoc_a open/adhoc_b
want_scripts scripts-shard-1-of-3 1/3 open/g1 open/g4
want_scripts scripts-shard-0-of-3 0/3 open/g0 open/g3
want_scripts scripts-shards-cover-the-generated "0/3,1/3,2/3" open/g0 open/g1 open/g2 open/g3 open/g4 open/g5
want_scripts scripts-adhoc-and-a-shard adhoc,2/3 adhoc_c open/adhoc_a open/adhoc_b open/g2 open/g5

# --- sibylfs_regressions and sibylfs_suite_sane --------------------------------

printf '%s\n' 'a 0 0' 'b 1 0' 'c 1 1' 'd 2 0' 'e 0 1' 'f 2 2' >"$WORK/verdicts"
got=$(sibylfs_regressions "$WORK/verdicts" | tr '\n' ' ')
if [ "$got" = "b d " ]; then
	pass regressions-are-dcfs-rejected-backing-accepted
else
	fail regressions-are-dcfs-rejected-backing-accepted "got '$got', wanted 'b d '"
fi

# sibylfs_in and sibylfs_not_in, with a list that is empty and one that is not.
printf '%s\n' a b c >"$WORK/file"
printf '%s\n' b c d >"$WORK/list"
got=$(sibylfs_in "$WORK/list" "$WORK/file" | tr '\n' ' ')
[ "$got" = "b c " ] && pass in-keeps-the-listed || fail in-keeps-the-listed "got '$got'"
got=$(sibylfs_not_in "$WORK/list" "$WORK/file" | tr '\n' ' ')
[ "$got" = "a " ] && pass not-in-keeps-the-unlisted || fail not-in-keeps-the-unlisted "got '$got'"
got=$(sibylfs_not_in /nonexistent "$WORK/file" | tr '\n' ' ')
[ "$got" = "a b c " ] && pass not-in-nothing-keeps-all || fail not-in-nothing-keeps-all "got '$got'"
: >"$WORK/nothing"
got=$(sibylfs_not_in "$WORK/nothing" "$WORK/file" | tr '\n' ' ')
[ "$got" = "a b c " ] && pass not-in-an-empty-list-keeps-all || fail not-in-an-empty-list-keeps-all "got '$got'"

# expect_sane NAME WANT_STATUS FILE: sibylfs_suite_sane's status on FILE; its
# own pass/fail line is for the run it gates, so it goes to a file here.
expect_sane() {
	es_failed_before=$FAILED
	sibylfs_suite_sane ext4 "$3" >"$WORK/sane.out"
	es_status=$?
	FAILED=$es_failed_before
	expect_status "$1" "$2" "$es_status"
}
: >"$WORK/empty"
expect_sane sane-fails-with-nothing-run 1 "$WORK/empty"
awk 'BEGIN { for (i = 0; i < 20; i++) print "s" i, 0, 0 }' >"$WORK/all-accepted"
expect_sane sane-passes-when-the-model-accepts-the-backing 0 "$WORK/all-accepted"
awk 'BEGIN { for (i = 0; i < 20; i++) print "s" i, 0, 0; print "t 0 2" }' >"$WORK/one-unjudged"
expect_sane sane-fails-on-a-trace-without-a-verdict 1 "$WORK/one-unjudged"
awk 'BEGIN { for (i = 0; i < 11; i++) print "s" i, 1, 1; for (i = 0; i < 9; i++) print "t" i, 0, 0 }' >"$WORK/half-rejected"
expect_sane sane-fails-when-the-backing-is-mostly-rejected 1 "$WORK/half-rejected"

exit "$FAILED"
