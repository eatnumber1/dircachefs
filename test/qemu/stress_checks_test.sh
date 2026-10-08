#!/bin/bash
# Self-check of the fsstress/fsx test's checks (step 11.2b): sources the real
# guest/lib.sh and guest/stress_lib.sh (the functions guest/stress.sh calls)
# and runs them over trees and logs that are damaged on purpose. A tree that
# differs from its twin in one byte of a file, in a mode, by an extra name or
# by being empty, an fsx log that does not say A-OK (or says it for another
# number of operations) and an fsstress log with too few operations must each
# be rejected with the check's own FAIL line; identical trees and good logs
# pass.
#
# Usage: stress_checks_test.sh <lib.sh> <stress_lib.sh> <testutil>
set -euo pipefail

LIB=$(readlink -f "$1")
STRESS_LIB=$(readlink -f "$2")
TESTUTIL=$(readlink -f "$3")
WORK=$(mktemp -d)
trap 'rm -rf "${WORK}"' EXIT

fail() {
  echo "FAIL: $*" >&2
  exit 1
}

# The verdict lines ("TEST <name> PASS|FAIL ...") of the check CMD... run
# with the real libraries.
verdict() {
  TESTUTIL=${TESTUTIL} TMPDIR=${WORK} /bin/sh -c '
    FAILED=0
    . "$1" >/dev/null 2>&1 || true
    . "$2"
    shift 2
    "$@" 2>&1 | grep "^TEST "
  ' sh "${LIB}" "${STRESS_LIB}" "$@" || true
}

expect_pass() {
  local name=$1 got
  shift
  got=$(verdict "$@")
  [[ "${got}" == "TEST ${name} PASS"* ]] || fail "${name}: want PASS, got '${got}'"
  echo "PASS: ${name} accepts a good input"
}

expect_reject() {
  local name=$1 what=$2 got
  shift 2
  got=$(verdict "$@")
  [[ "${got}" == "TEST ${name} FAIL ("* ]] || fail "${what}: want ${name} FAIL, got '${got}'"
  echo "PASS: ${what} is rejected"
}

make_tree() {
  mkdir -p "$1/d/e"
  printf 'hello\n' >"$1/f"
  printf 'world\n' >"$1/d/g"
  ln -s f "$1/l"
  printf 'x' >"$1/d/e/h"
  chmod 640 "$1/d/g"
}

make_tree "${WORK}/a"
make_tree "${WORK}/b"
expect_pass same stress_same_tree same "${WORK}/a" "${WORK}/b"

# One byte of one file, the size unchanged.
make_tree "${WORK}/bytes"
printf 'hellp\n' >"${WORK}/bytes/f"
expect_reject same "a file differing in one byte" stress_same_tree same "${WORK}/a" "${WORK}/bytes"

make_tree "${WORK}/mode"
chmod 600 "${WORK}/mode/d/g"
expect_reject same "a differing mode" stress_same_tree same "${WORK}/a" "${WORK}/mode"

make_tree "${WORK}/extra"
: >"${WORK}/extra/d/e/extra"
expect_reject same "an extra name" stress_same_tree same "${WORK}/a" "${WORK}/extra"

make_tree "${WORK}/missing"
rm "${WORK}/missing/d/g"
expect_reject same "a missing name" stress_same_tree same "${WORK}/a" "${WORK}/missing"

make_tree "${WORK}/link"
ln -sf d "${WORK}/link/l"
expect_reject same "a differing symlink target" stress_same_tree same "${WORK}/a" "${WORK}/link"

mkdir "${WORK}/empty1" "${WORK}/empty2"
expect_reject same "two empty trees" stress_same_tree same "${WORK}/empty1" "${WORK}/empty2"

# fsx.
printf 'Seed set to 1\nAll 2500 operations completed A-OK!\n' >"${WORK}/fsx_good"
printf 'Seed set to 1\nmapped read: BAD DATA\nREAD BAD DATA: offset = 0x0\n' >"${WORK}/fsx_bad"
printf 'Seed set to 1\nAll 100 operations completed A-OK!\n' >"${WORK}/fsx_short"
: >"${WORK}/fsx_empty"
expect_pass fsx fsx_verdict fsx "${WORK}/fsx_good" 2500
expect_reject fsx "an fsx run that found bad data" fsx_verdict fsx "${WORK}/fsx_bad" 2500
expect_reject fsx "an fsx run of fewer operations" fsx_verdict fsx "${WORK}/fsx_short" 2500
expect_reject fsx "an empty fsx log" fsx_verdict fsx "${WORK}/fsx_empty" 2500

# fsstress.
for i in $(seq 1 100); do echo "0/${i}: write f$i 0 0 1 0"; done >"${WORK}/ss_good"
printf 'fsstress: out of memory\n' >"${WORK}/ss_bad"
expect_pass ss fsstress_verdict ss "${WORK}/ss_good" 80
expect_reject ss "an fsstress log with too few operations" fsstress_verdict ss "${WORK}/ss_good" 101
expect_reject ss "an fsstress log with no operations" fsstress_verdict ss "${WORK}/ss_bad" 1

# The summary counts per operation.
got=$(/bin/sh -c '. "$1"; fsstress_ops "$2"' sh "${STRESS_LIB}" "${WORK}/ss_good" | tr '\n' ' ')
[[ "${got}" == "total 100 write 100 " ]] || fail "fsstress_ops: got '${got}'"
echo "PASS: fsstress_ops counts operations"
