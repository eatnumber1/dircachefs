#!/bin/sh
# dcfs step 11.2b: fsstress and fsx, built from xfstests' sources
# (//third_party/xfstests), run against dcfs on the backing filesystem of the
# matrix (ext4, xfs or btrfs).
#
# fsstress runs random concurrent namespace and data operations (create,
# mkdir, link, symlink, rename and its flags, unlink, rmdir, mknod, write,
# truncate, fallocate, punch, mmap, xattrs, ...) from several processes
# under $MNT/fsstress. fsx runs random reads, writes, truncates and mmap
# operations on one file under $MNT, checking every byte it reads against
# its own model (data, size and mmap correctness). After each,
# the cache is compared with the backing filesystem: the tree seen through
# dcfs (from its cache, warm) digests like the backing filesystem's own
# (every name, type, mode, link count, owner, size, symlink target, xattr and
# the md5 of every file); then dcfs restarts and the comparison is made again
# over what recovery and the next lookups served. In the small and medium
# tiers the daemon is the checking build, which aborts naming the invariant
# it saw broken: the daemon's survival is a check too.
#
# The wrappers set the run's size: stress_short.sh, stress_long.sh (fixed
# seeds) and stress_random.sh (a seed from the kernel's random pool,
# printed; the manual tier).
#
# Run as /tests/<wrapper>.sh by guest/init when booted with dcfs_test=<it>.
FAILED=0
. "$(dirname "$0")/lib.sh"
. "$(dirname "$0")/stress_lib.sh"

DCFS=/bin/dcfs
TESTUTIL=/bin/testutil
FSSTRESS=/bin/fsstress
FSX=/bin/fsx

SRC=/src
MNT=/mnt
DB=/cache/dcfs.db
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
	if [ "$MOUNTED" -eq 1 ]; then
		umount "$MNT" 2>/dev/null || umount -l "$MNT" 2>/dev/null || true
	fi
	if [ -n "$DAEMON_PID" ] && kill -0 "$DAEMON_PID" 2>/dev/null; then
		kill "$DAEMON_PID" 2>/dev/null || true
		wait "$DAEMON_PID" 2>/dev/null || true
	fi
}
trap cleanup EXIT

echo "stress.sh: kernel $(uname -r)"

case "${STRESS_MODE:-}" in
short | long | random) ;;
*)
	fail stress-mode "STRESS_MODE '${STRESS_MODE:-}' is not short, long or random (run a guest/stress_<mode>.sh wrapper)"
	exit "$FAILED"
	;;
esac

# The size of each mode: fsstress -p PROCS -n OPS, fsx -N FSX_OPS -l FSX_LEN.
case "$STRESS_MODE" in
short)
	PROCS=4
	OPS=250
	FSX_OPS=2500
	FSX_LEN=131072
	FSSTRESS_SEED=1
	FSX_SEED=1
	;;
long)
	PROCS=8
	OPS=1500
	FSX_OPS=20000
	FSX_LEN=1048576
	FSSTRESS_SEED=20260101
	FSX_SEED=20260101
	;;
random)
	PROCS=8
	OPS=${STRESS_OPS:-5000}
	FSX_OPS=${STRESS_FSX_OPS:-50000}
	FSX_LEN=1048576
	FSSTRESS_SEED=$(($(cut -c1-7 /proc/sys/kernel/random/uuid | sed 's/^/0x/')))
	FSX_SEED=$(($(cut -c9-15 /proc/sys/kernel/random/uuid | sed 's/^/0x/')))
	;;
esac
echo "stress.sh: mode $STRESS_MODE: fsstress -p $PROCS -n $OPS -s $FSSTRESS_SEED; fsx -N $FSX_OPS -l $FSX_LEN -S $FSX_SEED"

require_commands find md5sum sort
for tool in "$FSSTRESS" "$FSX" "$TESTUTIL"; do
	[ -x "$tool" ] || fail guest-tools "$tool is not in this guest"
done
[ "$FAILED" -eq 0 ] || exit "$FAILED"

mount /dev/vdb "$SRC"
mkdir -p /cache "$MNT"
FSTYPE=$(backing_fstype "$SRC")
echo "stress.sh: backing filesystem $FSTYPE"

if start_daemon "$LOG1"; then
	pass mount
else
	fail mount "daemon did not mount within 10s"
	exit "$FAILED"
fi

alive() {
	[ -n "$DAEMON_PID" ] && kill -0 "$DAEMON_PID" 2>/dev/null
}

# --- fsstress ----------------------------------------------------------------

mkdir "$MNT/fsstress"
# The five operations that are XFS ioctls (bulkstat, resvsp) have no meaning
# for a FUSE file system: no ioctl reaches the backing filesystem, they would
# only count failures.
t0=$(date +%s)
"$FSSTRESS" -d "$MNT/fsstress" -p "$PROCS" -n "$OPS" -s "$FSSTRESS_SEED" -v \
	-f bulkstat=0 -f bulkstat1=0 -f resvsp=0 -f unresvsp=0 \
	>/tmp/fsstress.out 2>/tmp/fsstress.err
rc=$?
t1=$(date +%s)
echo "stress.sh: fsstress exit $rc in $((t1 - t0))s; operations (from its -v log):"
fsstress_ops /tmp/fsstress.out | tr '\n' ' '
echo
if [ "$rc" -eq 0 ]; then
	pass fsstress-exit
else
	fail fsstress-exit "exit $rc"
	head -20 /tmp/fsstress.err
fi
fsstress_verdict fsstress-ran /tmp/fsstress.out $((PROCS * OPS * 8 / 10))
if alive; then pass fsstress-daemon-alive; else fail fsstress-daemon-alive "the daemon died"; fi

stress_same_tree fsstress-cache-matches-backing "$MNT/fsstress" "$SRC/fsstress"

# --- fsx ---------------------------------------------------------------------

t0=$(date +%s)
"$FSX" -N "$FSX_OPS" -S "$FSX_SEED" -l "$FSX_LEN" -P /tmp "$MNT/fsx.dat" >/tmp/fsx.out 2>&1
rc=$?
t1=$(date +%s)
echo "stress.sh: fsx exit $rc in $((t1 - t0))s: $(tail -1 /tmp/fsx.out)"
fsx_verdict fsx-completed /tmp/fsx.out "$FSX_OPS"
if alive; then pass fsx-daemon-alive; else fail fsx-daemon-alive "the daemon died"; fi
if [ "$(md5sum <"$MNT/fsx.dat")" = "$(md5sum <"$SRC/fsx.dat")" ] &&
	[ "$(stat -c %s "$MNT/fsx.dat")" = "$(stat -c %s "$SRC/fsx.dat")" ]; then
	pass fsx-file-matches-backing
else
	fail fsx-file-matches-backing "$MNT/fsx.dat differs from the backing filesystem's"
fi

# --- the whole tree again, warm and after a restart --------------------------

stress_same_tree both-cache-matches-backing "$MNT" "$SRC"
sync

# A restart: recovery of the dirty set (a clean stop leaves it empty), then
# the same comparison over what the new daemon serves.
if restart_daemon restart "$LOG2"; then
	stress_same_tree restart-cache-matches-backing "$MNT" "$SRC"
fi
if alive; then pass daemon-alive-at-end; else fail daemon-alive-at-end "the daemon died"; fi

# The long and random runs write more file data than the guest has memory
# (page cache of 400+ MiB in 512 MiB, 1.5 M pages scanned): reclaim, with its
# FORGETs and evictions, is part of what they stress, and none of their checks
# depends on an entry staying resident. The short run fits and checks it.
if [ "$STRESS_MODE" = short ]; then
	require_no_reclaim no-reclaim
else
	echo "stress.sh: no-reclaim not checked in mode $STRESS_MODE (data outgrows memory by design)"
fi
exit "$FAILED"
