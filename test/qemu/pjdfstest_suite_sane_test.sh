#!/bin/bash
# Self-check of the pjdfstest wrapper's pjdfstest-suite-sane gate (step 26.1):
# sources the real guest/lib.sh and guest/pjdfstest_lib.sh (the functions
# guest/pjdfstest.sh calls) and feeds them canned TAP output through the real
# tap_results and results files through the real pjdfstest_suite_sane. Each
# of a sabotaged backing baseline (over 5% failing on the raw filesystem),
# an empty run, garbled output (the busybox `tail -1` lesson) and a run whose
# two sides ran different numbers of checks must be rejected with the gate's
# own FAIL line; a healthy run passes.
#
# Usage: pjdfstest_suite_sane_test.sh <lib.sh> <pjdfstest_lib.sh>
set -euo pipefail

LIB=$(readlink -f "$1")
PJD_LIB=$(readlink -f "$2")
WORK=$(mktemp -d)
trap 'rm -rf "${WORK}"' EXIT

fail() {
  echo "FAIL: $*" >&2
  exit 1
}

# The gate's verdict line for DCFS_TAP and BACKING_TAP (files of raw TAP
# output of one .t file each), after the real tap_results parsed them.
verdict() {
  /bin/sh -c '
    FAILED=0
    . "$1" >/dev/null 2>&1 || true
    . "$2"
    FAILED=0
    tap_results chmod/00.t <"$3" >"$4/dcfs_results.txt"
    tap_results chmod/00.t <"$5" >"$4/backing_results.txt"
    rc=0
    pjdfstest_suite_sane ext4 "$4/dcfs_results.txt" "$4/backing_results.txt" || rc=$?
    echo "rc=${rc}"
  ' sh "${LIB}" "${PJD_LIB}" "$1" "${WORK}" "$2"
}

# n lines of TAP with the first bad ones "not ok".
tap() {
  local total=$1 bad=$2 i
  echo "1..${total}"
  for ((i = 1; i <= total; i++)); do
    if ((i <= bad)); then echo "not ok ${i}"; else echo "ok ${i}"; fi
  done
}

tap 100 0 >"${WORK}/good"
tap 100 1 >"${WORK}/few_bad"
tap 100 60 >"${WORK}/mostly_bad"
tap 100 5 >"${WORK}/five_bad"
: >"${WORK}/empty"
# What a busybox without `tail -1` produced: no result line is parsed as ok.
printf 'tail: invalid option -- 1\nnot ok\nnot ok\ngarbage 7\n' >"${WORK}/garbled"
tap 90 0 >"${WORK}/short"

expect_reject() {
  local name=$1 got
  got=$(verdict "$2" "$3")
  [[ "${got}" == "TEST pjdfstest-suite-sane FAIL ("*"the pjdfstest tooling in the guest is broken"*$'\nrc=1' ]] ||
    fail "${name}: got '${got}'"
  echo "PASS: ${name} is rejected"
}

got=$(verdict "${WORK}/good" "${WORK}/few_bad")
[[ "${got}" == $'TEST pjdfstest-suite-sane PASS\nrc=0' ]] || fail "healthy run: got '${got}'"
echo "PASS: healthy run is accepted"

# The raw side failing 60 of 100 checks (a sabotaged baseline).
got=$(verdict "${WORK}/mostly_bad" "${WORK}/mostly_bad")
[[ "${got}" == "TEST pjdfstest-suite-sane FAIL (60 of 100 checks failed directly on ext4 (100 ran through dcfs); "*$'\nrc=1' ]] ||
  fail "sabotaged baseline: got '${got}'"
echo "PASS: sabotaged baseline is rejected"
expect_reject "5% failing on the raw filesystem" "${WORK}/good" "${WORK}/five_bad"
expect_reject "empty prove output" "${WORK}/empty" "${WORK}/empty"
expect_reject "garbled prove output" "${WORK}/garbled" "${WORK}/garbled"
expect_reject "differing check counts" "${WORK}/short" "${WORK}/good"
