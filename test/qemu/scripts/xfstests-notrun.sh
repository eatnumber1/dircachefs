#!/bin/sh
# Writes the notrun list of a file system (test/qemu/guest/xfstests.<fstype>.notrun)
# from the serial logs of its xfstests shards (step 17.1): one
# "generic/NNN <reason>" per test the run did not run, with the reason in the
# form guest/xfstests_lib.sh's xfstests_reason gives it (digits as N, one
# space between words, 100 characters), sorted.
#
#   test/qemu/scripts/xfstests-notrun.sh bazel-testlogs/test/qemu/xfstests_*_test_ext4/test.outputs/serial.log
#
# The result is a starting point to read, not to accept: every line is a test
# that stops being a test of dcfs, and a reason that says a probe broke (xfs_io
# ... failed, O_DIRECT, fallocate, xattr) deserves a look before it is listed.
cat "$@" |
	sed -n 's|^xfstests: \(generic/[0-9]*\) notrun [0-9]*s dcfs-cpu=[0-9]*ticks: \(.*\)$|\1 \2|p; s|^xfstests: \(generic/[0-9]*\) notrun [0-9]*s: \(.*\)$|\1 \2|p' |
	awk '{ id = $1; $1 = ""; r = substr($0, 2); gsub(/[ \t]+/, " ", r); gsub(/[0-9]+/, "N", r); print id, substr(r, 1, 100) }' |
	LC_ALL=C sort -u
