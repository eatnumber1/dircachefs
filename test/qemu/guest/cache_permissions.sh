#!/bin/sh
# dcfs step 2 acceptance test: cache database permissions.
#
# Before this step, main.cc created the cache database mode 0644 after
# setting the daemon's own umask to 0 (see that umask(0) call's own comment
# for why), and SQLite gives its -wal/-shm files the main file's mode: any
# local user could read every cached name, attribute, xattr and symlink
# target, including those of directories they cannot list. This checks:
#
#   - the database, -wal and -shm files end up mode 0600, owned by root;
#   - an unprivileged user (//tools:testutil's "runas") cannot open any of
#     the three for reading;
#   - a cache-database directory that does not exist yet is created mode
#     0700, owned by root; and
#   - an existing, group- or world-accessible one gets a WARNING logged
#     (and dcfs still starts) -- exercised against /cache itself, created
#     0755 by guest/init's plain `mkdir -p /cache` with no explicit mode,
#     exactly the directory every other e2e test already points
#     --cache_db at directly.
#
# Uses only busybox applets/options (verified against the exact busybox
# baked into the initramfs: `busybox --list`, `busybox <applet> --help`) --
# no GNU find/stat/coreutils extensions -- plus //tools:testutil's "runas"
# to act as an unprivileged user (the guest has no /etc/passwd users).
#
# Run as /tests/cache_permissions.sh by guest/init when booted with
# dcfs_test=cache_permissions.sh; prints one "TEST ... PASS/FAIL" line per
# check and exits nonzero if any check failed. init turns that into the
# final ALL-TESTS-PASSED / TEST-FAILED verdict.
FAILED=0
. "$(dirname "$0")/lib.sh"

DCFS=/bin/dcfs
TESTUTIL=/bin/testutil

SRC=/src
MNT=/mnt
LOG1=/tmp/dcfs-1.log
LOG2=/tmp/dcfs-2.log

DAEMON_PID=""
MOUNTED=0

# See readonly.sh for why every cleanup command here is `|| true`-guarded.
cleanup() {
	rc=$?
	if [ "$rc" -ne 0 ] || [ "$FAILED" -ne 0 ]; then
		echo "--- dcfs stderr (case 1) ---"
		cat "$LOG1" 2>/dev/null
		echo "--- dcfs stderr (case 2) ---"
		cat "$LOG2" 2>/dev/null
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

echo "cache_permissions.sh: kernel $(uname -r)"

mount /dev/vdb /src
echo hello >/src/file.txt

unpriv() { "$TESTUTIL" runas 1000 1000 - -- "$@"; }

# check_mode_owner NAME PATH WANT_MODE: PASS if PATH exists, is WANT_MODE
# (octal, as `stat -c %a` prints it) and is owned by uid/gid 0; FAIL with
# the actual values otherwise.
check_mode_owner() {
	name=$1
	path=$2
	want_mode=$3
	if [ ! -e "$path" ]; then
		fail "$name" "$path does not exist"
		return
	fi
	mode=$(stat -c %a "$path")
	uid=$(stat -c %u "$path")
	gid=$(stat -c %g "$path")
	if [ "$mode" = "$want_mode" ] && [ "$uid" = "0" ] && [ "$gid" = "0" ]; then
		pass "$name"
	else
		fail "$name" "mode=$mode uid=$uid gid=$gid (want mode=$want_mode uid=0 gid=0)"
	fi
}

# check_unreadable NAME PATH: an unprivileged user's attempt to read PATH
# must fail -- EACCES on the file itself, or on a parent directory along
# the way, either way nothing about it is readable.
check_unreadable() {
	name=$1
	path=$2
	if out=$(unpriv cat "$path" 2>&1); then
		fail "$name" "unprivileged read of $path succeeded: $out"
	else
		pass "$name"
	fi
}

# --- case 1: existing, group/world-accessible cache directory -----------
#
# /cache was created by guest/init's plain `mkdir -p /cache` (no explicit
# mode), which under the guest's default umask leaves it 0755 -- an
# existing, group- or world-accessible directory, and also exactly what
# every other e2e test's --cache_db=/cache/dcfs.db already points at: this
# confirms that case still starts (with a WARNING), not that it is refused.

DB1=/cache/dcfs.db
DB=$DB1
if [ "$(stat -c %a /cache)" != "700" ]; then
	pass case1-precondition
else
	fail case1-precondition "/cache is already 0700; this case needs it group- or world-accessible to mean anything"
fi

if start_daemon "$LOG1"; then
	pass case1-mount
	MOUNTED=1

	check_mode_owner case1-db-mode "$DB1" 600
	check_mode_owner case1-wal-mode "$DB1-wal" 600
	check_mode_owner case1-shm-mode "$DB1-shm" 600

	check_unreadable case1-unpriv-db "$DB1"
	check_unreadable case1-unpriv-wal "$DB1-wal"
	check_unreadable case1-unpriv-shm "$DB1-shm"

	if grep -q "is group- or world-accessible" "$LOG1"; then
		pass case1-warning-logged
	else
		fail case1-warning-logged "no WARNING about /cache in $LOG1"
	fi

	kill -TERM "$DAEMON_PID" 2>/dev/null || true
	wait "$DAEMON_PID" 2>/dev/null || true
	DAEMON_PID=""
	if is_mounted "$MNT"; then
		umount "$MNT" 2>/dev/null || true
	fi
	MOUNTED=0
else
	fail case1-mount "daemon did not mount within 10s"
fi

# --- case 2: cache directory does not exist yet --------------------------

DB2=/cache/newdir/dcfs.db
DB=$DB2
if [ ! -e /cache/newdir ]; then
	pass case2-precondition
else
	fail case2-precondition "/cache/newdir already exists"
fi

if start_daemon "$LOG2"; then
	pass case2-mount
	MOUNTED=1

	check_mode_owner case2-dir-mode /cache/newdir 700
	check_mode_owner case2-db-mode "$DB2" 600
	check_mode_owner case2-wal-mode "$DB2-wal" 600
	check_mode_owner case2-shm-mode "$DB2-shm" 600

	check_unreadable case2-unpriv-db "$DB2"
	check_unreadable case2-unpriv-wal "$DB2-wal"
	check_unreadable case2-unpriv-shm "$DB2-shm"

	if grep -q "is group- or world-accessible" "$LOG2"; then
		fail case2-no-warning "unexpected WARNING in $LOG2 for a freshly created 0700 directory"
	else
		pass case2-no-warning
	fi

	kill -TERM "$DAEMON_PID" 2>/dev/null || true
	wait "$DAEMON_PID" 2>/dev/null || true
	DAEMON_PID=""
	if is_mounted "$MNT"; then
		umount "$MNT" 2>/dev/null || true
	fi
	MOUNTED=0
else
	fail case2-mount "daemon did not mount within 10s"
fi

exit "$FAILED"
