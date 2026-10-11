#!/bin/sh
# SPDX-License-Identifier: MIT
# Self-check of scripts/xfstests-notrun.sh (step 26.17 changed the line it
# reads): a serial log in the form guest/xfstests.sh prints now gives the "not
# run" tests with their reasons, digits as N, and nothing else; a line in any
# other form gives nothing, so a list written from a log of another form is
# empty, and the shell test below says so instead of passing.
#
# Usage: xfstests_notrun_test.sh <xfstests-notrun.sh>
set -eu

SCRIPT=$1
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

fail() {
	echo "FAIL: $*" >&2
	exit 1
}

COST="dcfs-cpu=0.010s reads=0 writes=0 sectors_read=0 sectors_written=0 flushes=0"
cat >"$WORK/serial.log" <<EOT
xfstests.sh: shard 1/6
xfstests: generic/001 pass 12s dcfs-cpu=2.500s reads=10 writes=40 sectors_read=80 sectors_written=900 flushes=30
xfstests: generic/010 notrun 1s $COST: this test requires a block device
xfstests: generic/002 fail 3s $COST
xfstests: generic/003 notrun 1s $COST: Filesystem fuse.dcfs is not supported, kernel 6.18.55
xfstests: generic/020 notrun 0s dcfs-cpu=0.000s reads=0 writes=0 sectors_read=0 sectors_written=0 flushes=0: attr  v2   needed
EOT
got=$(sh "$SCRIPT" "$WORK/serial.log")
want='generic/003 Filesystem fuse.dcfs is not supported, kernel N.N.N
generic/010 this test requires a block device
generic/020 attr vN needed'
[ "$got" = "$want" ] || fail "the notrun list is '$got', want '$want'"
echo "PASS: the not-run tests of a log come out with their reasons"

# The form before step 26.17 (ticks) is not read: a list from such a log is empty.
printf 'xfstests: generic/010 notrun 1s dcfs-cpu=0ticks: this test requires a block device\n' >"$WORK/old.log"
got=$(sh "$SCRIPT" "$WORK/old.log")
[ -z "$got" ] || fail "a line of the old form gave '$got'"
echo "PASS: a line of another form gives nothing"
echo "PASS: all checks passed"
