#!/bin/sh
# dcfs step 11.2: bounded sequences of operations with a power cut after them,
# in the manner of ACE (the CrashMonkey/ACE bounded black-box crash tests):
# sequences of up to two operations from a small set, on a small tree, with or
# without a persistence point between and after them, then a power cut (both
# disks drop writes, as in guest/fault_power.sh: fault_lib.sh), a remount from
# what the disks hold, a restart of dcfs, and the checks:
#
#   served   everything dcfs serves is what the backing filesystem holds, entry
#            by entry (guest/lib.sh's snapshot: type, size, mode, links, and the
#            md5 of every regular file);
#   durable  the backing filesystem holds exactly what it held at the
#            persistence point, and not what was done after it (nothing else
#            commits its journal within the run: the mount's commit interval is
#            long where the filesystem has one).
#
# Two persistence points, which leave different states for the cut to find:
#   dcfs     an fsync of the directory through dcfs (FSYNCDIR, which runs a sync
#            point): the backing filesystem is durable, and so is the dirty
#            set's clearing in the cache; what came after is lost on both disks
#            except the cache's durable phase 1 commits: "backing as of the
#            fsync, cache possibly ahead";
#   direct   a syncfs of the backing filesystem itself, behind dcfs (what a sync
#            point does, without the clearing of the dirty set): "backing
#            durable, the cache's phase 3 not": the dirty set was not cleared,
#            so recovery has the entries to forget. (An fsync of the directory
#            alone is not a persistence point for file data: the first run of
#            this kind, with that, lost the appended bytes of a file and the
#            md5 in the snapshot said so.)
# Neither makes the cache durable ahead of the backing filesystem by more than
# what phase 1's fsync does, and no sequence here cuts inside a mutation (that
# is guest/fault_power.sh).
#
# Kinds (dcfs_ace_kinds= on the kernel command line picks some; the operations
# come from dcfs_ace_ops=), per operation O and per ordered pair O1, O2:
#   one      O, cut                       served
#   pair     O1, O2, cut                  served
#   persist  O, fsync, cut                served, durable
#   split    O1, fsync, O2, cut           served, durable (O2 is lost)
#   direct   O, direct syncfs, cut        served, durable
#   dsplit   O1, direct syncfs, O2, cut   served, durable (O2 is lost)
#   fixtures the checker's negative fixtures: a persistence point that was not
#            made must be reported as one the backing filesystem did not keep,
#            and a name added behind dcfs's back as a difference; a checker that
#            reported neither would pass every sequence for the wrong reason.
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
KINDS="one pair persist split direct dsplit fixtures"
cmdline() { sed -n "s/.*\\b$1=\\([^ ]*\\).*/\\1/p" /proc/cmdline | tr , ' '; }
[ -z "$(cmdline dcfs_ace_ops)" ] || OPS=$(cmdline dcfs_ace_ops)
[ -z "$(cmdline dcfs_ace_kinds)" ] || KINDS=$(cmdline dcfs_ace_kinds)

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

echo "fault_ace.sh: kernel $(uname -r), operations: $OPS; kinds: $KINDS"
require_commands umount sync find stat diff sort ln mv rm mkdir md5sum

ace_create() { : >"$MNT/t/n"; }
ace_mkdir() { mkdir "$MNT/t/m"; }
ace_unlink() { rm "$MNT/t/a"; }
ace_rename() { mv "$MNT/t/a" "$MNT/t/r"; }
ace_replace() { mv "$MNT/t/a" "$MNT/t/b"; }
ace_link() { ln "$MNT/t/a" "$MNT/t/l"; }
ace_xrename() { mv "$MNT/t/a" "$MNT/t/e/a"; }
ace_chmod() { chmod 600 "$MNT/t/a"; }
ace_append() { echo more >>"$MNT/t/a"; }

# ace_rebuild: the tree, directly on the backing filesystem (a and b of
# different lengths), and no cache database.
ace_rebuild() {
	rm -rf "$SRC/t"
	mkdir "$SRC/t" "$SRC/t/e"
	echo aaaa >"$SRC/t/a"
	echo bbbbbbb >"$SRC/t/b"
	echo xxxx >"$SRC/t/e/x"
	sync
	rm -f "$CACHE_DIR"/dcfs.db*
}

# begin: a cold daemon over the tree.
begin() {
	ace_rebuild
	if ! fd_start "$LOG" --sync_interval_sec=3600; then
		fail ace-start "daemon did not mount within 10s"
		exit "$FAILED"
	fi
	ls "$MNT/t" "$MNT/t/e" >/dev/null
}

# The persistence points: the backing filesystem's state at that moment goes to
# /tmp/durable.snap.
persist_dcfs() {
	"$TESTUTIL" fsync "$MNT/t" || fail ace-fsync "fsync of $MNT/t failed"
	snapshot "$SRC" >/tmp/durable.snap
}
persist_direct() {
	"$TESTUTIL" syncfs "$SRC" || fail ace-syncfs "syncfs of $SRC failed"
	snapshot "$SRC" >/tmp/durable.snap
}
# A persistence point that is not made (the negative fixture).
persist_none() {
	snapshot "$SRC" >/tmp/durable.snap
}

# cut_and_restart WHAT: the cut, the remount, the restart; ends with a daemon
# over what the disks hold and the caches dropped.
cut_and_restart() {
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
		fail "ace-restart" "$1: daemon did not mount within 10s after the cut"
		exit "$FAILED"
	fi
	drop_caches
}

# problems [DURABLE-SNAPSHOT]: what is wrong with the state after the cut, as
# text (empty: nothing): served against the backing filesystem and, if a
# snapshot is given, the backing filesystem against it.
problems() {
	snapshot "$SRC" atime >/tmp/backing.snap
	snapshot "$MNT" atime >/tmp/served.snap 2>&1
	if ! command diff /tmp/served.snap /tmp/backing.snap >/tmp/snap.diff 2>&1; then
		echo "served (<) and backing (>) differ: $(tr '\n' '|' </tmp/snap.diff)"
	fi
	# Without access times: the persistence point's own snapshot read the
	# files, and the cut may have lost those reads' access times.
	[ -n "${1:-}" ] && snapshot "$SRC" >/tmp/backing-durable.snap
	if [ -n "${1:-}" ] && ! command diff "$1" /tmp/backing-durable.snap >/tmp/durable.diff 2>&1; then
		echo "the backing filesystem after the cut (>) is not what it was at the persistence point (<): $(tr '\n' '|' </tmp/durable.diff)"
	fi
}

# finish KIND SEQ [DURABLE-SNAPSHOT]
finish() {
	cut_and_restart "$2"
	f_problems=$(problems "${3:-}")
	if [ -n "$f_problems" ]; then
		ACE_BAD=$((ACE_BAD + 1))
		fail "ace-$1" "$2: $f_problems"
	fi
	fd_crash
	ACE_N=$((ACE_N + 1))
}

fd_setup || {
	fail setup "wrapping or mounting the disks failed"
	exit "$FAILED"
}
FD_LONG_COMMIT=1
fd_umount_disks
fd_mount_backing || {
	fail setup "mounting the backing filesystem with a long commit interval failed"
	exit "$FAILED"
}
mount "$(fault_dev "$FD_CACHE")" "$CACHE_DIR" || {
	fail setup "mounting the cache filesystem failed"
	exit "$FAILED"
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
		persist_dcfs
		finish persist "$o1 fsync" /tmp/durable.snap
	done
}
kind_split() {
	for o1 in $OPS; do
		for o2 in $OPS; do
			begin
			"ace_$o1"
			persist_dcfs
			"ace_$o2" 2>/dev/null
			finish split "$o1 fsync $o2" /tmp/durable.snap
		done
	done
}
kind_direct() {
	for o1 in $OPS; do
		begin
		"ace_$o1"
		persist_direct
		finish direct "$o1 direct-syncfs" /tmp/durable.snap
	done
}
kind_dsplit() {
	for o1 in $OPS; do
		for o2 in $OPS; do
			begin
			"ace_$o1"
			persist_direct
			"ace_$o2" 2>/dev/null
			finish dsplit "$o1 direct-syncfs $o2" /tmp/durable.snap
		done
	done
}

# The negative fixtures: the checks must be able to fail.
kind_fixtures() {
	# A create whose persistence point was not made: the backing filesystem
	# after the cut does not have it, and the check must say so.
	begin
	ace_create
	persist_none
	ace_fixture_ok=0
	cut_and_restart "fixture: no persistence point"
	case "$(problems /tmp/durable.snap)" in
	*"is not what it was at the persistence point"*) ace_fixture_ok=1 ;;
	esac
	fd_crash
	if [ "$ace_fixture_ok" -eq 1 ]; then
		echo "fault_ace.sh: a persistence point that was not made is reported"
	else
		ACE_BAD=$((ACE_BAD + 1))
		fail ace-fixtures "a create with no persistence point was not reported as lost"
	fi
	# A name added behind dcfs's back after the restart: served != backing.
	begin
	ace_create
	cut_and_restart "fixture: behind its back"
	ls "$MNT/t" >/dev/null
	touch "$SRC/t/behind-its-back"
	case "$(problems)" in
	*"served (<) and backing (>) differ"*)
		echo "fault_ace.sh: a name added behind dcfs's back is reported"
		;;
	*)
		ACE_BAD=$((ACE_BAD + 1))
		fail ace-fixtures "a name only the backing filesystem has went unnoticed"
		;;
	esac
	fd_crash
	ACE_N=2
}

for kind in $KINDS; do
	ACE_N=0
	ACE_BAD=0
	"kind_$kind"
	if [ "$ACE_BAD" -eq 0 ]; then
		pass "ace-$kind"
		echo "fault_ace.sh: $kind: $ACE_N sequences, every one held"
	fi
done

require_no_reclaim no-reclaim
exit "$FAILED"
