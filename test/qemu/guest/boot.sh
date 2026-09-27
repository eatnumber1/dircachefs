#!/bin/sh
# dcfs's own QEMU boot smoke test (not adapted from fuse-generation-qemu;
# that repo's guest/run-tests.sh is the LKML patch's test suite and tests
# a different daemon).
#
# Checks that dcfs and fhtest are present and runnable and that the vdb
# scratch disk (mkfs'd ext4 by run-qemu.sh) mounts. Run as /tests/boot.sh
# by guest/init when booted with dcfs_test=boot.sh; prints one "TEST ...
# PASS/FAIL" line per check and exits nonzero if any check failed. init
# turns that into the final ALL-TESTS-PASSED / TEST-FAILED verdict.
FAILED=0
pass() { echo "TEST $1 PASS"; }
fail() { echo "TEST $1 FAIL ($2)"; FAILED=1; }

echo "boot.sh: kernel $(uname -r)"

if [ -x /bin/dcfs ]; then
	pass dcfs-present
else
	fail dcfs-present "not found or not executable"
fi

# dcfs uses Abseil flags: --help can exit nonzero after printing usage
# (Abseil treats an unrecognized/help flag as a parse error for --help;
# --helpfull is the well-known one), so accept either a zero exit or
# usage-looking output.
OUT=$(/bin/dcfs --help 2>&1)
RC=$?
if [ "$RC" -eq 0 ] || printf '%s' "$OUT" | grep -q -e '--source' -e '[Uu]sage'; then
	pass dcfs-help
else
	fail dcfs-help "rc=$RC: $OUT"
fi

if [ -x /bin/fhtest ]; then
	pass fhtest-present
else
	fail fhtest-present "not found or not executable"
fi

if [ -b /dev/vdb ]; then
	pass vdb-present
else
	fail vdb-present "/dev/vdb missing"
fi

if mount -t ext4 /dev/vdb /src; then
	pass vdb-mount
	umount /src
else
	fail vdb-mount "mount failed"
fi

exit "$FAILED"
