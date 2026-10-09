#!/bin/sh
# dcfs step 4.10 acceptance test: the durable dirty set and unclean-shutdown
# recovery.
#
# A power loss can leave the cache database and the backing filesystem at
# unrelated points in time: SQLite's WAL (synchronous=NORMAL) and the
# backing filesystem's journal each come back as some prefix of what was
# written, independently. The dangerous case the dirty set exists for is
# "cache ahead": dcfs's phase 3 of a mutation survived, but the backing
# filesystem lost the mutation itself. A power loss cannot honestly be
# produced inside one QEMU boot (the guest page cache survives anything
# short of a reboot, and a reboot ends the test run), so this test produces
# exactly the state such a power loss leaves instead:
#
#   1. mutate through /mnt (create, unlink, rename, chmod), so dcfs's cache
#      records the new state (phase 3) and the dirty set records what
#      changed (phase 1);
#   2. kill -9 the daemon: no clean shutdown, no sync point;
#   3. undo each mutation directly on the backing filesystem while dcfs is
#      down -- the backing filesystem "lost" the last few seconds;
#   4. restart dcfs against the same cache database.
#
# Without recovery dcfs would now serve the lost state (the created file,
# not the unlinked one, the old mode) from its cache forever. With it, the
# restart logs a WARNING naming the recovered entries, every entry the
# mutations touched shows the backing filesystem's truth, and everything
# nobody touched is still served warm (zero backing sectors read).
#
# It also checks the other half of the protocol: a periodic sync point
# (--sync_interval_sec) syncfs()es the backing filesystem and empties the
# dirty set, so a crash after it recovers nothing and stays warm; and a
# clean unmount is recognized as one at the next start.
#
# Run as /tests/power.sh by guest/init when booted with dcfs_test=power.sh;
# prints one "TEST ... PASS/FAIL" line per check and exits nonzero if any
# check failed.
FAILED=0
. "$(dirname "$0")/lib.sh"

DCFS=/bin/dcfs

SRC=/src
MNT=/mnt
DB=/cache/dcfs.db
LOGS=""

DAEMON_PID=""
MOUNTED=0

# See readonly.sh for why every cleanup command here is `|| true`-guarded.
cleanup() {
	rc=$?
	if [ "$rc" -ne 0 ] || [ "$FAILED" -ne 0 ]; then
		for log in $LOGS; do
			echo "--- dcfs stderr ($log) ---"
			cat "$log" 2>/dev/null
		done
	fi
	if [ "$MOUNTED" -eq 1 ]; then
		umount "$MNT" 2>/dev/null || umount -l "$MNT" 2>/dev/null || true
	fi
	if [ -n "$DAEMON_PID" ] && kill -0 "$DAEMON_PID" 2>/dev/null; then
		kill "$DAEMON_PID" 2>/dev/null || true
		wait "$DAEMON_PID" 2>/dev/null || true
	fi
}
trap cleanup EXIT

echo "power.sh: kernel $(uname -r)"

# start_daemon LOG [extra dcfs flags...]: overrides lib.sh's generic
# start_daemon only to also track LOG in $LOGS, for cleanup's dump-all-logs
# loop above.
start_daemon() {
	log=$1
	shift
	LOGS="$LOGS $log"
	"${MOUNT_DCFS:-/sbin/mount.dcfs}" -o "$(dcfs_options "$@")" "$SRC" "$MNT" >"$log" 2>&1 &
	DAEMON_PID=$!
	MOUNTED=0
	if "${TESTUTIL:-/bin/testutil}" waitmount "$MNT" present "$DAEMON_PID"; then
		MOUNTED=1
		return 0
	fi
	return 1
}

# crash_daemon: SIGKILL, so no sync point and no clean-shutdown marker.
crash_daemon() {
	kill -KILL "$DAEMON_PID" 2>/dev/null || true
	wait "$DAEMON_PID" 2>/dev/null || true
	DAEMON_PID=""
	umount -l "$MNT" 2>/dev/null || true
	MOUNTED=0
}

# stop_daemon: a clean unmount, which ends the daemon's session loop.
stop_daemon() {
	umount "$MNT" || umount -l "$MNT" || true
	MOUNTED=0
	wait "$DAEMON_PID" 2>/dev/null || true
	DAEMON_PID=""
}

# must_start NAME LOG [flags...]
must_start() {
	name=$1
	shift
	if start_daemon "$@"; then
		pass "$name"
	else
		fail "$name" "daemon did not mount within 10s"
		exit "$FAILED"
	fi
}

# warm_check NAME: stats and lists everything under $MNT/c and stats $MNT/g
# (none of it touched by a lost mutation) and requires zero backing sectors
# read.
warm_check() {
	drop_caches_quiesced
	before=$(sectors_read vdb)
	ok=1
	[ "$(stat -c %s "$MNT/c/c1")" = 3 ] || ok=0
	[ "$(stat -c %s "$MNT/c/c2")" = 3 ] || ok=0
	[ "$(ls "$MNT/c" | tr '\n' ' ')" = "c1 c2 " ] || ok=0
	stat -c %s "$MNT/g" >/dev/null || ok=0
	after=$(sectors_read vdb)
	if [ "$ok" = 1 ] && [ "$before" = "$after" ]; then
		pass "$1"
	else
		fail "$1" "ok=$ok sectors_read(vdb) $before -> $after"
	fi
}

# --- build the backing tree (before dcfs ever sees it) ---------------------

mount /dev/vdb "$SRC"
mkdir "$SRC/a" "$SRC/b" "$SRC/c"
echo a1 >"$SRC/a/a1"
echo b1 >"$SRC/b/b1"
echo c1 >"$SRC/c/c1"
echo c2 >"$SRC/c/c2"
echo f >"$SRC/f"
echo g >"$SRC/g"
chmod 644 "$SRC/f"
sync

mkdir -p /cache "$MNT"

# --- a clean shutdown is recognized as one --------------------------------

must_start mount /tmp/dcfs-1.log --sync_interval_sec=3600
find "$MNT" -exec stat -c '%s %a' {} + >/dev/null
stop_daemon
if grep -q "did not shut down cleanly" /tmp/dcfs-1.log; then
	fail clean-first-start "first start of a fresh cache reported recovery"
else
	pass clean-first-start
fi

must_start clean-restart /tmp/dcfs-2.log --sync_interval_sec=3600
if grep -q "did not shut down cleanly" /tmp/dcfs-2.log; then
	fail clean-shutdown-recognized "$(grep "cleanly" /tmp/dcfs-2.log)"
else
	pass clean-shutdown-recognized
fi
warm_check clean-restart-warm

# --- the backing filesystem loses the last mutations ------------------------

# Phase 1 (dirty set) and phase 3 (new cached state) of each of these are
# committed; --sync_interval_sec=3600 keeps any sync point from clearing
# the dirty set before the crash.
touch "$MNT/a/new"
rm "$MNT/a/a1"
mv "$MNT/b/b1" "$MNT/b/b2"
chmod 600 "$MNT/f"
if [ -e "$MNT/a/new" ] && [ ! -e "$MNT/a/a1" ] && [ -e "$MNT/b/b2" ] &&
	[ "$(stat -c %a "$MNT/f")" = 600 ]; then
	pass mutations-applied
else
	fail mutations-applied "$(ls -R "$MNT")"
fi
crash_daemon

# What a power loss that kept dcfs's database but not the backing
# filesystem's last journal commit leaves behind.
rm "$SRC/a/new"
echo a1 >"$SRC/a/a1"
mv "$SRC/b/b2" "$SRC/b/b1"
chmod 644 "$SRC/f"
sync

must_start restart-after-loss /tmp/dcfs-3.log --sync_interval_sec=3600
# Untouched entries first, before anything repopulates: still warm.
warm_check untouched-still-warm

if grep -q "did not shut down cleanly.*recovered [1-9][0-9]* dirty" /tmp/dcfs-3.log; then
	pass recovery-logged
else
	fail recovery-logged "no recovery WARNING in the log"
fi
grep "did not shut down cleanly" /tmp/dcfs-3.log || true

# Every entry the lost mutations touched shows the backing filesystem's
# truth, not what dcfs had cached.
if [ ! -e "$MNT/a/new" ]; then
	pass lost-create-forgotten
else
	fail lost-create-forgotten "a/new still served"
fi
if [ "$(cat "$MNT/a/a1" 2>&1)" = a1 ]; then
	pass lost-unlink-forgotten
else
	fail lost-unlink-forgotten "a/a1: $(cat "$MNT/a/a1" 2>&1)"
fi
if [ -e "$MNT/b/b1" ] && [ ! -e "$MNT/b/b2" ]; then
	pass lost-rename-forgotten
else
	fail lost-rename-forgotten "b: $(ls "$MNT/b" | tr '\n' ' ')"
fi
if [ "$(stat -c %a "$MNT/f")" = 644 ]; then
	pass lost-chmod-forgotten
else
	fail lost-chmod-forgotten "f mode $(stat -c %a "$MNT/f")"
fi
if [ "$(ls "$MNT/a" | tr '\n' ' ')" = "a1 " ]; then
	pass lost-listing-relisted
else
	fail lost-listing-relisted "a: $(ls "$MNT/a" | tr '\n' ' ')"
fi
stop_daemon

# --- a sync point empties the dirty set -------------------------------------

must_start sync-start /tmp/dcfs-4.log --sync_interval_sec=1
touch "$MNT/c/c3"
rm "$MNT/c/c3"
# The first request more than a second later runs the sync point. (A
# readdir, since the kernel would answer a stat from its own attribute
# cache without asking dcfs.)
sleep 2
ls "$MNT" >/dev/null
crash_daemon
must_start restart-after-sync /tmp/dcfs-5.log --sync_interval_sec=3600
if grep -q "did not shut down cleanly.*recovered 0 dirty" /tmp/dcfs-5.log; then
	pass sync-point-emptied-dirty-set
else
	fail sync-point-emptied-dirty-set "$(grep "cleanly" /tmp/dcfs-5.log)"
fi
warm_check after-sync-point-warm
stop_daemon

exit "$FAILED"
