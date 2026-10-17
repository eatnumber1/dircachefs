#!/bin/bash
# SPDX-License-Identifier: MIT
# Self-check of the SibylFS differential test's own logic (step 12.18b): the
# real guest/lib.sh and guest/sibylfs_lib.sh functions that guest/sibylfs.sh
# calls, over canned input, under the guest's busybox ash and awk (Alpine's,
# not the host's shell and tools). Each of these must hold, and fail when its
# code is broken:
#
# - a script selection that is empty, misspelled, has an empty part, or names
#   a shard that does not exist is refused;
# - the script chooser picks the hand-written scripts and shard K of N of the
#   generated ones from the member names of the suite's tarball, and leaves
#   out the checker-only scripts and the .tmpl copies;
# - the regressions are the scripts the model allows on the backing file system
#   and rejects (or cannot judge) through dcfs;
# - sibylfs_suite_sane passes a healthy run and fails an empty one, a run with
#   a trace without a verdict on either side, and one in which the model
#   allows less than half of the backing file system's traces (exactly half
#   passes);
# - no_out_of_band fails on a dcfs log with an out-of-band warning.
#
# What needs the real tools (the wrapper's exit statuses, a real run) is
# guest/sibylfs_selftest.sh, in a guest.
#
# Usage: sibylfs_lib_test.sh <lib.sh> <sibylfs_lib.sh> <busybox>
set -euo pipefail

LIB=$(readlink -f "$1")
SLIB=$(readlink -f "$2")
BUSYBOX=$(readlink -f "$3")
WORK=$(mktemp -d)
trap 'rm -rf "${WORK}"' EXIT

fail() {
  echo "FAIL: $*" >&2
  exit 1
}

# The guest's tools, and only they, for the functions under test.
mkdir "${WORK}/bin"
cp "${BUSYBOX}" "${WORK}/bin/busybox"
for applet in awk basename cat cut dirname grep head sed sh sort tr uniq wc; do
  ln -s busybox "${WORK}/bin/${applet}"
done

# run_fn FUNCTION ARGS...: the function's stdout, then "rc=<status>". stdin is
# the caller's.
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
  ' sh "${LIB}" "${SLIB}" "$@"
}

# expect NAME GOT WANT: GOT (a function's output) ends with WANT.
expect() {
  [[ "$2" == *"$3" ]] || fail "$1: got '$2', wanted '...$3'"
  echo "PASS: $1"
}

# --- sibylfs_spec_valid ----------------------------------------------------

for spec in adhoc 0/16 15/16 adhoc,3/16 0/16,1/16,adhoc 0/1 07/10; do
  got=$(run_fn sibylfs_spec_valid "${spec}")
  expect "the selection '${spec}' is accepted" "${got}" "rc=0"
done
for spec in "" adhoc_ all 16/16 0/0 1/0 a/b 1/ /3 -1/3 1/2/3 adhoc,16/16 \
  adhoc,,0/16 ,adhoc adhoc, "adhoc 0/16" 0/16x; do
  got=$(run_fn sibylfs_spec_valid "${spec}")
  expect "the selection '${spec}' is refused" "${got}" "rc=1"
done

# --- sibylfs_scripts -------------------------------------------------------

T=sibylfs_test_suite-0123/test-suite
cat >"${WORK}/members" <<EOS
sibylfs_test_suite-0123/
${T}/
${T}/README.md
${T}/open/
${T}/open/g0-int.trace
${T}/open/g1-int.trace
${T}/open/g2-int.trace
${T}/open/g3-int.trace
${T}/open/g4-int.trace
${T}/open/g5-int.trace
${T}/open/adhoc_a-int.trace
${T}/open/adhoc_b-int.trace
${T}/adhoc_c-int.trace
${T}/open/g6-check-int.trace
${T}/open.tmpl/adhoc_a-int.trace
${T}/open.tmpl/readme.txt
${T}/open/readme.txt
EOS
chosen() { run_fn sibylfs_scripts "$1" <"${WORK}/members" |
  sed "s|^${T}/||" | tr '\n' ' '; }
expect "adhoc selects the hand-written scripts, once each" \
  "$(chosen adhoc)" "adhoc_c-int.trace open/adhoc_a-int.trace \
open/adhoc_b-int.trace rc=0 "
expect "shard 1 of 3" "$(chosen 1/3)" \
  "open/g1-int.trace open/g4-int.trace rc=0 "
expect "shard 0 of 3" "$(chosen 0/3)" \
  "open/g0-int.trace open/g3-int.trace rc=0 "
expect "the shards together are every generated script" \
  "$(chosen 0/3,1/3,2/3)" "open/g0-int.trace open/g1-int.trace \
open/g2-int.trace open/g3-int.trace open/g4-int.trace open/g5-int.trace rc=0 "
expect "adhoc and a shard" "$(chosen adhoc,2/3)" "adhoc_c-int.trace \
open/adhoc_a-int.trace open/adhoc_b-int.trace open/g2-int.trace \
open/g5-int.trace rc=0 "

# --- sibylfs_regressions, sibylfs_in, sibylfs_not_in -------------------------

printf '%s\n' 'a 0 0' 'b 1 0' 'c 1 1' 'd 2 0' 'e 0 1' 'f 2 2' \
  >"${WORK}/verdicts"
got=$(run_fn sibylfs_regressions "${WORK}/verdicts" | tr '\n' ' ')
expect "the regressions are rejected through dcfs, allowed on the backing" \
  "${got}" "b d rc=0 "

printf '%s\n' a b c >"${WORK}/file"
printf '%s\n' b c d >"${WORK}/list"
: >"${WORK}/nothing"
expect "in keeps the listed" \
  "$(run_fn sibylfs_in "${WORK}/list" "${WORK}/file" | tr '\n' ' ')" "b c rc=0 "
expect "not-in keeps the unlisted" \
  "$(run_fn sibylfs_not_in "${WORK}/list" "${WORK}/file" | tr '\n' ' ')" \
  "a rc=0 "
expect "not-in a list that does not exist keeps all" \
  "$(run_fn sibylfs_not_in "${WORK}/none" "${WORK}/file" | tr '\n' ' ')" \
  "a b c rc=0 "
expect "not-in an empty list keeps all" \
  "$(run_fn sibylfs_not_in "${WORK}/nothing" "${WORK}/file" | tr '\n' ' ')" \
  "a b c rc=0 "

# --- sibylfs_suite_sane ------------------------------------------------------

# sane NAME WANT_RC FILE: the gate's status on the verdicts in FILE.
sane() {
  local got
  got=$(run_fn sibylfs_suite_sane ext4 "$3")
  expect "$1" "${got}" "rc=$2"
  if [[ "$2" == 1 ]]; then
    [[ "${got}" == *"TEST sibylfs-suite-sane FAIL ("* ]] ||
      fail "$1: no FAIL line in '${got}'"
  else
    [[ "${got}" == *"TEST sibylfs-suite-sane PASS"* ]] ||
      fail "$1: no PASS line in '${got}'"
  fi
}
lines() { awk -v n="$1" -v d="$2" -v b="$3" -v p="$4" \
  'BEGIN { for (i = 0; i < n; i++) print p i, d, b }'; }
: >"${WORK}/empty"
sane "an empty run fails" 1 "${WORK}/empty"
lines 20 0 0 s >"${WORK}/all"
sane "a run the model allows throughout passes" 0 "${WORK}/all"
{ lines 20 0 0 s; echo "t 0 2"; } >"${WORK}/backing_unjudged"
sane "a trace without a verdict on the backing file system fails" 1 \
  "${WORK}/backing_unjudged"
{ lines 20 0 0 s; echo "t 2 0"; } >"${WORK}/dcfs_unjudged"
sane "a trace without a verdict through dcfs fails" 1 "${WORK}/dcfs_unjudged"
{ lines 10 1 1 s; lines 10 0 0 t; } >"${WORK}/half"
sane "exactly half of the backing file system's traces allowed passes" 0 \
  "${WORK}/half"
{ lines 11 1 1 s; lines 9 0 0 t; } >"${WORK}/under_half"
sane "under half allowed fails" 1 "${WORK}/under_half"

# --- no_out_of_band ----------------------------------------------------------

printf '%s\n' 'W1017 main.cc:351] a warning' >"${WORK}/clean.log"
got=$(run_fn no_out_of_band some-check "${WORK}/clean.log")
expect "a log without out-of-band warnings passes" "${got}" \
  $'TEST some-check PASS\nrc=0'
printf '%s\n' 'W1017 main.cc:351] a warning' \
  'W1017 backing.cc:9] out-of-band change of x' >"${WORK}/oob.log"
got=$(run_fn no_out_of_band some-check "${WORK}/oob.log")
[[ "${got}" == *"out-of-band warning count: 1"* &&
  "${got}" == *"TEST some-check FAIL ("*$'\nrc=0' ]] ||
  fail "a log with an out-of-band warning: got '${got}'"
echo "PASS: a log with an out-of-band warning fails the check"
got=$(run_fn no_out_of_band some-check "${WORK}/missing.log")
expect "a log that does not exist counts none" "${got}" \
  $'TEST some-check PASS\nrc=0'
