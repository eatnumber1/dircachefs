#!/bin/sh
# dcfs step 11.2: bounded sequences of operations with a power cut after them,
# in the manner of ACE (the Crash Monkey / ACE bounded black-box crash tests):
# every sequence of up to two operations from a small set, on a small tree,
# with or without a persistence point (an fsync of the directory through
# dcfs) between and after them, then a power cut (both disks drop writes, as in
# guest/fault_power.sh: fault_lib.sh), a remount from what the disks hold, a
# restart of dcfs, and the checks:
#
#   served    everything dcfs serves is what the backing filesystem holds,
#             entry by entry (type, size, mode, listings): no "cache ahead" or
#             "cache behind", whichever operations the cut fell between;
#   durable   after an fsync of the directory through dcfs (FSYNCDIR, which
#             runs a sync point) the backing filesystem keeps everything done
#             before it, and loses what was done after it (nothing else
#             commits its journal within the run: the mount's commit interval is
#             long where the filesystem has one).
#
# Kinds, per operation O (and per ordered pair O1, O2):
#   one       O, cut                    served
#   pair      O1, O2, cut               served
#   persist   O, fsync, cut             served, durable (the backing filesystem
#                                       is what it was at the fsync)
#   split     O1, fsync, O2, cut        served, durable (O2 is lost)
#
# The tree is rebuilt, and the cache database removed, for every sequence, so
# each starts cold. A violation names the kind, the operations and the diff.
#
# Run as /tests/fault_ace.sh by guest/init when booted with
# dcfs_test=fault_ace.sh.
FAILED=0
. "$(dirname "$0")/lib.sh"
. "$(dirname "$0")/fault_lib.sh"
. "$(dirname "$0")/fault_dcfs_lib.sh"

DCFS=/bin/dcfs
TESTUTIL=/bin/testutil
SRC=/src
MNT=/mnt
DB=$CACHE_DIR/dcfs.db
LOG=/tmp/dcfs.log

DAEMON_PID=""
MOUNTED=0

# OPS: the operations, each a function ace_<name> on the tree under $MNT/t.
OPS="create mkdir unlink rename replace link xrename chmod append"
# ACE_OPS (the kernel command line): a subset, for a quick run.
CMD_OPS=$(sed -n 's/.*\bdcfs_ace_ops=\([^ ]*\).*/\1/p' /proc/cmdline | tr , ' ')
[ -n "$CMD_OPS" ] && OPS=$CMD_OPS

cleanup() {
	rc=$?
	if [ "$rc" -ne 0 ] || [ "$FAILED" -ne 0 ]; then
		echo "--- dcfs stderr (the last run) ---"
		cat "$LOG" 2>/dev/null
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

echo "fault_ace.sh: kernel $(uname -r), operations: $OPS"
require_commands umount sync find stat truncate

ace_create() { : >"$MNT/t/n"; }
ace_mkdir() { mkdir "$MNT/t/m"; }
ace_unlink() { rm "$MNT/t/a"; }
ace_rename() { mv "$MNT/t/a" "$MNT/t/r"; }
ace_replace() { mv "$MNT/t/a" "$MNT/t/b"; }
ace_link() { ln "$MNT/t/a" "$MNT/t/l"; }
ace_xrename() { mv "$MNT/t/a" "$MNT/t/e/a"; }
ace_chmod() { chmod 600 "$MNT/t/a"; }
ace_append() { echo more >>"$MNT/t/a"; }

snapshot() {
	(cd "$1" && find . -path ./lost+found -prune -o -print | sort |
		while read -r p; do stat -c '%n %F %s %a' "$p"; done)
}

# reset: the tree, directly on the backing filesystem, and no cache database.
reset() {
	rm -rf "$SRC/t"
	mkdir "$SRC/t" "$SRC/t/e"
	echo aaaa >"$SRC/t/a"
	echo bbbb >"$SRC/t/b"
	echo xxxx >"$SRC/t/e/x"
	sync
	rm -f "$CACHE_DIR"/dcfs.db*
}

# begin: a cold daemon over the tree.
begin() {
	reset
	if ! fd_start "$LOG" --sync_interval_sec=3600; then
		fail ace-start "daemon did not mount within 10s"
		exit "$FAILED"
	fi
	ls "$MNT/t" "$MNT/t/e" >/dev/null
}

# finish KIND SEQ [DURABLE-SNAPSHOT]: the cut, the remount, the restart, the
# checks. DURABLE-SNAPSHOT is a file with the backing filesystem's state at
# the fsync, which it must still have.
finish() {
	f_kind=$1
	f_seq=$2
	f_durable=${3:-}
	fd_cut || {
		fail ace-cut "fd_cut failed"
		exit "$FAILED"
	}
	fd_crash
	fd_restore || {
		fail ace-restore "remounting after the cut failed"
		exit "$FAILED"
	}
	if ! fd_start "$LOG" --sync_interval_sec=3600; then
		fail "ace-$f_kind-restart" "$f_seq: daemon did not mount within 10s after the cut"
		exit "$FAILED"
	fi
	drop_caches
	snapshot "$SRC" >/tmp/backing.snap
	snapshot "$MNT" >/tmp/served.snap 2>&1
	if ! command diff /tmp/served.snap /tmp/backing.snap >/tmp/snap.diff 2>&1; then
		ACE_BAD=$((ACE_BAD + 1))
		fail "ace-$f_kind" "$f_seq: served (<) and backing (>) differ: $(tr '\n' '|' </tmp/snap.diff)"
	fi
	if [ -n "$f_durable" ] && ! command diff "$f_durable" /tmp/backing.snap >/tmp/durable.diff 2>&1; then
		ACE_BAD=$((ACE_BAD + 1))
		fail "ace-$f_kind-durable" "$f_seq: the backing filesystem after the cut (>) is not what it was at the fsync (<): $(tr '\n' '|' </tmp/durable.diff)"
	fi
	fd_crash
	ACE_N=$((ACE_N + 1))
}

# persist: an fsync of the directory through dcfs, and the backing
# filesystem's state at that moment in /tmp/durable.snap.
persist() {
	"$TESTUTIL" fsync "$MNT/t" || fail ace-fsync "fsync of $MNT/t failed"
	snapshot "$SRC" >/tmp/durable.snap
}

fd_setup || {
	fail setup "wrapping or mounting the disks failed"
	exit "$FAILED"
}
FD_BACK_OPTS="-o commit=600"
fd_umount_disks
fd_mount_backing || {
	fail setup "mounting the backing filesystem with a long commit interval failed"
	exit "$FAILED"
}
mount "$(fault_dev "$FD_CACHE")" "$CACHE_DIR" || {
	fail setup "mounting the cache filesystem failed"
	exit "$FAILED"
}

run_kind() {
	ACE_N=0
	ACE_BAD=0
	"$@"
}

kind_one() {
	for o1 in $OPS; do
		begin
		"ace_$o1"
		finish one "$o1"
	done
}

kind_pair() {
	for o1 in $OPS; do
		for o2 in $OPS; do
			begin
			"ace_$o1"
			"ace_$o2" 2>/dev/null
			finish pair "$o1 $o2"
		done
	done
}

kind_persist() {
	for o1 in $OPS; do
		begin
		"ace_$o1"
		persist
		finish persist "$o1 fsync" /tmp/durable.snap
	done
}

kind_split() {
	for o1 in $OPS; do
		for o2 in $OPS; do
			begin
			"ace_$o1"
			persist
			"ace_$o2" 2>/dev/null
			finish split "$o1 fsync $o2" /tmp/durable.snap
		done
	done
}

for kind in one pair persist split; do
	run_kind "kind_$kind"
	if [ "$ACE_BAD" -eq 0 ]; then
		pass "ace-$kind"
		echo "fault_ace.sh: $kind: $ACE_N sequences, every one held"
	fi
done

require_no_reclaim no-reclaim
exit "$FAILED"
