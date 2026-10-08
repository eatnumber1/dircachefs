#!/bin/sh
# dcfs step 22.1: the cancellation inventory's measurements. How long each
# request path that can wait on the backing filesystem takes when every
# backing I/O is slow: the backing filesystem is on a device-mapper delay
# target (every read and write takes DELAY_MS, a spinning disk's seek), the
# cache database on a fast disk. Each line "inventory: NAME MS ms" is the
# wall time of one operation through dcfs, the kernel's and the backing
# filesystem's caches dropped first where the line says "cold". The verdict
# is only that every step ran: VM timing is too noisy to pass or fail on.
#
# A spin-up (the first I/O to a sleeping disk: seconds) is not something a
# delay target can make; it adds once to the first cold line after idling.
#
# Run as /tests/cancel_inventory.sh by guest/init when booted with
# dcfs_test=cancel_inventory.sh.
FAILED=0
. "$(dirname "$0")/lib.sh"

DCFS=/bin/dcfs
BENCH=/bin/dcfs_bench
DELAY_MS=${DELAY_MS:-10}
ENTRIES=${ENTRIES:-2000}
BIG=${BIG:-20000}
FILES=${FILES:-500}

SRC=/src
MNT=/mnt
DB=/cache/dcfs.db
LOG=/tmp/dcfs-inventory.log
DAEMON_PID=""
MOUNTED=0

cleanup() {
	rc=$?
	if [ "$rc" -ne 0 ] || [ "$FAILED" -ne 0 ]; then
		echo "--- dcfs stderr ---"
		tail -50 "$LOG" 2>/dev/null
	fi
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

# timed NAME CMD...: runs CMD, prints its wall time.
timed() {
	name=$1
	shift
	t0=$(uptime_ms)
	if "$@" >/dev/null 2>&1; then
		t1=$(uptime_ms)
		echo "inventory: $name $((t1 - t0)) ms"
		pass "$name"
	else
		fail "$name" "$* failed"
	fi
}

# start_timed NAME: starts dcfs and prints the time until it is mounted
# (polled every 100 ms: start_daemon polls every second).
start_timed() {
	t0=$(uptime_ms)
	"${MOUNT_DCFS:-/sbin/mount.dcfs}" \
		-o "$(dcfs_options --sync_interval_sec=1000000)" "$SRC" "$MNT" >>"$LOG" 2>&1 &
	DAEMON_PID=$!
	i=0
	while [ "$i" -lt 3000 ]; do
		if is_mounted "$MNT"; then
			t1=$(uptime_ms)
			MOUNTED=1
			echo "inventory: $1 $((t1 - t0)) ms"
			pass "$1"
			return 0
		fi
		if ! kill -0 "$DAEMON_PID" 2>/dev/null; then
			break
		fi
		i=$((i + 1))
		sleep 0.1
	done
	fail "$1" "dcfs did not mount"
	return 1
}

echo "cancel_inventory.sh: kernel $(uname -r), DELAY_MS=$DELAY_MS ENTRIES=$ENTRIES BIG=$BIG FILES=$FILES"
mkdir -p /prep "$SRC" "$MNT" /cache
mount /dev/vdb /cache || {
	fail mount-cache "could not mount /dev/vdb"
	exit 1
}
# The tree is made at full speed, then seen through the delay target.
mount /dev/vdc /prep || {
	fail mount-prep "could not mount /dev/vdc"
	exit 1
}
"$BENCH" mktree /prep "$ENTRIES" "$BIG" || {
	fail mktree "could not make the tree"
	exit 1
}
mkdir -p /prep/w
umount /prep
slow_dev=$("$BENCH" dm-delay slow /dev/vdc "$DELAY_MS") || {
	fail dm-delay "could not create the delay device"
	exit 1
}
mount "$slow_dev" "$SRC" || {
	fail mount-slow "could not mount $slow_dev"
	exit 1
}
drop_caches

# Start-up with a new cache (StartRun, InitRoot: the root's probe).
start_timed start-new-cache || exit 1

# LOOKUP of a name in an uncached directory three levels down: each level
# is populated (getdents64, then a probe of every name: openat O_PATH,
# statx, name_to_handle_at, generation, listxattr), 100 entries at the last.
drop_caches
timed cold-lookup-depth3 stat "$MNT/t/000/00/f00"
# The same directory's neighbour: answered from the cache.
drop_caches
timed warm-lookup stat "$MNT/t/000/00/f01"
# READDIR of a BIG-entry directory never listed: one population (find
# reads the names only: getdents64, no stat).
drop_caches
timed cold-populate-big find "$MNT/big" -maxdepth 1 -name nomatch
# For comparison, the backing filesystem's own share: the same names and a
# stat of each, straight on the (delayed) backing filesystem, cold.
drop_caches
timed backing-only-cold-ls-l-big ls -l "$SRC/big"
# Listed again with every entry's attributes (READDIRPLUS, then a stat of
# each), from the cache (the kernel's caches dropped).
drop_caches
timed warm-readdirplus-stat-big ls -l "$MNT/big"
# OPEN (and read) of a file whose backing inode is not cached.
drop_caches
timed cold-open-read cat "$MNT/t/000/01/f02"
# CREATE, UNLINK, RENAME in a cached directory, backing caches dropped.
ls "$MNT/t/000/02" >/dev/null
drop_caches
timed create sh -c ": >$MNT/t/000/02/new"
drop_caches
timed rename mv "$MNT/t/000/02/new" "$MNT/t/000/02/renamed"
drop_caches
timed unlink rm "$MNT/t/000/02/renamed"
# Writes go through passthrough (the kernel); FSYNC is the backing fsync
# and the sync point after it (syncfs of everything written since).
timed write-32m dd if=/dev/zero of="$MNT/w/a" bs=1M count=32
timed fsync-after-32m dd if=/dev/zero of="$MNT/w/b" bs=4096 count=1 conv=fsync
timed write-fsync-32m dd if=/dev/zero of="$MNT/w/c" bs=1M count=32 conv=fsync

# DESTROY with FILES written files to reconcile, then FinishRun's sync point
# and checkpoint: SIGTERM to exit.
mkdir -p "$MNT/w/many"
i=0
while [ "$i" -lt "$FILES" ]; do
	echo x >"$MNT/w/many/f$i"
	i=$((i + 1))
done
# (RELEASE is asynchronous: give the last ones time to arrive, or the
# writable opens still outstanding make the shutdown unclean.)
sleep 2
t0=$(uptime_ms)
kill -TERM "$DAEMON_PID"
wait "$DAEMON_PID"
t1=$(uptime_ms)
DAEMON_PID=""
echo "inventory: destroy-finishrun-$FILES $((t1 - t0)) ms"
pass destroy
umount "$MNT" 2>/dev/null || true

# Start-up after a clean shutdown (nothing to recover), cold.
drop_caches
start_timed start-clean || exit 1

# Start-up after a crash with FILES creations unsynced (the dirty set):
# RecoverDirty, the sweep, and 12.4b's probe of every recovered inode by
# handle, backing caches cold (a power loss's case; a daemon crash's has
# them warm).
mkdir -p "$MNT/w/dirty"
i=0
while [ "$i" -lt "$FILES" ]; do
	: >"$MNT/w/dirty/f$i"
	i=$((i + 1))
done
kill -KILL "$DAEMON_PID"
wait "$DAEMON_PID" 2>/dev/null || true
DAEMON_PID=""
umount "$MNT" 2>/dev/null || umount -l "$MNT" 2>/dev/null || true
drop_caches
start_timed start-after-crash-$FILES-dirty || exit 1
grep -E "recovered|forgot" "$LOG" | tail -3

exit "$FAILED"
