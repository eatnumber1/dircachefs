#!/bin/sh
# Host-side self-check of guest/ram_disks.sh's verdicts (step 26.17): the real
# ram_disk_judge (guest/ram_disks_lib.sh) over canned values, good ones (every
# check ok) and one bad value at a time (the check that value belongs to, and
# only that one, says why not). A check that nothing makes fail passes every
# RAM disk.
#
# Usage: ram_disks_lib_test.sh <ram_disks_lib.sh>
set -eu

LIB=$1
fail() {
	echo "FAIL: $*" >&2
	exit 1
}
# shellcheck disable=SC1090
. "$LIB"

SIZE=67108864

# judged <ext4|xfs> KERNEL FILE MAGIC SIZE FS ITABLE LOGBS ROTATIONAL: the verdict
# lines of a disk declared with the first argument's filesystem and 64 MiB.
judged() {
	j_want=$1
	shift
	ram_disk_judge "$1" "$2" "$3" "$4" "$SIZE" "$5" "$j_want" "$6" "$7" "$8"
}

GOOD_EXT4="loop0 /ramdisks/vdb.img 1021994 $SIZE ext4 4 2 1"
GOOD_XFS="loop1 /ramdisks/vdc.img 1021994 $SIZE xfs - - 1"

# shellcheck disable=SC2086 # the canned values are words
bad_lines=$(judged ext4 $GOOD_EXT4 | grep -v ' ok$' || true)
[ -z "$bad_lines" ] || fail "a good ext4 RAM disk fails: $bad_lines"
# shellcheck disable=SC2086
bad_lines=$(judged xfs $GOOD_XFS | grep -v ' ok$' || true)
[ -z "$bad_lines" ] || fail "a good xfs RAM disk fails: $bad_lines"
# shellcheck disable=SC2086
n=$(judged ext4 $GOOD_EXT4 | wc -l)
[ "$n" -eq 7 ] || fail "an ext4 RAM disk has $n checks, not 7 (is-loop, backing, size, filesystem, rotational, itable-zeroed, mke2fs-profile)"
echo "PASS: a good ext4 and a good xfs RAM disk pass every check"

# bad <check> <ext4|xfs> <good values with one replaced...>: only <check> fails.
bad() {
	b_check=$1
	b_want=$2
	shift 2
	b_out=$(judged "$b_want" "$@")
	b_failed=$(echo "$b_out" | grep -v ' ok$' | cut -d' ' -f1 | tr '\n' ' ')
	[ "$b_failed" = "$b_check " ] || fail "expected only $b_check to fail for '$*', got: $b_failed"
}
bad is-loop ext4 vda /ramdisks/vdb.img 1021994 $SIZE ext4 4 2 1
bad backed-by-ramdisks-tmpfs ext4 loop0 /tmp/vdb.img 1021994 $SIZE ext4 4 2 1
bad backed-by-ramdisks-tmpfs ext4 loop0 /ramdisks/vdb.img ef53 $SIZE ext4 4 2 1
bad backed-by-ramdisks-tmpfs ext4 loop0 "" "" $SIZE ext4 4 2 1
bad size ext4 loop0 /ramdisks/vdb.img 1021994 1048576 ext4 4 2 1
bad filesystem ext4 loop0 /ramdisks/vdb.img 1021994 $SIZE xfs 4 2 1
bad rotational ext4 loop0 /ramdisks/vdb.img 1021994 $SIZE ext4 4 2 0
bad rotational ext4 loop0 /ramdisks/vdb.img 1021994 $SIZE ext4 4 2 ""
bad itable-zeroed ext4 loop0 /ramdisks/vdb.img 1021994 $SIZE ext4 0 2 1
bad itable-zeroed ext4 loop0 /ramdisks/vdb.img 1021994 $SIZE ext4 3 2 1
bad mke2fs-profile ext4 loop0 /ramdisks/vdb.img 1021994 $SIZE ext4 4 0 1
bad rotational xfs loop1 /ramdisks/vdc.img 1021994 $SIZE xfs - - 0
echo "PASS: each check fails, alone, for the value it judges"

# The ext4-only checks are not made on the others (their values are "-").
judged xfs $GOOD_XFS | grep -q 'itable\|profile' && fail "an xfs disk is judged by ext4's checks"
echo "PASS: ext4's checks are not made for xfs"
echo "PASS: all checks passed"
