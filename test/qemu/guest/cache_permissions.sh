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
#     0700, owned by root;
#   - an existing, group- or world-accessible one gets a WARNING logged
#     (and dcfs still starts) -- exercised against /cache itself, created
#     0755 by guest/init's plain `mkdir -p /cache` with no explicit mode,
#     exactly the directory every other e2e test already points
#     --cache_db at directly;
#   - a pre-existing database (and -wal) left mode 0644 by a build from
#     before this check -- but still owned by root -- is tightened to 0600
#     (with a WARNING), not refused (case 3);
#   - a --cache_db path that is a symlink is refused outright, and the
#     link's target is never opened at all, let alone modified (case 4);
#     and
#   - a --cache_db path that already exists as a regular file owned by
#     some other (unprivileged) user is refused outright, and that file is
#     never touched (case 5) -- both of these guard against another local
#     user pre-creating the cache path in a directory dcfs can write to
#     (e.g. a shared /tmp), to have root either follow a symlink
#     elsewhere, or read/tamper with a file they already control.
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
LOG3=/tmp/dcfs-3.log
LOG4=/tmp/dcfs-4.log
LOG5=/tmp/dcfs-5.log

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
		echo "--- dcfs stderr (case 3) ---"
		cat "$LOG3" 2>/dev/null
		echo "--- dcfs stderr (case 4) ---"
		cat "$LOG4" 2>/dev/null
		echo "--- dcfs stderr (case 5) ---"
		cat "$LOG5" 2>/dev/null
		for f in /tmp/dcfs-r1-*.log; do
			[ -e "$f" ] || continue
			echo "--- $f ---"
			cat "$f"
		done
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

# --- case 3: pre-existing database (and -wal) left mode 0644 by root -----
#
# A cache database (or its -wal) made before this check existed, still
# owned by root, must be tightened to 0600 -- not refused outright, since
# nothing hostile is implied by a mode a previous dcfs build simply never
# fixed.

mkdir -m 700 /cache/case3
: >/cache/case3/dcfs.db
chmod 644 /cache/case3/dcfs.db
: >/cache/case3/dcfs.db-wal
chmod 644 /cache/case3/dcfs.db-wal
: >/cache/case3/dcfs.db-shm
chmod 644 /cache/case3/dcfs.db-shm

DB3=/cache/case3/dcfs.db
DB=$DB3
if start_daemon "$LOG3"; then
	pass case3-mount
	MOUNTED=1

	check_mode_owner case3-db-mode "$DB3" 600
	check_mode_owner case3-wal-mode "$DB3-wal" 600
	check_mode_owner case3-shm-mode "$DB3-shm" 600

	if grep -q "tightened to 0600" "$LOG3"; then
		pass case3-warning-logged
	else
		fail case3-warning-logged "no 'tightened to 0600' WARNING in $LOG3"
	fi

	kill -TERM "$DAEMON_PID" 2>/dev/null || true
	wait "$DAEMON_PID" 2>/dev/null || true
	DAEMON_PID=""
	if is_mounted "$MNT"; then
		umount "$MNT" 2>/dev/null || true
	fi
	MOUNTED=0
else
	fail case3-mount "daemon did not mount within 10s"
fi

# --- case 4: --cache_db path is a symlink ---------------------------------
#
# Another local user able to write the cache directory (or who got there
# first, e.g. a shared /tmp) could leave a symlink at the --cache_db path
# to have root open (and, via WAL, write through) some unrelated file.
# dcfs must refuse outright, and must never have opened the link's target
# at all -- checked by hashing it before and after.

mkdir -m 700 /cache/case4
echo "untouched content" >/cache/case4/target.db
chmod 644 /cache/case4/target.db
ln -s target.db /cache/case4/dcfs.db
before_target=$(md5sum /cache/case4/target.db)

DB4=/cache/case4/dcfs.db
DB=$DB4
if start_daemon "$LOG4"; then
	fail case4-refused "dcfs mounted with --cache_db a symlink"
	kill -TERM "$DAEMON_PID" 2>/dev/null || true
	wait "$DAEMON_PID" 2>/dev/null || true
	if is_mounted "$MNT"; then
		umount "$MNT" 2>/dev/null || true
	fi
	MOUNTED=0
else
	pass case4-refused
	wait "$DAEMON_PID" 2>/dev/null || true
fi
DAEMON_PID=""

if grep -qi "symlink" "$LOG4"; then
	pass case4-message
else
	fail case4-message "no symlink-refusal message in $LOG4"
fi

if [ -L /cache/case4/dcfs.db ]; then
	pass case4-symlink-intact
else
	fail case4-symlink-intact "/cache/case4/dcfs.db is no longer a symlink"
fi

after_target=$(md5sum /cache/case4/target.db)
if [ "$before_target" = "$after_target" ]; then
	pass case4-target-untouched
else
	fail case4-target-untouched "target.db changed: before=$before_target after=$after_target"
fi

# Opened-but-not-modified: had dcfs opened the target, it would have
# tightened its mode to 0600.
if [ "$(stat -c %a /cache/case4/target.db)" = "644" ]; then
	pass case4-target-mode-untouched
else
	fail case4-target-mode-untouched "target.db mode is now $(stat -c %a /cache/case4/target.db), not 644"
fi

# --- case 5: --cache_db path already exists, owned by another user -------
#
# A file another local user already owns at the --cache_db path could be
# one they can read or tamper with once dcfs starts writing the cache
# through it, or one they crafted to feed dcfs/SQLite bad bytes. dcfs must
# refuse outright, and must never have touched it (its ownership must stay
# exactly as it was).

mkdir -m 700 /cache/case5
: >/cache/case5/dcfs.db
chown 1000:1000 /cache/case5/dcfs.db

DB5=/cache/case5/dcfs.db
DB=$DB5
if start_daemon "$LOG5"; then
	fail case5-refused "dcfs mounted with --cache_db owned by uid 1000"
	kill -TERM "$DAEMON_PID" 2>/dev/null || true
	wait "$DAEMON_PID" 2>/dev/null || true
	if is_mounted "$MNT"; then
		umount "$MNT" 2>/dev/null || true
	fi
	MOUNTED=0
else
	pass case5-refused
	wait "$DAEMON_PID" 2>/dev/null || true
fi
DAEMON_PID=""

if grep -q "owned by uid 1000" "$LOG5"; then
	pass case5-message
else
	fail case5-message "no ownership-refusal message in $LOG5"
fi

owner=$(stat -c %u /cache/case5/dcfs.db)
if [ "$owner" = "1000" ]; then
	pass case5-untouched
else
	fail case5-untouched "/cache/case5/dcfs.db is now owned by uid $owner, not 1000"
fi

# --- R1: the cache files must grant no access beyond the backing root ----
#
# The database, -wal and -shm owner must be root or the owner of --source's
# root directory; group/other read or write only if that directory grants
# the same to the group (which must be the directory's group) / others.
# /src's root directory is root:root 0755 after mkfs.

# refused NAME LOGSUFFIX PATTERN: dcfs must refuse to start (DB is set by
# the caller) with a message matching PATTERN.
refused() {
	name=$1
	log=/tmp/dcfs-r1-$2.log
	pattern=$3
	if start_daemon "$log"; then
		fail "$name-refused" "dcfs mounted"
		kill -TERM "$DAEMON_PID" 2>/dev/null || true
		wait "$DAEMON_PID" 2>/dev/null || true
		if is_mounted "$MNT"; then
			umount "$MNT" 2>/dev/null || true
		fi
		MOUNTED=0
	else
		pass "$name-refused"
		wait "$DAEMON_PID" 2>/dev/null || true
	fi
	DAEMON_PID=""
	if grep -q "$pattern" "$log"; then
		pass "$name-message"
	else
		fail "$name-message" "no '$pattern' in $log"
	fi
}

# Both permission sets must be named in the error.
BOTH="grants more access than the backing root"

# 6: database 0644 but the backing root is 0700: group/other read.
mkdir -m 700 /cache/case6
: >/cache/case6/dcfs.db
chmod 644 /cache/case6/dcfs.db
chmod 700 /src
DB=/cache/case6/dcfs.db
refused case6 6 "$BOTH"
if grep -q "mode 0644" /tmp/dcfs-r1-6.log && grep -q "mode 0700" /tmp/dcfs-r1-6.log; then
	pass case6-both-modes-named
else
	fail case6-both-modes-named "log lacks both modes"
fi
chmod 755 /src

# 7: -shm world-writable (the directory grants others only r-x); -wal and
# -shm are checked like the database.
mkdir -m 700 /cache/case7
: >/cache/case7/dcfs.db-shm
chmod 666 /cache/case7/dcfs.db-shm
DB=/cache/case7/dcfs.db
refused case7 7 "$BOTH"

# 8: -wal group differs from the directory's group, mode grants group read.
mkdir -m 700 /cache/case8
: >/cache/case8/dcfs.db-wal
chmod 640 /cache/case8/dcfs.db-wal
chown 0:1000 /cache/case8/dcfs.db-wal
DB=/cache/case8/dcfs.db
refused case8 8 "$BOTH"

# 9: symlinked -wal and -shm are refused.
mkdir -m 700 /cache/case9w /cache/case9s
echo x >/cache/case9w/target
ln -s target /cache/case9w/dcfs.db-wal
echo x >/cache/case9s/target
ln -s target /cache/case9s/dcfs.db-shm
DB=/cache/case9w/dcfs.db
refused case9w 9w "symlink"
DB=/cache/case9s/dcfs.db
refused case9s 9s "symlink"

# 10: foreign-owned -wal and -shm are refused.
mkdir -m 700 /cache/case10w /cache/case10s
: >/cache/case10w/dcfs.db-wal
chown 1000:0 /cache/case10w/dcfs.db-wal
: >/cache/case10s/dcfs.db-shm
chown 1000:0 /cache/case10s/dcfs.db-shm
DB=/cache/case10w/dcfs.db
refused case10w 10w "owned by uid 1000"
DB=/cache/case10s/dcfs.db
refused case10s 10s "owned by uid 1000"

# 11: a non-regular file (FIFO) at the database path is refused.
mkdir -m 700 /cache/case11
mkfifo /cache/case11/dcfs.db
DB=/cache/case11/dcfs.db
refused case11 11 "not a regular file"

# 12: positive: the backing root is owned by uid 1000, so a database owned
# by uid 1000 (mode 0600) is accepted.
mkdir -m 700 /cache/case12
: >/cache/case12/dcfs.db
chmod 600 /cache/case12/dcfs.db
chown 1000:1000 /cache/case12/dcfs.db
chown 1000:1000 /src
DB=/cache/case12/dcfs.db
if start_daemon /tmp/dcfs-r1-12.log; then
	pass case12-owner-of-backing-root-accepted
	MOUNTED=1
	kill -TERM "$DAEMON_PID" 2>/dev/null || true
	wait "$DAEMON_PID" 2>/dev/null || true
	DAEMON_PID=""
	if is_mounted "$MNT"; then
		umount "$MNT" 2>/dev/null || true
	fi
	MOUNTED=0
else
	fail case12-owner-of-backing-root-accepted "dcfs refused a database owned by the backing root's owner"
fi
chown 0:0 /src

exit "$FAILED"
