#!/bin/sh
# dcfs step 23.7 (review M2): the cost of DESTROY's reconciliation.
#
# At unmount the kernel sends no FORGETs, so DESTROY reconciles every file
# written during the run that the kernel still caches (design.md, "mmap
# after close"): one statx through the descriptor dcfs holds on each, which
# touches no disk, before FinishRun's sync point. This writes ENTRIES files
# through dcfs (100000 by default), checks dcfs holds about that many
# descriptors, stops it with SIGTERM, and requires the shutdown to finish
# within BOUND_SECS and to be recorded clean (the next start recovers
# nothing). The time and the descriptor count are printed.
#
# Before the SIGTERM, a holder keeps every written file's dcfs inode (so
# the kernel sends no FORGET and DESTROY has them all to reconcile) and
# every cache is dropped, which leaves the backing inodes in memory only
# through dcfs's held descriptors. The shutdown must then read nothing from
# the backing device (review L-d): the no-disk claim itself, not only the
# time.
#
# The guest must hold all of this in memory: 100000 pinned inodes (dcfs's
# and the backing filesystem's, with their dentries) and the written files'
# page cache cost about 9 KiB each, and once MemFree reaches the kernel's
# low watermark kswapd evicts the inodes nothing pins yet (all of them
# while the files are being written), the kernel sends a FORGET for each,
# and dcfs drops that file's held descriptor (measured 2026-10-07: no
# reclaim until MemFree fell to 60 MB at 73,418 files, then the held count
# fell as kswapd scanned). MemAvailable does not show it (it counts the
# reclaimable caches as available), so the test reads the reclaim counters
# and fails (require_no_reclaim), saying so, when the guest was too small.
#
# Run as /tests/destroy.sh by guest/init when booted with dcfs_test=destroy.sh.
FAILED=0
. "$(dirname "$0")/lib.sh"

DCFS=/bin/dcfs
BENCH=/bin/dcfs_bench

SRC=/src
MNT=/mnt
DB=/cache/dcfs.db
LOG1=/tmp/dcfs-1.log
LOG2=/tmp/dcfs-2.log
ENTRIES=${ENTRIES:-100000}
BOUND_SECS=${BOUND_SECS:-60}

DAEMON_PID=""
HOLD_PID=""
MOUNTED=0
TESTUTIL=/bin/testutil
DEV=vdb

cleanup() {
	rc=$?
	if [ "$rc" -ne 0 ] || [ "$FAILED" -ne 0 ]; then
		echo "--- dcfs stderr ---"
		tail -50 "$LOG1" 2>/dev/null
		tail -50 "$LOG2" 2>/dev/null
	fi
	if [ -n "$HOLD_PID" ]; then
		kill "$HOLD_PID" 2>/dev/null || true
		wait "$HOLD_PID" 2>/dev/null || true
	fi
	if is_mounted "$MNT"; then
		umount "$MNT" 2>/dev/null || umount -l "$MNT" 2>/dev/null || true
	fi
	if [ -n "$DAEMON_PID" ] && kill -0 "$DAEMON_PID" 2>/dev/null; then
		kill "$DAEMON_PID" 2>/dev/null || true
		wait "$DAEMON_PID" 2>/dev/null || true
	fi
}
trap cleanup EXIT

# uptime_ms: milliseconds since boot (busybox date has no %N).
uptime_ms() {
	read -r up _ </proc/uptime
	echo "${up%.*}${up#*.}0"
}

echo "destroy.sh: kernel $(uname -r), ENTRIES=$ENTRIES BOUND_SECS=$BOUND_SECS"
mount /dev/vdb /src
mkdir -p /cache /mnt
if start_daemon "$LOG1"; then
	pass mount
else
	fail mount "daemon did not mount within 10s"
	exit "$FAILED"
fi

t0=$(uptime_ms)
if "$BENCH" mktree "$MNT" "$ENTRIES" 0; then
	pass mktree
else
	fail mktree "could not write the tree through dcfs"
	exit "$FAILED"
fi
t1=$(uptime_ms)
fds=$(ls /proc/"$DAEMON_PID"/fd | wc -l)
echo "destroy.sh: wrote $ENTRIES files in $(((t1 - t0) / 1000)) s; dcfs holds $fds descriptors"
if [ "$fds" -ge "$ENTRIES" ]; then
	pass held-descriptors
else
	fail held-descriptors "dcfs holds $fds descriptors for $ENTRIES written files (MemFree $(awk '/^MemFree:/ {print $2}' /proc/meminfo) KiB,)"
fi

# Keep every written file's dcfs inode, then drop every cache.
"$TESTUTIL" opath-hold-tree "$MNT" >/tmp/hold.out 2>&1 &
HOLD_PID=$!
i=0
while [ "$i" -lt 300 ] && ! grep -q READY /tmp/hold.out; do
	if ! kill -0 "$HOLD_PID" 2>/dev/null; then
		break
	fi
	i=$((i + 1))
	sleep 1
done
# mktree writes ENTRIES files and the one at the bottom of deep/.
if grep -qx "READY $((ENTRIES + 1))" /tmp/hold.out; then
	pass hold-tree
	echo "destroy.sh: holder: $(cat /tmp/hold.out)"
else
	fail hold-tree "holder not ready: $(cat /tmp/hold.out)"
	exit "$FAILED"
fi
fds_held=$(daemon_fd_count)
echo "destroy.sh: dcfs holds $fds_held descriptors with the holder ready; MemFree $(awk '/^MemFree:/ {print $2}' /proc/meminfo) KiB"
require_no_reclaim no-reclaim
drop_caches_quiesced
fds_dropped=$(daemon_fd_count)
echo "destroy.sh: after dropping the caches dcfs holds $fds_dropped descriptors"
# Every inode is pinned by the holder, so the drop must forget none.
if [ "$fds_dropped" -eq "$fds_held" ]; then
	pass still-held
else
	fail still-held "dcfs held $fds_held descriptors and $fds_dropped after drop_caches (FORGETs came despite the holder)"
fi

r0=$(sectors_read "$DEV")
t2=$(uptime_ms)
kill -TERM "$DAEMON_PID"
wait "$DAEMON_PID"
rc=$?
t3=$(uptime_ms)
r1=$(sectors_read "$DEV")
DAEMON_PID=""
kill "$HOLD_PID" 2>/dev/null || true
wait "$HOLD_PID" 2>/dev/null || true
HOLD_PID=""
echo "destroy.sh: sectors read from $DEV between SIGTERM and exit: $((r1 - r0))"
if [ "$r1" -eq "$r0" ]; then
	pass shutdown-reads-nothing
else
	fail shutdown-reads-nothing "$((r1 - r0)) sectors read from $DEV between SIGTERM and exit"
fi
ms=$((t3 - t2))
echo "destroy.sh: SIGTERM to exit: $ms ms for $ENTRIES written files"
if [ "$rc" -eq 0 ]; then
	pass clean-exit
else
	fail clean-exit "dcfs exited $rc"
fi
if [ "$ms" -le $((BOUND_SECS * 1000)) ]; then
	pass shutdown-bounded
else
	fail shutdown-bounded "shutdown took $ms ms (bound ${BOUND_SECS}s)"
fi

if start_daemon "$LOG2"; then
	if grep -q "did not shut down cleanly" "$LOG2"; then
		fail recorded-clean "$(grep "did not shut down cleanly" "$LOG2")"
	else
		pass recorded-clean
	fi
else
	fail recorded-clean "daemon did not start again"
fi

exit "$FAILED"
