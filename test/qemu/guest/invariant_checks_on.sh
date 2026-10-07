#!/bin/sh
# dcfs step 26.2: self-check of the runtime invariant checks' tier selection
# (test/qemu/README.md, "Gates and their self-checks"). This test is small,
# so qemu_test.bzl's initramfs_for gives it :initramfs_checked, as it gives
# every small and medium test: the daemon must be the testonly checking
# build, which says so in its log at startup (dcfs/testonly/
# main_invariant_checker.cc). If the tiers ever booted the plain build, the
# invariant checks would pass vacuously; this fails instead.
#
# Run as /tests/invariant_checks_on.sh by guest/init when booted with
# dcfs_test=invariant_checks_on.sh.
FAILED=0
. "$(dirname "$0")/lib.sh"

DCFS=/bin/dcfs
SRC=/src
DB=/cache/dcfs.db
MNT=/mnt
LOG=/tmp/dcfs.log

DAEMON_PID=""
MOUNTED=0

cleanup() {
	rc=$?
	if [ "$rc" -ne 0 ] || [ "$FAILED" -ne 0 ]; then
		echo "--- dcfs stderr ---"
		tail -n 50 "$LOG" 2>/dev/null
	fi
	if [ "$MOUNTED" -eq 1 ]; then
		umount "$MNT" 2>/dev/null || true
	fi
	if [ -n "$DAEMON_PID" ] && kill -0 "$DAEMON_PID" 2>/dev/null; then
		kill "$DAEMON_PID" 2>/dev/null || true
		wait "$DAEMON_PID" 2>/dev/null || true
	fi
}
trap cleanup EXIT

mount /dev/vdb "$SRC"
mkdir -p /cache
echo x >"$SRC/f"

if start_daemon "$LOG"; then
	pass mount
else
	fail mount "dcfs did not mount"
	exit 1
fi

if grep -q "invariant checks: on" "$LOG"; then
	pass checking-build
else
	fail checking-build "the daemon of a small test is not the checking build (no 'invariant checks: on' in its log): qemu_test.bzl's initramfs_for?"
fi

# Some requests, each checked at its end, and DESTROY's full check at the
# unmount: a violation would fail the run (run-qemu.sh).
if [ "$(cat "$MNT/f")" = x ] && ls "$MNT" >/dev/null && mkdir "$MNT/d" &&
	echo y >"$MNT/d/g" && rm "$MNT/d/g" && rmdir "$MNT/d"; then
	pass requests
else
	fail requests "requests through the mount failed"
fi

kill -TERM "$DAEMON_PID" 2>/dev/null || true
if wait "$DAEMON_PID"; then
	pass clean-exit
else
	fail clean-exit "the daemon exited nonzero (an invariant violated at shutdown?)"
fi
DAEMON_PID=""
MOUNTED=0
exit "$FAILED"
