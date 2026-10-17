#!/bin/sh
# SPDX-License-Identifier: MIT
# dcfs step 12.18b: the checks of the differential test's parts that need the
# real SibylFS tools and root (guest/sibylfs_lib.sh), with no dcfs: the
# wrapper must turn the checker's verdict line into an exit status and tell
# "rejected" from "no verdict"; a real run must be accepted and the same trace
# doctored must be rejected; the account commands that fs_test_posix runs must
# make and remove users and groups. The script chooser and the gates, which
# need no tools, are test/qemu/sibylfs_lib_test.sh on the host.
#
# Run as /tests/sibylfs_selftest.sh by guest/init when booted with
# dcfs_test=sibylfs_selftest.sh.
FAILED=0
. "$(dirname "$0")/lib.sh"
. "$(dirname "$0")/sibylfs_lib.sh"
require_commands addgroup adduser awk cat deluser delgroup grep mkdir

WORK=/tmp/sibylfs-selftest
SIBYLFS_TMP=$WORK/tmp
SIBYLFS_LOG=$WORK/posix.log

# expect_status NAME WANT GOT: a check that a status is the wanted one.
expect_status() {
  if [ "$3" -eq "$2" ]; then
    pass "$1"
  else
    fail "$1" "status $3, wanted $2"
  fi
}

# expect_absent NAME PATTERN FILE: a check that no line of FILE matches.
expect_absent() {
  if grep -q "$2" "$3"; then
    fail "$1" "$2 is still in $3: $(grep "$2" "$3")"
  else
    pass "$1"
  fi
}

main() {
  mkdir -p "$WORK" "$SIBYLFS_TMP"
  sibylfs_install_account_commands

  # --- sibylfs_check: the verdict line becomes the exit status --------------

  printf '%s\n' '@type trace' 'mkdir "/foo" 0o777' 'Tau' 'RV_none' \
    >"$WORK/accepted.trace"
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
    fail check-rejected-keeps-the-reason "the checker's output has no \
'allowed are only: EEXIST' line: $(cat "$WORK/rejected.out")"
  fi

  printf '%s\n' 'this is not a trace' >"$WORK/garbage.trace"
  sibylfs_check "$WORK/garbage.trace" "$WORK/garbage.out"
  expect_status check-no-verdict 2 $?

  # --- the tools end to end: a real run is accepted, a doctored one not -----

  printf '%s\n' '@type script' 'mkdir "/d1" 0o777' \
    'open_close "/d1/f1.txt" [O_CREAT;O_RDWR] 0o666' \
    'unlink "/d1/f1.txt"' 'rmdir "/d1"' >"$WORK/run.script"
  sibylfs_run "$WORK/run.script" "$WORK/root1" "$WORK/run.trace"
  expect_status run-script 0 $?
  sibylfs_check "$WORK/run.trace" "$WORK/run.out"
  expect_status run-accepted 0 $?
  # The mkdir of d1 is the first result; claim it failed.
  awk '!done && $1 == "RV_none" { print "        EEXIST"; done = 1; next }
    { print }' "$WORK/run.trace" >"$WORK/doctored.trace"
  sibylfs_check "$WORK/doctored.trace" "$WORK/doctored.out"
  expect_status run-doctored-rejected 1 $?

  # A script that names a user and a group runs: fs_test_posix finds the
  # account commands, and removes the accounts it made.
  printf '%s\n' '@type script' 'Pid 2 -> create (User_id 1) (Group_id 1)' \
    'Pid 2 -> mkdir "d" 0o777' 'stat "d"' >"$WORK/users.script"
  sibylfs_run "$WORK/users.script" "$WORK/root2" "$WORK/users.trace"
  expect_status run-users-script 0 $?
  expect_absent users-script-leaves-no-user '^fs_user_' /etc/passwd
  expect_absent users-script-leaves-no-group '^fs_group_' /etc/group

  # --- the account commands, one by one ------------------------------------

  groupadd fs_group_7
  expect_status groupadd 0 $?
  grep -q '^fs_group_7:' /etc/group
  expect_status groupadd-makes-the-group 0 $?
  useradd -s /bin/false fs_user_7
  expect_status useradd 0 $?
  grep -q '^fs_user_7:.*:/bin/false$' /etc/passwd
  expect_status useradd-makes-the-user-with-its-shell 0 $?
  userdel fs_user_7
  expect_status userdel 0 $?
  expect_absent userdel-removes-the-user '^fs_user_7:' /etc/passwd
  groupdel fs_group_7
  expect_status groupdel 0 $?
  expect_absent groupdel-removes-the-group '^fs_group_7:' /etc/group

  # A script that dies halfway leaves its accounts; they are forgotten.
  useradd -s /bin/false fs_user_8
  groupadd fs_group_8
  sibylfs_forget_accounts
  expect_absent forget-accounts-removes-the-users '^fs_user_' /etc/passwd
  expect_absent forget-accounts-removes-the-groups '^fs_group_' /etc/group
}

main "$@"
exit "$FAILED"
