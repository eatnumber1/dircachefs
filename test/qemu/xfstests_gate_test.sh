#!/bin/bash
# Self-check of the xfstests gate (step 17.1): sources the real
# guest/lib.sh and guest/xfstests_lib.sh (the functions guest/xfstests.sh
# calls) and feeds them canned results and lists. The gate must pass a run
# whose failures and "not run" tests are exactly the listed ones, and must
# reject each of: a failure that is not listed, a listed test that passes, does
# not run or times out, a timeout that is not listed, a "not run" test that is
# not on the notrun list or whose reason changed, a listed "not run" test that
# runs, a test of the shard with no result, an empty run, a run in which fewer
# than an eighth of the tests ran (the boundary is pinned: one of ten is
# rejected, two of ten accepted), and lists that name a test twice, a test
# that is not in the group list or not run by the set, or a test in two lists.
#
# The functions run under the guest's busybox ash and gawk (Alpine's, as the
# guest's `awk` is), not the host's shell and tools.
#
# Usage: xfstests_gate_test.sh <lib.sh> <xfstests_lib.sh> <busybox> <gawk>
set -euo pipefail

LIB=$(readlink -f "$1")
XLIB=$(readlink -f "$2")
BUSYBOX=$(readlink -f "$3")
GAWK=$(readlink -f "$4")
WORK=$(mktemp -d)
trap 'rm -rf "${WORK}"' EXIT

fail() {
  echo "FAIL: $*" >&2
  exit 1
}

# The guest's tools, and only they, for the functions under test.
mkdir "${WORK}/bin"
cp "${BUSYBOX}" "${WORK}/bin/busybox"
for applet in basename cat cut dirname grep head readlink sed sh sort tr wc; do
  ln -s busybox "${WORK}/bin/${applet}"
done
# (A script, not a link: the wrapper finds Alpine's tree from its own path.)
printf '#!%s/bin/sh\nexec %s "$@"\n' "${WORK}" "${GAWK}" >"${WORK}/bin/awk"
chmod +x "${WORK}/bin/awk"

# run_fn FUNCTION ARGS...: the function's output and "rc=<status>".
run_fn() {
  PATH=${WORK}/bin "${WORK}/bin/sh" -c '
    FAILED=0
    . "$1" >/dev/null 2>&1 || true
    . "$2"
    FAILED=0
    shift 2
    rc=0
    "$@" || rc=$?
    echo "rc=${rc}"
  ' sh "${LIB}" "${XLIB}" "$@"
}

# gate RESULTS EXPECTED SHARD NOTRUN
gate() { run_fn xfstests_gate "$@"; }

# lists_valid EXPECTED EXCLUDED GROUPLIST NOTRUN SELECTED
lists_valid() { run_fn xfstests_lists_valid "$@"; }

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
cat >"${WORK}/notrun" <<'EOS'
# the reasons xfstests gave
generic/002 Reflink not supported by scratch filesystem type: fuse
generic/009 dmsetup utility required, skipped this test
generic/091 not in this shard either
EOS
cat >"${WORK}/good" <<'EOS'
generic/001 pass 3
generic/002 notrun 1 Reflink not supported by scratch filesystem type: fuse
generic/003 fail 4
generic/004 fail 5
generic/005 pass 3
generic/006 pass 3
generic/007 pass 3
generic/008 pass 3
generic/009 notrun 1 dmsetup utility required, skipped this test
generic/010 pass 3
EOS

got=$(gate "${WORK}/good" "${WORK}/expected" "${WORK}/shard" "${WORK}/notrun")
[[ "${got}" == *"TEST xfstests-expected-failures PASS"*$'\nrc=0' ]] || fail "healthy run: got '${got}'"
echo "PASS: a run failing and not running exactly the listed tests is accepted"

expect_reject() {
  local name=$1 want=$2 results=$3 got
  got=$(gate "${results}" "${WORK}/expected" "${WORK}/shard" "${WORK}/notrun")
  [[ "${got}" == *"TEST xfstests-expected-failures FAIL ("*"${want}"*$'\nrc=1' ]] ||
    fail "${name}: got '${got}'"
  echo "PASS: ${name} is rejected"
}

# variant NAME SED-ARGS...: the good results edited into ${WORK}/NAME.
variant() {
  local name=$1
  shift
  sed "$@" "${WORK}/good" >"${WORK}/${name}"
}

variant unlisted -e 's|^generic/006 pass|generic/006 fail|'
expect_reject "an unlisted failure" "unexpected failure: generic/006 (fail)" "${WORK}/unlisted"

variant fixed -e 's|^generic/003 fail|generic/003 pass|'
expect_reject "a listed test that passes" "listed as failing but passed: generic/003" "${WORK}/fixed"

variant notrun_listed -e 's|^generic/004 fail 5|generic/004 notrun 1 some reason|'
expect_reject "a listed test that did not run" "listed as failing but did not run: generic/004" "${WORK}/notrun_listed"

variant timed_out_listed -e 's|^generic/004 fail 5|generic/004 timeout 300|'
expect_reject "a listed test that times out" "listed test timed out: generic/004" "${WORK}/timed_out_listed"

variant timed_out -e 's|^generic/006 pass|generic/006 timeout|'
expect_reject "an unlisted timeout" "unexpected failure: generic/006 (timeout)" "${WORK}/timed_out"

grep -v '^generic/007 ' "${WORK}/good" >"${WORK}/missing"
expect_reject "a test with no result" "no result: generic/007" "${WORK}/missing"

# A test that passed and is now "not run": the probe that broke was xfstests'.
variant slipped -e 's|^generic/005 pass 3|generic/005 notrun 1 xfs_io fallocate  failed (old kernel/wrong fs?)|'
expect_reject "a test that slipped into not run" "unexpected not run: generic/005 (xfs_io fallocate failed (old kernel/wrong fs?))" "${WORK}/slipped"

variant reason -e 's|^generic/002 notrun 1 .*|generic/002 notrun 1 O_DIRECT is not supported|'
expect_reject "a not-run reason that changed" 'not-run reason changed: generic/002 (was "Reflink not supported by scratch filesystem type: fuse", now "O_DIRECT is not supported")' "${WORK}/reason"

variant ran -e 's|^generic/002 notrun 1 .*|generic/002 pass 2|'
expect_reject "a listed not-run test that runs" "listed as not run but ran: generic/002 (pass)" "${WORK}/ran"

# The floor: one of ten ran is rejected, two of ten accepted. (The two that run
# are the listed failures, so nothing else is wrong with the run.)
cat >"${WORK}/shard1" <<'EOS'
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
: >"${WORK}/notrun_none"
for i in 001 002 003 004 005 006 007 008 009 010; do
  printf 'generic/%s notrun 1 not supported\n' "${i}"
done >"${WORK}/floor_results"
: >"${WORK}/expected_none"
for i in 002 003 004 005 006 007 008 009 010; do
  printf 'generic/%s not supported\n' "${i}"
done >"${WORK}/notrun_floor"
# One test runs and passes (generic/001), nine are not run as listed.
sed -i 's|^generic/001 notrun 1 not supported|generic/001 pass 3|' "${WORK}/floor_results"
got=$(gate "${WORK}/floor_results" "${WORK}/expected_none" "${WORK}/shard1" "${WORK}/notrun_floor")
[[ "${got}" == *"TEST xfstests-expected-failures FAIL ("*"only 1 of 10 tests ran"*$'\nrc=1' ]] ||
  fail "one of ten ran: got '${got}'"
echo "PASS: one test of ten running is rejected"
sed -i 's|^generic/002 notrun 1 not supported|generic/002 pass 3|' "${WORK}/floor_results"
grep -v '^generic/002 ' "${WORK}/notrun_floor" >"${WORK}/notrun_floor2"
got=$(gate "${WORK}/floor_results" "${WORK}/expected_none" "${WORK}/shard1" "${WORK}/notrun_floor2")
[[ "${got}" == *"TEST xfstests-expected-failures PASS"*$'\nrc=0' ]] ||
  fail "two of ten ran: got '${got}'"
echo "PASS: two tests of ten running is accepted"

# Nothing ran: a broken runtime reports everything as not run.
for i in 001 002 003 004 005 006 007 008 009 010; do
  printf 'generic/%s notrun 1 not supported\n' "${i}"
done >"${WORK}/allnotrun"
sed 's|^\(generic/[0-9]*\) .*|\1 not supported|' "${WORK}/allnotrun" >"${WORK}/notrun_all"
got=$(gate "${WORK}/allnotrun" "${WORK}/expected_none" "${WORK}/shard1" "${WORK}/notrun_all")
[[ "${got}" == *"only 0 of 10 tests ran"*$'\nrc=1' ]] || fail "nothing ran: got '${got}'"
echo "PASS: a run where nothing ran is rejected"

: >"${WORK}/empty"
expect_reject "an empty run" "no results" "${WORK}/empty"

# The reason of a not-run test as the results keep it.
got=$(PATH=${WORK}/bin "${WORK}/bin/sh" -c '. "$1"; xfstests_reason "$2"' sh "${XLIB}" \
  "$(printf 'This test requires at least 5GB free on /mnt/scratch to run\nsecond line')")
[[ "${got}" == "This test requires at least NGB free on /mnt/scratch to run" ]] ||
  fail "xfstests_reason: got '${got}'"
long=$(printf 'x%.0s' {1..150})
got=$(PATH=${WORK}/bin "${WORK}/bin/sh" -c '. "$1"; xfstests_reason "$2"' sh "${XLIB}" "${long}")
[[ "${#got}" -eq 100 ]] || fail "xfstests_reason does not cut at 100: ${#got}"
echo "PASS: reasons are normalized and cut"

# The lists themselves.
printf '001 a\n002 b\n003 c\n090 d\n091 e\n' >"${WORK}/groups_ok"
printf 'generic/001\ngeneric/002\ngeneric/003\n' >"${WORK}/selected"
printf 'generic/001 a\n' >"${WORK}/exp_ok"
printf 'generic/002 not supported\n' >"${WORK}/nr_ok"
printf 'generic/090 slow\n' >"${WORK}/exc_ok"
valid() { lists_valid "$1" "$2" "${WORK}/groups_ok" "$3" "${WORK}/selected"; }
got=$(valid "${WORK}/exp_ok" "${WORK}/exc_ok" "${WORK}/nr_ok")
[[ "${got}" == *"TEST xfstests-lists PASS"*$'\nrc=0' ]] || fail "valid lists: got '${got}'"
echo "PASS: valid lists are accepted"

expect_bad_lists() {
  local name=$1 want=$2 expected=$3 excluded=$4 notrun=$5 got
  got=$(valid "${expected}" "${excluded}" "${notrun}")
  [[ "${got}" == *"TEST xfstests-lists FAIL ("*"${want}"*$'\nrc=1' ]] || fail "${name}: got '${got}'"
  echo "PASS: ${name} is rejected"
}
printf 'generic/001 a\ngeneric/001 b\n' >"${WORK}/exp_dup"
expect_bad_lists "a test listed twice" "listed twice: generic/001" "${WORK}/exp_dup" "${WORK}/exc_ok" "${WORK}/nr_ok"
printf 'generic/777 a\n' >"${WORK}/exp_unknown"
expect_bad_lists "a test that does not exist" "not a test in the group list: generic/777" "${WORK}/exp_unknown" "${WORK}/exc_ok" "${WORK}/nr_ok"
printf 'generic/090\n' >"${WORK}/exc_noreason"
expect_bad_lists "an exclusion with no reason" "no reason: generic/090" "${WORK}/exp_ok" "${WORK}/exc_noreason" "${WORK}/nr_ok"
printf 'generic/001 x\n' >"${WORK}/exc_both"
expect_bad_lists "a test both expected to fail and excluded" "both expected to fail and excluded: generic/001" "${WORK}/exp_ok" "${WORK}/exc_both" "${WORK}/nr_ok"
printf 'generic/001\n' >"${WORK}/exp_noreason"
expect_bad_lists "an expected failure with no reason" "no reason: generic/001" "${WORK}/exp_noreason" "${WORK}/exc_ok" "${WORK}/nr_ok"
printf 'generic/002\n' >"${WORK}/nr_noreason"
expect_bad_lists "a not-run entry with no reason" "no reason: generic/002" "${WORK}/exp_ok" "${WORK}/exc_ok" "${WORK}/nr_noreason"
printf 'generic/001 not supported\n' >"${WORK}/nr_both"
expect_bad_lists "a test both expected to fail and not run" "both expected to fail and not run: generic/001" "${WORK}/exp_ok" "${WORK}/exc_ok" "${WORK}/nr_both"
printf 'generic/090 not supported\n' >"${WORK}/nr_excluded"
expect_bad_lists "a test both not run and excluded" "both not run and excluded: generic/090" "${WORK}/exp_ok" "${WORK}/exc_ok" "${WORK}/nr_excluded"
printf 'generic/091 not supported\n' >"${WORK}/nr_stale"
expect_bad_lists "a not-run entry for a test this set does not run" "listed as not run but not run by this set: generic/091" "${WORK}/exp_ok" "${WORK}/exc_ok" "${WORK}/nr_stale"
printf 'generic/091 a\n' >"${WORK}/exp_stale"
expect_bad_lists "an expected failure for a test this set does not run" "expected to fail but not run by this set: generic/091" "${WORK}/exp_stale" "${WORK}/exc_ok" "${WORK}/nr_ok"

echo "xfstests_gate_test: all checks passed"
