#!/bin/sh
# dcfs Phase 22: request cancellation (docs/design.md, "Cancellation").
#
# The backing filesystem is on a device-mapper delay target (every read and
# write takes DELAY_MS), with a BIG-entry directory never listed through
# dcfs: its population is one request of many seconds (cancel_inventory_test
# measured 13.6-19.5 s for 20000 entries at 10 ms before Phase 22). The
# kernel waits for dcfs's reply even for a SIGKILLed caller, so without
# checkpoints an interrupted `ls` waits for the whole population.
#
#   - `timeout -s INT 1 ls` of it returns within BOUND_MS of the signal;
#   - `kill -9` of a `find` blocked in another such directory (k/big, so
#     the first one's population, complete when checkpoints are missing,
#     cannot make it pass) returns within BOUND_MS;
#   - dcfs serves a stat elsewhere afterwards;
#   - a full listing then completes, and equals the backing directory's
#     (an interrupted population recorded nothing wrong).
#
# Run as /tests/cancel.sh by guest/init when booted with dcfs_test=cancel.sh.
FAILED=0
. "$(dirname "$0")/lib.sh"

DCFS=/bin/dcfs
BENCH=/bin/dcfs_bench
DELAY_MS=${DELAY_MS:-10}
BIG=${BIG:-20000}
# Under KVM the interrupted paths take 0.7-1.0 s. TCG runs the guest 2-12x
# slower (Phase 5.1 measured 2-9x), so the bound scales by 4 there: 8 s
# (1.2 s and 0.8 s measured under TCG on 2026-10-07, much of the wait being
# DELAY_MS per I/O, which TCG does not lengthen) still fails a run without
# checkpoints, whose wait is the rest of the population (12.1 s after the
# signal under KVM).
if [ "${DCFS_ACCEL:-kvm}" = tcg ]; then
	BOUND_MS=${BOUND_MS:-8000}
else
	BOUND_MS=${BOUND_MS:-2000}
fi

SRC=/src
MNT=/mnt
DB=/cache/dcfs.db
LOG=/tmp/dcfs-cancel.log
DAEMON_PID=""
MOUNTED=0
FIND_PID=""

cleanup() {
	rc=$?
	if [ "$rc" -ne 0 ] || [ "$FAILED" -ne 0 ]; then
		echo "--- dcfs stderr ---"
		tail -50 "$LOG" 2>/dev/null
	fi
	[ -n "$FIND_PID" ] && kill -9 "$FIND_PID" 2>/dev/null
	if is_mounted "$MNT"; then
		umount "$MNT" 2>/dev/null || umount -l "$MNT" 2>/dev/null || true
	fi
	if [ -n "$DAEMON_PID" ] && kill -0 "$DAEMON_PID" 2>/dev/null; then
		kill "$DAEMON_PID" 2>/dev/null || true
		wait "$DAEMON_PID" 2>/dev/null || true
	fi
	umount "$SRC" 2>/dev/null || true
}
trap cleanup EXIT

# uptime_ms: milliseconds since boot (busybox date has no %N), 10 ms steps.
uptime_ms() {
	read -r up _ </proc/uptime
	echo "${up%.*}${up#*.}0"
}

# within NAME MS: passes if MS <= BOUND_MS.
within() {
	echo "cancel.sh: $1 took $2 ms"
	if [ "$2" -le "$BOUND_MS" ]; then
		pass "$1"
	else
		fail "$1" "took $2 ms, more than $BOUND_MS"
	fi
}

echo "cancel.sh: kernel $(uname -r), accel ${DCFS_ACCEL:-kvm}, DELAY_MS=$DELAY_MS BIG=$BIG BOUND_MS=$BOUND_MS"
mkdir -p /prep "$SRC" "$MNT" /cache
mount /dev/vdb /cache || {
	fail mount-cache "could not mount /dev/vdb"
	exit 1
}
mount /dev/vdc /prep || {
	fail mount-prep "could not mount /dev/vdc"
	exit 1
}
mkdir -p /prep/k
"$BENCH" mktree /prep 200 "$BIG" && "$BENCH" mktree /prep/k 1 "$BIG" || {
	fail mktree "could not make the tree"
	exit 1
}
umount /prep
slow_dev=$("$BENCH" dm-delay slow /dev/vdc "$DELAY_MS") || {
	fail dm-delay "could not create the delay device"
	exit 1
}
mount "$slow_dev" "$SRC" || {
	fail mount-slow "could not mount $slow_dev"
	exit 1
}
if start_daemon "$LOG" --stderrthreshold=0 --v=2; then
	pass mount
else
	fail mount "dcfs did not mount"
	exit 1
fi

# SIGINT during the population: the signal comes at 1 s.
drop_caches
t0=$(uptime_ms)
timeout -s INT 1 ls "$MNT/big" >/dev/null 2>/tmp/ls.err
rc=$?
t1=$(uptime_ms)
echo "cancel.sh: ls exited $rc: $(cat /tmp/ls.err)"
if [ "$rc" -ne 0 ]; then
	pass ls-interrupted
else
	fail ls-interrupted "ls finished the listing within the 1 s timeout (rc 0)"
fi
within ls-sigint $((t1 - t0 - 1000))
# An interrupted request is no error of dcfs's, so it is not logged at any
# default level: --v=2 shows the request with its reply.
if grep -q " -> .*Interrupted before" "$LOG"; then
	pass checkpoint-logged
else
	fail checkpoint-logged "dcfs logged no interrupted request"
fi

# dcfs serves other requests afterwards.
t0=$(uptime_ms)
if stat "$MNT/t/000/00/f00" >/dev/null; then
	pass stat-after
else
	fail stat-after "stat of another directory's file failed"
fi
t1=$(uptime_ms)
echo "cancel.sh: stat elsewhere took $((t1 - t0)) ms"

# SIGKILL of a find blocked in another directory's population.
drop_caches
find "$MNT/k/big" >/dev/null 2>&1 &
FIND_PID=$!
sleep 1
if kill -0 "$FIND_PID" 2>/dev/null; then
	t0=$(uptime_ms)
	kill -9 "$FIND_PID"
	wait "$FIND_PID" 2>/dev/null
	t1=$(uptime_ms)
	FIND_PID=""
	within find-sigkill $((t1 - t0))
else
	fail find-sigkill "find finished within 1 s: nothing to interrupt"
fi

# Uninterrupted, the listing completes, and equals the backing directory's.
ls "$MNT/big" | sort >/tmp/mnt.txt
ls "$SRC/big" | sort >/tmp/src.txt
n=$(wc -l </tmp/mnt.txt)
if [ "$n" -eq "$BIG" ] && cmp -s /tmp/mnt.txt /tmp/src.txt; then
	pass full-listing
else
	fail full-listing "dcfs listed $n names; differs from the backing: $(cmp /tmp/mnt.txt /tmp/src.txt 2>&1)"
fi
# And again, from the cache.
drop_caches
ls "$MNT/big" | sort >/tmp/mnt2.txt
if cmp -s /tmp/mnt2.txt /tmp/src.txt; then
	pass cached-listing
else
	fail cached-listing "the cached listing differs from the backing"
fi

exit "$FAILED"
