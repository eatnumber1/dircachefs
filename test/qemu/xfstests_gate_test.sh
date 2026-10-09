#!/bin/bash
# Self-check of the xfstests gate (step 17.1): sources the real
# guest/lib.sh and guest/xfstests_lib.sh (the functions guest/xfstests.sh
# calls) and feeds them canned results and lists. The gate must pass a run
# whose failures are exactly the listed ones, and must reject each of: a
# failure that is not listed, a listed test that passes, a listed test that
# did not run at all, a test of the shard with no result, a run in which
# fewer than an eighth ran, and lists that name a test twice, a test that is not in
# the group list, or a test as both expected to fail and excluded.
#
# Usage: xfstests_gate_test.sh <lib.sh> <xfstests_lib.sh>
set -euo pipefail

LIB=$(readlink -f "$1")
XLIB=$(readlink -f "$2")
WORK=$(mktemp -d)
trap 'rm -rf "${WORK}"' EXIT

fail() {
  echo "FAIL: $*" >&2
  exit 1
}

# gate RESULTS EXPECTED SHARD: the gate's output and "rc=<status>".
gate() {
  /bin/sh -c '
    FAILED=0
    . "$1" >/dev/null 2>&1 || true
    . "$2"
    FAILED=0
    rc=0
    xfstests_gate "$3" "$4" "$5" || rc=$?
    echo "rc=${rc}"
  ' sh "${LIB}" "${XLIB}" "$1" "$2" "$3"
}

# lists_valid EXPECTED EXCLUDED GROUPLIST
lists_valid() {
  /bin/sh -c '
    FAILED=0
    . "$1" >/dev/null 2>&1 || true
    . "$2"
    FAILED=0
    rc=0
    xfstests_lists_valid "$3" "$4" "$5" || rc=$?
    echo "rc=${rc}"
  ' sh "${LIB}" "${XLIB}" "$1" "$2" "$3"
}

cat >"${WORK}/shard" <<'EOS'
generic/001
generic/002
generic/003
generic/004
generic/005
generic/006
generic/007
generic/008
generic/009
generic/010
EOS
cat >"${WORK}/expected" <<'EOS'
# a comment, then a blank line

generic/003 the reason for 003
generic/004   another reason, with  spaces
generic/090 not in this shard, so not looked at
EOS
cat >"${WORK}/good" <<'EOS'
generic/001 pass 3
generic/002 notrun 1
generic/003 fail 4
generic/004 timeout 300
generic/005 pass 3
generic/006 pass 3
generic/007 pass 3
generic/008 pass 3
generic/009 notrun 1
generic/010 pass 3
EOS

got=$(gate "${WORK}/good" "${WORK}/expected" "${WORK}/shard")
[[ "${got}" == *"TEST xfstests-expected-failures PASS"*$'\nrc=0' ]] || fail "healthy run: got '${got}'"
echo "PASS: a run failing exactly the listed tests is accepted"

expect_reject() {
  local name=$1 want=$2 results=$3 expected=$4 got
  got=$(gate "${results}" "${expected}" "${WORK}/shard")
  [[ "${got}" == *"TEST xfstests-expected-failures FAIL ("*"${want}"*$'\nrc=1' ]] ||
    fail "${name}: got '${got}'"
  echo "PASS: ${name} is rejected"
}

# A failure that is not listed.
sed 's|^generic/006 pass|generic/006 fail|' "${WORK}/good" >"${WORK}/unlisted"
expect_reject "an unlisted failure" "unexpected failure: generic/006" "${WORK}/unlisted" "${WORK}/expected"

# A listed test that passes (the list must stay honest).
sed 's|^generic/003 fail|generic/003 pass|' "${WORK}/good" >"${WORK}/fixed"
expect_reject "a listed test that passes" "listed as failing but passed: generic/003" "${WORK}/fixed" "${WORK}/expected"

# A listed test that was not run at all.
sed 's|^generic/004 timeout 300|generic/004 notrun 1|' "${WORK}/good" >"${WORK}/notrun"
expect_reject "a listed test that did not run" "listed as failing but did not run: generic/004" "${WORK}/notrun" "${WORK}/expected"

# A test of the shard with no result (the guest died, a wrapper skipped it).
grep -v '^generic/007 ' "${WORK}/good" >"${WORK}/missing"
expect_reject "a test with no result" "no result: generic/007" "${WORK}/missing" "${WORK}/expected"

# Almost nothing ran: a broken runtime reports notrun for everything.
sed -e 's| pass | notrun |' -e 's| fail | notrun |' -e 's| timeout | notrun |' "${WORK}/good" >"${WORK}/allnotrun"
expect_reject "a run where almost nothing ran" "only 0 of 10 tests ran" "${WORK}/allnotrun" "${WORK}/expected"
# One in five ran (as in a healthy run, most tests are "not run" for what FUSE
# cannot do): accepted.
cat >"${WORK}/fifth" <<'EOS'
generic/001 pass 3
generic/002 notrun 1
generic/003 fail 4
generic/004 timeout 300
generic/005 notrun 3
generic/006 notrun 3
generic/007 notrun 3
generic/008 notrun 3
generic/009 notrun 1
generic/010 notrun 3
EOS
got=$(gate "${WORK}/fifth" "${WORK}/expected" "${WORK}/shard")
[[ "${got}" == *"TEST xfstests-expected-failures PASS"*$'\nrc=0' ]] || fail "a run where one in five tests ran: got '${got}'"
echo "PASS: a run where one in five tests ran is accepted"

# An empty results file.
: >"${WORK}/empty"
expect_reject "an empty run" "no results" "${WORK}/empty" "${WORK}/expected"

# The lists themselves.
printf 'generic/001\ngeneric/003\ngeneric/090\n' >"${WORK}/groups_ok"
printf 'generic/001 a\ngeneric/003 b\n' >"${WORK}/exp_ok"
printf 'generic/090 c\n' >"${WORK}/exc_ok"
got=$(lists_valid "${WORK}/exp_ok" "${WORK}/exc_ok" "${WORK}/groups_ok")
[[ "${got}" == *"TEST xfstests-lists PASS"*$'\nrc=0' ]] || fail "valid lists: got '${got}'"
echo "PASS: valid lists are accepted"

expect_bad_lists() {
  local name=$1 want=$2 expected=$3 excluded=$4 got
  got=$(lists_valid "${expected}" "${excluded}" "${WORK}/groups_ok")
  [[ "${got}" == *"TEST xfstests-lists FAIL ("*"${want}"*$'\nrc=1' ]] || fail "${name}: got '${got}'"
  echo "PASS: ${name} is rejected"
}
printf 'generic/001 a\ngeneric/001 b\n' >"${WORK}/exp_dup"
expect_bad_lists "a test listed twice" "listed twice: generic/001" "${WORK}/exp_dup" "${WORK}/exc_ok"
printf 'generic/777 a\n' >"${WORK}/exp_unknown"
expect_bad_lists "a test that does not exist" "not a test in the group list: generic/777" "${WORK}/exp_unknown" "${WORK}/exc_ok"
printf 'generic/001\n' >"${WORK}/exc_noreason"
expect_bad_lists "an exclusion with no reason" "no reason: generic/001" "${WORK}/exp_ok" "${WORK}/exc_noreason"
printf 'generic/003 x\n' >"${WORK}/exc_both"
expect_bad_lists "a test both expected to fail and excluded" "both expected to fail and excluded: generic/003" "${WORK}/exp_ok" "${WORK}/exc_both"
printf 'generic/003\n' >"${WORK}/exp_noreason"
expect_bad_lists "an expected failure with no reason" "no reason: generic/003" "${WORK}/exp_noreason" "${WORK}/exc_ok"

echo "xfstests_gate_test: all checks passed"
