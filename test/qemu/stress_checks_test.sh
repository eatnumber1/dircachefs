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
# The functions run under the guest's busybox (applets first on PATH), not the
# host's shell and tools.
#
# Usage: stress_checks_test.sh <lib.sh> <stress_lib.sh> <testutil> <busybox>
set -euo pipefail

LIB=$(readlink -f "$1")
STRESS_LIB=$(readlink -f "$2")
TESTUTIL=$(readlink -f "$3")
BUSYBOX=$(readlink -f "$4")
WORK=$(mktemp -d)
trap 'rm -rf "${WORK}"' EXIT

fail() {
  echo "FAIL: $*" >&2
  exit 1
}

# The guest's applets, and only they, for the functions under test.
mkdir "${WORK}/bin"
cp "${BUSYBOX}" "${WORK}/bin/busybox"
for applet in awk basename cat cut diff find grep head md5sum sed sh sort tail tr uniq wc; do
  ln -s busybox "${WORK}/bin/${applet}"
done
# Two trees made here differ in ctime (the time they were made), which no
# test can set, so `stat` answers the format stress_digest asks for with the
# mtime in both places; the fixtures set the mtimes. (A ctime difference is
# found by the same line as an mtime one; the guest run compares real ctimes.)
cat >"${WORK}/bin/stat" <<STAT
#!${WORK}/bin/sh
[ "\$1 \$2" = "-c %n mtime=%Y ctime=%Z" ] || { echo "stat: unexpected arguments: \$*" >&2; exit 1; }
exec ${WORK}/bin/busybox stat -c '%n mtime=%Y ctime=%Y' "\$3"
STAT
chmod +x "${WORK}/bin/stat"

# The verdict lines ("TEST <name> PASS|FAIL ...") of the check CMD... run
# with the real libraries.
verdict() {
  TESTUTIL=${TESTUTIL} TMPDIR=${WORK} PATH=${WORK}/bin "${WORK}/bin/sh" -c '
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
  find "$1" -exec touch -h -d @1700000000 {} +
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

make_tree "${WORK}/mtime"
touch -d @1700000001 "${WORK}/mtime/d/g"
expect_reject same "a differing mtime" stress_same_tree same "${WORK}/a" "${WORK}/mtime"

make_tree "${WORK}/xattr"
"${TESTUTIL}" setxattr "${WORK}/xattr/f" user.k v ||
  fail "this test's temporary directory does not take user xattrs: the xattr fixture cannot run"
find "${WORK}/xattr" -exec touch -h -d @1700000000 {} +
expect_reject same "a file with an extra xattr" stress_same_tree same "${WORK}/a" "${WORK}/xattr"

make_tree "${WORK}/fifo"
rm "${WORK}/fifo/d/e/h"
mkfifo "${WORK}/fifo/d/e/h"
find "${WORK}/fifo" -exec touch -h -d @1700000000 {} +
expect_reject same "a fifo where a file was" stress_same_tree same "${WORK}/a" "${WORK}/fifo"

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
for i in $(seq 1 100); do
  for op in creat mkdir link symlink rename unlink write; do echo "0/${i}: ${op} f${i} 0"; done
done >"${WORK}/ss_good"
printf 'fsstress: out of memory\n' >"${WORK}/ss_bad"
expect_pass ss fsstress_verdict ss "${WORK}/ss_good" 80
expect_reject ss "an fsstress log with too few operations" fsstress_verdict ss "${WORK}/ss_good" 101
expect_reject ss "an fsstress log with no operations" fsstress_verdict ss "${WORK}/ss_bad" 1

# fsstress: a log of operations that all failed (ENOENT), the shape of a run
# whose operations never worked, and one with an EIO among good ones.
for i in $(seq 1 100); do
  for op in creat mkdir link symlink rename unlink write; do
    case "${op}" in
      creat) echo "0/${i}: creat f${i} x:0 2 0" ;;
      *) echo "0/${i}: ${op} f${i} 2" ;;
    esac
  done
done >"${WORK}/ss_allfail"
expect_reject ss "an fsstress log where every operation failed" fsstress_verdict ss "${WORK}/ss_allfail" 1
{
  cat "${WORK}/ss_good"
  for op in creat mkdir link symlink rename unlink; do echo "0/200: ${op} f 0"; done
  echo "0/201: write f1 [0,5] 5"
} >"${WORK}/ss_eio"
expect_reject ss "an fsstress log with an EIO" fsstress_verdict ss "${WORK}/ss_eio" 1

# Every kind of line fsstress -v prints is classified as a result, a failure
# or left out.
cat >"${WORK}/ss_mixed" <<'LOG'
1/0: writev - no filename
1/4: mknod c0 0
1/4: mknod add id=0,parent=-1
1/9: creat f1 x:0 0 0
1/9: creat add id=1,parent=-1
1/10: creat f2 x:0 17 0
1/14: clonerange f1[1 2] [0,1] -> f1[1 2] [2,1] error 95
1/18: rename(NOREPLACE) f2 to f3 0
1/18: rename source entry: id=2,parent=-1
1/26: copyrange f3[1] [0,0] -> f3[1] [2,0]
1/28: read - f3[1 2] zero size
1/29: setxattr c0 3 -1
LOG
got=$("${WORK}/bin/sh" -c '. "$1"; fsstress_results "$2"' sh "${STRESS_LIB}" "${WORK}/ss_mixed" | tr '\n' ';')
[[ "${got}" == "clonerange 95 1;copyrange 0 1;creat 0 1;creat 17 1;mknod 0 1;rename 0 1;setxattr -1 1;" ]] ||
  fail "fsstress_results: got '${got}'"
echo "PASS: fsstress_results classifies the lines of a log"

# The seed of the random mode is a number (a UUID's first hex digits, not its
# dash).
for _ in 1 2 3; do
  got=$(PATH="${WORK}/bin" "${WORK}/bin/sh" -c '. "$1"; stress_seed' sh "${STRESS_LIB}")
  [[ "${got}" =~ ^[0-9]+$ ]] || fail "stress_seed: got '${got}'"
done
echo "PASS: stress_seed is a number"

# The features fsx disabled must be the expected ones.
d() { printf 'fsx: main: filesystem does not support %s, disabling!\n' "$@"; }
d "clone range" "dedupe range" >"${WORK}/exp_raw"
sed 's/^.*support \(.*\), disabling!$/filesystem does not support \1/' "${WORK}/exp_raw" >"${WORK}/exp"
{ echo "Seed set to 1"; d "dedupe range" "clone range"; } >"${WORK}/fsx_two"
{ echo "Seed set to 1"; d "clone range" "dedupe range" "dontcache IO"; } >"${WORK}/fsx_three"
{ echo "Seed set to 1"; d "clone range"; } >"${WORK}/fsx_one"
expect_pass dis fsx_disabled_verdict dis "${WORK}/fsx_two" "${WORK}/exp"
expect_reject dis "a feature fsx newly disabled" fsx_disabled_verdict dis "${WORK}/fsx_three" "${WORK}/exp"
expect_reject dis "a feature fsx no longer disables" fsx_disabled_verdict dis "${WORK}/fsx_one" "${WORK}/exp"
