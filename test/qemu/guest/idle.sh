#!/bin/sh
# dcfs phase 10: the idle test. With dcfs mounted on a warm cache and only
# the activity a quiet server sees (statfs, as df does it -- busybox has no
# df, `stat -f` makes the same call --, stat and ls of cached paths, dcfs's
# own timers, sync points and database checkpoints), the backing device's
# I/O counters (/proc/diskstats: reads and writes completed) must
# not move for IDLE_SECS seconds.
#
# "Warm" means dcfs's database has every entry (a `find` through the
# mount); the kernel's caches (page, dentry, inode, FUSE) are then dropped
# so that everything the quiet server does afterwards has to be answered
# from dcfs's cache, and the counters are sampled right after the drop: the
# very first stat after a drop counts too. The backing filesystem's own
# writeback is out of the picture because nothing is dirty: the tree is
# synced before dcfs mounts, and the activity below never writes.
#
# The warm-up also quiesces the backing filesystem (drop_caches_quiesced,
# lib.sh). `sync` alone leaves xfs two timer ticks of log covering to do:
# xfs_log_worker runs every fs.xfs.xfssyncd_centisecs (30 s) and, when the
# log is idle but not yet "covered", commits a dummy superblock
# transaction (xfs_log_cover -> xfs_sync_sb) and forces the log; it takes
# two such transactions to cover the log, so without the quiesce the window
# sees +2 writes at about 30 s and +2 at about 60 s after the mount, then
# nothing more (measured in the guest, 240 s window, with and without dcfs
# running: Phase 6.2 log). That is xfs settling after the tree was written,
# not dcfs I/O, and a freeze/thaw does the covering synchronously.
#
# idle_short.sh and idle_long.sh set IDLE_SECS (medium and large tiers).
# Not covered: an NFS client idling on an export (needs the Debian rootfs
# of nfs_test; see docs/plan/phases/10-benchmarks.md).
FAILED=0
. "$(dirname "$0")/lib.sh"

DCFS=/bin/dcfs
BENCH=/bin/dcfs_bench
SRC=/src
MNT=/mnt
DB=/cache/dcfs.db
LOG=/tmp/dcfs.log
DEV=vdb

IDLE_SECS=${IDLE_SECS:-60}
ENTRIES=${ENTRIES:-2000}
BIG=${BIG:-500}

DAEMON_PID=""
MOUNTED=0

cleanup() {
	rc=$?
	if [ "$rc" -ne 0 ] || [ "$FAILED" -ne 0 ]; then
		echo "--- dcfs stderr ---"
		cat "$LOG" 2>/dev/null
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

# io_counts: "<reads completed> <writes completed>" of $DEV.
io_counts() {
	while read -r _maj _min name rd _rdm _rds _rdt wr _rest; do
		if [ "$name" = "$DEV" ]; then
			echo "$rd $wr"
			return
		fi
	done </proc/diskstats
}

# One second's worth of what a quiet server sees.
quiet_activity() {
	stat -f "$MNT" >/dev/null || return 1
	stat "$MNT" "$MNT/t/000/00/f00" "$MNT/t/000/00/f01" \
		"$MNT/deep/l1/l2/l3/l4/l5/l6/leaf" >/dev/null || return 1
	ls "$MNT/t/000/00" "$MNT/big" >/dev/null || return 1
}

echo "idle.sh: kernel $(uname -r), IDLE_SECS=$IDLE_SECS ENTRIES=$ENTRIES"

mount /dev/$DEV $SRC
"$BENCH" mktree "$SRC" "$ENTRIES" "$BIG" || {
	fail mktree "could not make the tree"
	exit 1
}
sync

mkdir -p /cache /mnt
if start_daemon "$LOG"; then
	pass mount
else
	fail mount "daemon did not mount within 10s"
	exit "$FAILED"
fi

find "$MNT" >/dev/null
pass warm-find

drop_caches_quiesced
sleep 1
set -- $(io_counts)
r0=$1
w0=$2
echo "idle.sh: counters after warm-up: reads=$r0 writes=$w0"

elapsed=0
while [ "$elapsed" -lt "$IDLE_SECS" ]; do
	if ! quiet_activity; then
		fail quiet-activity "a quiet-server command failed after ${elapsed}s"
		break
	fi
	sleep 1
	elapsed=$((elapsed + 1))
done

set -- $(io_counts)
r1=$1
w1=$2
echo "idle.sh: counters after ${elapsed}s: reads=$r1 writes=$w1"
if [ "$r1" -eq "$r0" ] && [ "$w1" -eq "$w0" ]; then
	pass backing-idle
else
	fail backing-idle "backing device I/O during ${elapsed}s idle: reads +$((r1 - r0)), writes +$((w1 - w0))"
fi

if kill -0 "$DAEMON_PID" 2>/dev/null; then
	pass daemon-still-alive
else
	fail daemon-still-alive "daemon exited"
fi

# The counters must be able to move at all: with the caches dropped again,
# reading the backing directory directly has to cost disk reads. (Guards
# against a test that passes because /proc/diskstats is not watching the
# right device or everything is pinned in memory.)
drop_caches
set -- $(io_counts)
before_direct=$1
ls -l "$SRC/t/000/00" "$SRC/big" >/dev/null
set -- $(io_counts)
if [ "$1" -gt "$before_direct" ]; then
	pass counters-live
else
	fail counters-live "a cold direct read of the backing directory did not move the read counter"
fi

exit "$FAILED"
