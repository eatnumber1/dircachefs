#!/bin/sh
# dcfs step 11.1: I/O errors on the CACHE disk, the one holding the SQLite
# database (docs/plan/phases/11-crash-stress-failure-testing.md, 11.3, 11.4;
# design.md, "The write-through protocol" and "Sync points"). The cache
# filesystem sits on a dm device (guest/fault_lib.sh) whose table the test
# switches at a chosen point.
#
# 1. A write error in a mutation's phase 1 (the durable commit: SQLite's WAL
#    is fsynced before the syscall). Nothing may reach the backing filesystem
#    (the mutation's phase 2 never runs), the caller gets EIO, and
#    nothing wrong is served from then on. After the disk is healed and dcfs
#    restarted, it starts, serves what the backing filesystem holds and
#    mutates again.
# 2. A write error at a sync point: the backing filesystem is synced, then
#    the dirty set is cleared in the cache database, which fails (the cache
#    filesystem's journal aborted, so it is read-only). The dirty set must
#    survive: after the disk is healed and dcfs restarted the recovery names
#    the rows, and everything served matches the backing filesystem.
#
# Run as /tests/fault_cache.sh by guest/init when booted with
# dcfs_test=fault_cache.sh.
FAILED=0
. "$(dirname "$0")/lib.sh"
. "$(dirname "$0")/fault_lib.sh"
. "$(dirname "$0")/fault_dcfs_lib.sh"

DCFS=/bin/dcfs
TESTUTIL=/bin/testutil
SRC=/src
MNT=/mnt
DB=$CACHE_DIR/dcfs.db
LOG1=/tmp/dcfs-1.log
LOG2=/tmp/dcfs-2.log
LOG3=/tmp/dcfs-3.log
LOG4=/tmp/dcfs-4.log

DAEMON_PID=""
MOUNTED=0

cleanup() {
	rc=$?
	if [ "$rc" -ne 0 ] || [ "$FAILED" -ne 0 ]; then
		for log in "$LOG1" "$LOG2" "$LOG3" "$LOG4"; do
			echo "--- dcfs stderr ($log) ---"
			cat "$log" 2>/dev/null
		done
	fi
	umount -l "$MNT" 2>/dev/null || true
	if [ -n "$DAEMON_PID" ] && kill -0 "$DAEMON_PID" 2>/dev/null; then
		kill -KILL "$DAEMON_PID" 2>/dev/null || true
		wait "$DAEMON_PID" 2>/dev/null || true
	fi
	fd_umount_disks
	fault_unwrap "$FD_BACK" >/dev/null 2>&1 || true
	fault_unwrap "$FD_CACHE" >/dev/null 2>&1 || true
}
trap cleanup EXIT

echo "fault_cache.sh: kernel $(uname -r)"
require_commands umount sync

alive() {
	[ -n "$DAEMON_PID" ] && kill -0 "$DAEMON_PID" 2>/dev/null
}

# heal_and_remount: the daemon is gone; both disks healthy, both filesystems
# mounted again (the cache filesystem's aborted journal replays).
heal_and_remount() {
	fault_mode "$FD_CACHE" healthy || fail heal "fault_mode healthy failed"
	fd_umount_disks
	mount "$(fault_dev "$FD_BACK")" "$SRC" || fail remount-backing "cannot mount the backing filesystem"
	mount "$(fault_dev "$FD_CACHE")" "$CACHE_DIR" || fail remount-cache "cannot mount the healed cache filesystem"
}

fd_setup || {
	fail setup "wrapping or mounting the disks failed"
	exit "$FAILED"
}
mkdir "$SRC/d1" "$SRC/d2"
echo known >"$SRC/d1/known"
echo other >"$SRC/d2/other"
sync

# --- 1. a cache-disk write error in phase 1 ---------------------------------

if fd_start "$LOG1"; then
	pass mount
else
	fail mount "daemon did not mount within 10s"
	exit "$FAILED"
fi
ls "$MNT/d1" "$MNT/d2" >/dev/null
fault_mode "$FD_CACHE" error-writes || fail inject-cache-write-errors "fault_mode failed"
out=$(touch "$MNT/d2/new" 2>&1)
rc=$?
echo "fault_cache.sh: create with the cache disk failing said: rc=$rc $out"
if [ "$rc" -ne 0 ]; then
	pass phase1-error-replied
else
	fail phase1-error-replied "touch succeeded with every cache-disk write failing"
fi
# A failure of the cache database's storage is EIO to the caller (step
# 11.1b): not EAGAIN ("Resource temporarily unavailable"), which asks the
# caller to retry what will not succeed.
case "$out" in
*"I/O error"*) pass phase1-error-is-eio ;; # musl's strerror(EIO)
*) fail phase1-error-is-eio "touch said: $out" ;;
esac
if [ ! -e "$SRC/d2/new" ]; then
	pass phase1-error-backing-untouched
else
	fail phase1-error-backing-untouched "d2/new reached the backing filesystem although phase 1 failed"
fi
if alive; then
	if [ ! -e "$MNT/d2/new" ]; then
		pass phase1-error-name-not-present
	else
		fail phase1-error-name-not-present "d2/new is served as present"
	fi
else
	echo "fault_cache.sh: the daemon stopped serving after the cache-disk error (allowed: it must not serve wrong data)"
	pass phase1-error-name-not-present
fi
fd_crash
heal_and_remount
if fd_start "$LOG2"; then
	pass restart-after-phase1-error
else
	fail restart-after-phase1-error "daemon did not mount within 10s"
	exit "$FAILED"
fi
if [ ! -e "$MNT/d2/new" ] && [ "$(ls "$MNT/d2")" = other ] && [ "$(cat "$MNT/d1/known")" = known ]; then
	pass restart-serves-the-backing
else
	fail restart-serves-the-backing "d2: $(ls "$MNT/d2" 2>&1) d1/known: $(cat "$MNT/d1/known" 2>&1)"
fi
if touch "$MNT/d2/new" && [ -e "$SRC/d2/new" ] && [ -e "$MNT/d2/new" ]; then
	pass create-after-heal
else
	fail create-after-heal "touch d2/new after healing"
fi
fd_crash

# --- 2. a cache-disk write error at a sync point -----------------------------

# A third run, whose sync interval is short, over the same cache database.
fd_umount_disks
fault_mode "$FD_BACK" healthy
mount "$(fault_dev "$FD_BACK")" "$SRC"
mount "$(fault_dev "$FD_CACHE")" "$CACHE_DIR"
if fd_start "$LOG3" --sync_interval_sec=2; then
	pass sync-run-mount
else
	fail sync-run-mount "daemon did not mount within 10s"
	exit "$FAILED"
fi
ls "$MNT/d1" "$MNT/d2" >/dev/null
# A mutation, whose phase 1 puts d1 in the dirty set (durably: the WAL is
# fsynced), made well within the sync interval.
touch "$MNT/d1/made"
[ -e "$SRC/d1/made" ] || fail sync-run-create "d1/made is not on the backing filesystem"
# The cache disk fails, and its filesystem's journal meets the errors.
fault_mode "$FD_CACHE" error-writes || fail inject-cache-sync-errors "fault_mode failed"
echo junk >"$CACHE_DIR/junk" 2>/dev/null || true
sync
sleep 3
# The first request after the interval runs the sync point: the backing
# filesystem is synced, and clearing the dirty set fails.
ls "$MNT/d1" >/tmp/ls-d1.out 2>&1
if alive; then
	if grep -q "made" /tmp/ls-d1.out; then
		pass sync-error-still-serves
	else
		fail sync-error-still-serves "ls d1: $(cat /tmp/ls-d1.out)"
	fi
else
	echo "fault_cache.sh: the daemon stopped serving after the sync-point error (allowed)"
	pass sync-error-still-serves
fi
echo "fault_cache.sh: the daemon's log after the sync point:"
grep -i -e sync -e warning -e error "$LOG3" | tail -5
fd_crash
heal_and_remount
if fd_start "$LOG4"; then
	pass restart-after-sync-error
else
	fail restart-after-sync-error "daemon did not mount within 10s"
	exit "$FAILED"
fi
if [ "$(fd_recovered "$LOG4")" -ge 1 ]; then
	pass sync-error-dirty-set-survived
else
	fail sync-error-dirty-set-survived "no recovered dirty rows: the failed sync point cleared them: $(grep -i cleanly "$LOG4")"
fi
if [ -e "$MNT/d1/made" ] && [ "$(ls "$MNT/d1" | tr '\n' ' ')" = "known made " ]; then
	pass restart-serves-the-backing-2
else
	fail restart-serves-the-backing-2 "d1: $(ls "$MNT/d1" 2>&1)"
fi
require_no_reclaim no-reclaim
exit "$FAILED"
