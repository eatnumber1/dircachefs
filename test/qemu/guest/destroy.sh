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
MOUNTED=0

cleanup() {
	rc=$?
	if [ "$rc" -ne 0 ] || [ "$FAILED" -ne 0 ]; then
		echo "--- dcfs stderr ---"
		tail -50 "$LOG1" 2>/dev/null
		tail -50 "$LOG2" 2>/dev/null
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
if [ "$fds" -ge $((ENTRIES * 9 / 10)) ]; then
	pass held-descriptors
else
	fail held-descriptors "dcfs holds $fds descriptors for $ENTRIES written files"
fi

t2=$(uptime_ms)
kill -TERM "$DAEMON_PID"
wait "$DAEMON_PID"
rc=$?
t3=$(uptime_ms)
DAEMON_PID=""
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
