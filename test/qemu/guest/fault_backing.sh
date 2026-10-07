#!/bin/sh
# dcfs step 11.1: I/O errors on the BACKING disk (design.md, "The
# write-through protocol", "Phase 2"; docs/plan/phases/11-crash-stress-
# failure-testing.md, 11.3). The backing filesystem sits on a dm device
# (guest/fault_lib.sh) whose table the test switches at a chosen point.
#
# 1. A backing read error during a cold LOOKUP. The error goes back to the
#    caller; nothing is recorded as present or as absent, so when the disk is
#    healthy again the same lookup answers what the backing filesystem holds
#    (a recorded "absent" would answer ENOENT for a file that exists, a
#    recorded error nothing at all).
# 2. A backing write error during a CREATE's phase 2. The backing filesystem's
#    journal has met the write errors and aborted (ext4 remounts read-only),
#    so the create syscall fails; the error goes back to the caller, the name
#    is not served as present, and the daemon (this is the checking build in the
#    small tier: an invariant broken by the error path aborts it) goes on
#    serving. The mutation's phase 1 left the parent in the dirty set: after
#    the disk is healed and dcfs restarted over the remounted filesystem,
#    recovery names them and everything served matches the backing.
#
# Run as /tests/fault_backing.sh by guest/init when booted with
# dcfs_test=fault_backing.sh.
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

DAEMON_PID=""
MOUNTED=0

cleanup() {
	rc=$?
	if [ "$rc" -ne 0 ] || [ "$FAILED" -ne 0 ]; then
		for log in "$LOG1" "$LOG2"; do
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

echo "fault_backing.sh: kernel $(uname -r)"
require_commands umount sync

alive() {
	[ -n "$DAEMON_PID" ] && kill -0 "$DAEMON_PID" 2>/dev/null
}

fd_setup || {
	fail setup "wrapping or mounting the disks failed"
	exit "$FAILED"
}
mkdir "$SRC/d1" "$SRC/d2"
echo other >"$SRC/d2/other"
# Thousands of files first, so that the one the cold lookup needs is made last
# and its inode lies in metadata the mount (and dcfs's start) has not read: on
# btrfs a small tree is one leaf, which a mount reads whole.
mkdir "$SRC/pad"
/bin/dcfs_bench mktree "$SRC/pad" 6000 0 >/dev/null 2>&1 || fail pad "could not make the padding tree"
echo known >"$SRC/d1/known"
sync
# Mount the backing filesystem afresh, so that its own caches hold nothing of
# what the cold lookup below must read from the disk.
umount "$SRC"
mount "$(fault_dev "$FD_BACK")" "$SRC" || {
	fail remount-backing "cannot mount the backing filesystem"
	exit "$FAILED"
}

if fd_start "$LOG1"; then
	pass mount
else
	fail mount "daemon did not mount within 10s"
	exit "$FAILED"
fi
ls "$MNT" >/dev/null

# --- 1. a backing read error during a cold LOOKUP ----------------------------

drop_caches_quiesced
fault_mode "$FD_BACK" error-reads || fail inject-read-errors "fault_mode failed"
drop_caches
out=$(stat -c %s "$MNT/d1/known" 2>&1)
rc=$?
if [ "$rc" -ne 0 ]; then
	pass lookup-read-error-replied
else
	fail lookup-read-error-replied "stat succeeded ($out) with every backing read failing"
fi
if alive; then
	pass lookup-read-error-daemon-alive
else
	fail lookup-read-error-daemon-alive "the daemon died"
fi
fault_mode "$FD_BACK" healthy || fail heal-read-errors "fault_mode failed"
drop_caches
out=$(stat -c %s "$MNT/d1/known" 2>&1)
if [ "$out" = 6 ]; then
	pass lookup-after-heal-present
else
	fail lookup-after-heal-present "stat: $out (want the size, 6): the failed lookup was recorded"
fi
if [ "$(ls "$MNT/d1" 2>&1)" = known ]; then
	pass listing-after-heal
else
	fail listing-after-heal "ls: $(ls "$MNT/d1" 2>&1)"
fi
if alive; then pass daemon-alive-after-read-error; else fail daemon-alive-after-read-error "the daemon died"; fi

# --- 2. a backing write error during a CREATE's phase 2 -----------------------

ls "$MNT/d2" >/dev/null
fault_mode "$FD_BACK" error-writes || fail inject-write-errors "fault_mode failed"
# The journal meets the errors at its next commit: a change and a sync, as
# fault_selftest.sh shows; after that the filesystem is aborted.
echo trigger >"$SRC/trigger" 2>/dev/null || true
sync
sleep 1
out=$(touch "$MNT/d2/new" 2>&1)
rc=$?
if [ "$rc" -ne 0 ]; then
	pass create-error-replied
else
	fail create-error-replied "touch succeeded on a backing filesystem whose journal aborted"
fi
echo "fault_backing.sh: create said: $out"
if alive; then
	pass create-error-daemon-alive
else
	fail create-error-daemon-alive "the daemon died on the failed create"
fi
if [ ! -e "$MNT/d2/new" ]; then
	pass create-error-name-not-present
else
	fail create-error-name-not-present "d2/new is served as present"
fi
# What dcfs lists is what the failed backing filesystem lists, whatever that
# is (an error on xfs after its shutdown, nothing on btrfs after its
# transaction aborted), and never the name whose create failed.
via_backing=$(ls "$SRC/d2" 2>&1)
via_dcfs=$(ls "$MNT/d2" 2>&1)
echo "fault_backing.sh: after the failed create, d2 on the backing filesystem: $via_backing; through dcfs: $via_dcfs"
case "$via_dcfs" in
*new*) fail create-error-listing "ls d2 through dcfs shows the failed create: $via_dcfs" ;;
*) pass create-error-listing ;;
esac
if alive; then pass daemon-alive-after-write-error; else fail daemon-alive-after-write-error "the daemon died"; fi

# The disk is healed, and the daemon restarted over the remounted backing
# filesystem: phase 1's dirty rows are recovered, nothing served is wrong.
fd_crash
fault_mode "$FD_BACK" healthy || fail heal-write-errors "fault_mode failed"
fd_umount_disks
mount "$(fault_dev "$FD_BACK")" "$SRC" || fail remount-backing "cannot mount the healed backing filesystem"
mount "$(fault_dev "$FD_CACHE")" "$CACHE_DIR" || fail remount-cache "cannot mount the cache filesystem"
if fd_start "$LOG2"; then
	pass restart
else
	fail restart "daemon did not mount within 10s"
	exit "$FAILED"
fi
if [ "$(fd_recovered "$LOG2")" -ge 1 ]; then
	pass restart-recovers-dirty
else
	fail restart-recovers-dirty "no recovered dirty rows: $(grep -i 'cleanly' "$LOG2")"
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
require_no_reclaim no-reclaim
exit "$FAILED"
