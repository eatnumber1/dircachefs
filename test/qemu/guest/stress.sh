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
# its own model (data, size and mmap correctness). After them, the tree seen
# through dcfs digests like the backing filesystem's own (stress_lib.sh,
# stress_digest: names, types, modes, link counts, owners, sizes, symlink
# targets, xattrs, mtime and ctime, file contents; what it leaves out is
# listed there), three times: right after the run (the kernel's FUSE caches,
# with their one-hour timeouts, answer most of it), after drop_caches with
# the same daemon running (dcfs's own cache answers), and after a restart
# (recovery, then lookups the new daemon makes cold). In the small and
# medium tiers the daemon is the checking build, which aborts naming the
# invariant it saw broken: the daemon's survival is a check too.
#
# Not proved: fsstress exits 0 whatever its operations returned, so the test
# prints the errno of every operation and requires successes of the main ones
# and no EIO; the operations a FUSE mount cannot do are listed in README.md
# ("Step 11.2b").
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

# cmdline NAME: the value of NAME=VALUE on the kernel command line.
cmdline() { sed -n "s/.*\b$1=\([^ ]*\).*/\1/p" /proc/cmdline; }

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
	# Sizes and seeds can be set on the kernel command line (the target's
	# `cmdline =`), e.g. to replay a failure with its printed seeds.
	OPS=$(cmdline stress_ops)
	OPS=${OPS:-5000}
	FSX_OPS=$(cmdline stress_fsx_ops)
	FSX_OPS=${FSX_OPS:-50000}
	FSX_LEN=1048576
	FSSTRESS_SEED=$(cmdline stress_seed)
	FSSTRESS_SEED=${FSSTRESS_SEED:-$(stress_seed)}
	FSX_SEED=$(cmdline stress_fsx_seed)
	FSX_SEED=${FSX_SEED:-$(stress_seed)}
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
# The four operations that are XFS ioctls (bulkstat, bulkstat1, resvsp,
# unresvsp) have no meaning for a FUSE file system: no ioctl reaches the
# backing filesystem, they would only count failures.
t0=$(date +%s)
"$FSSTRESS" -d "$MNT/fsstress" -p "$PROCS" -n "$OPS" -s "$FSSTRESS_SEED" -v \
	-f bulkstat=0 -f bulkstat1=0 -f resvsp=0 -f unresvsp=0 \
	>/tmp/fsstress.out 2>/tmp/fsstress.err
rc=$?
t1=$(date +%s)
echo "stress.sh: fsstress exit $rc in $((t1 - t0))s; every operation by errno (op errno count; errno 0 succeeded):"
fsstress_results /tmp/fsstress.out
if [ "$rc" -eq 0 ]; then
	pass fsstress-exit
else
	fail fsstress-exit "exit $rc"
	head -20 /tmp/fsstress.err
fi
# Of the PROCS*OPS operations, 0.2% at least succeed as each of the main
# kinds (creat, mkdir, link, symlink, rename, unlink, write).
fsstress_verdict fsstress-operations-succeeded /tmp/fsstress.out $((PROCS * OPS / 500))
if alive; then pass fsstress-daemon-alive; else fail fsstress-daemon-alive "the daemon died"; fi

stress_same_tree fsstress-kernel-cache-matches-backing "$MNT/fsstress" "$SRC/fsstress"

# --- fsx ---------------------------------------------------------------------

t0=$(date +%s)
"$FSX" -N "$FSX_OPS" -S "$FSX_SEED" -l "$FSX_LEN" -P /tmp "$MNT/fsx.dat" >/tmp/fsx.out 2>&1
rc=$?
t1=$(date +%s)
echo "stress.sh: fsx exit $rc in $((t1 - t0))s: $(tail -1 /tmp/fsx.out)"
echo "stress.sh: fsx disabled:"
fsx_disabled /tmp/fsx.out
fsx_verdict fsx-completed /tmp/fsx.out "$FSX_OPS"
# What dcfs lacks today, measured on ext4, xfs and btrfs alike: fsx turns a
# feature off when its first use fails with an error that says "not here".
# No ioctl (clone and dedupe range, which fsx also finds the backing
# filesystems have), no RWF_DONTCACHE, and only the fallocate modes dcfs
# forwards (keep size, punch hole and zero range work; collapse, insert,
# unshare and write-zeroes do not); atomic writes need O_DIRECT, which fsx
# only uses with -Z. An entry leaving this list means dcfs gained the feature
# and fsx now tests it.
cat >/tmp/fsx-expected.txt <<'LIST'
atomic writes need O_DIRECT (-Z)
filesystem does not support clone range
filesystem does not support dedupe range
filesystem does not support dontcache IO
filesystem does not support fallocate mode FALLOC_FL_COLLAPSE_RANGE
filesystem does not support fallocate mode FALLOC_FL_INSERT_RANGE
filesystem does not support fallocate mode FALLOC_FL_UNSHARE_RANGE
filesystem does not support fallocate mode FALLOC_FL_WRITE_ZEROES
LIST
fsx_disabled_verdict fsx-disabled-features /tmp/fsx.out /tmp/fsx-expected.txt
if alive; then pass fsx-daemon-alive; else fail fsx-daemon-alive "the daemon died"; fi
if [ "$(md5sum <"$MNT/fsx.dat")" = "$(md5sum <"$SRC/fsx.dat")" ] &&
	[ "$(stat -c %s "$MNT/fsx.dat")" = "$(stat -c %s "$SRC/fsx.dat")" ]; then
	pass fsx-file-matches-backing
else
	fail fsx-file-matches-backing "$MNT/fsx.dat differs from the backing filesystem's"
fi

# --- the whole tree again, warm and after a restart --------------------------

stress_same_tree both-kernel-cache-matches-backing "$MNT" "$SRC"
sync

# The same daemon, the kernel's caches dropped: dcfs's own cache (its
# database) answers now.
drop_caches_quiesced
stress_same_tree both-dcfs-cache-matches-backing "$MNT" "$SRC"

# A restart: recovery of the dirty set (a clean stop leaves it empty), then
# the same comparison over what the new daemon serves.
if restart_daemon restart "$LOG2"; then
	stress_same_tree restart-matches-backing "$MNT" "$SRC"
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
