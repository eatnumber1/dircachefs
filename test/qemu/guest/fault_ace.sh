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
#   mixed    (step 26.14e) a seeded sample of mixed-fault sequences: one of the
#            operations, then three events drawn from the nine below, then a
#            power cut (unless the third event was one), judged by the path
#            oracle (served == backing, after every restart) and the identity
#            oracle (guest/fault_lib.sh identity_check: a handle taken before
#            a cut opens to the same object or fails ESTALE, never answers for
#            something gone):
#              crash      SIGKILL of the daemon and a restart, no cut
#              crash3     a create held in phase 3 (both filesystems frozen
#                         in turn) and the daemon killed there, then a restart
#              cutahead   a create in t/e (its phase 1 fsyncs the cache's WAL,
#                         taking what the cache recorded to its disk), then a
#                         power cut with the backing filesystem's commit still
#                         to come (the cache ahead of it), and a restart
#              cutbehind  a syncfs of the backing filesystem, then the cut
#                         (the backing is durable, the cache's phase 3 is not)
#              fail3      a create that reaches the backing filesystem and
#                         whose phase 3 fails (the cache filesystem is full:
#                         guest/enospc_cache.sh's fault point), then the
#                         space is freed
#              lookup     the kernel's caches dropped (no sync), then a lookup
#                         of every name the sequences use
#              listing    the caches dropped, then both directories listed
#              sync       an fsync of the directory through dcfs (a sync point)
#              handle     name_to_handle_at of every object (identity_take)
#            dcfs_ace_seed=N (default 1) and dcfs_ace_count=N (default 20)
#            pick the sample; dcfs_seed= (run-qemu.sh's $DCFS_SEED) overrides
#            the seed, so a scheduled job draws a new sample each time. The
#            sequences are printed (and kept in /tmp/mixed.seq), and
#            dcfs_ace_sequences=op:e1:e2:e3,... replays chosen ones.
#   mixedfixtures  the mixed oracle's negative fixtures: the same sequence
#            passes untouched and is rejected once the handle record is
#            tampered with (the `tamper` event, fixtures only).
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
BG_PID=""
FHTEST=/bin/fhtest

# OPS: the operations, each a function ace_<name> on the tree under $MNT/t.
OPS="create mkdir unlink rename replace link xrename chmod append"
KINDS="one pair persist split direct dsplit fixtures"
# The mixed kinds are not in the default list: they are targets of their own.
cmdline() { sed -n "s/.*\\b$1=\\([^ ]*\\).*/\\1/p" /proc/cmdline | tr , ' '; }
[ -z "$(cmdline dcfs_ace_ops)" ] || OPS=$(cmdline dcfs_ace_ops)
[ -z "$(cmdline dcfs_ace_kinds)" ] || KINDS=$(cmdline dcfs_ace_kinds)

cleanup() {
	rc=$?
	if [ "$rc" -ne 0 ] || [ "$FAILED" -ne 0 ]; then
		echo "--- dcfs stderr (the last run) ---"
		cat "$LOG" 2>/dev/null
	fi
	[ -z "$BG_PID" ] || kill -KILL "$BG_PID" 2>/dev/null || true
	fd_thaw back >/dev/null 2>&1 || true
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
require_commands umount sync find stat diff sort ln mv rm mkdir md5sum awk

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

# --- mixed-fault sequences (step 26.14e) --------------------------------------

MIXED_EVENTS="crash crash3 cutahead cutbehind fail3 lookup listing sync handle"
HANDLES=/tmp/handles.rec
FILL=$CACHE_DIR/fill

# mixed_sequences: the sequences to run, one per line, "op e1 e2 e3".
mixed_sequences() {
	if [ -n "$(cmdline dcfs_ace_sequences)" ]; then
		for ms_seq in $(cmdline dcfs_ace_sequences); do
			echo "$ms_seq" | tr : ' '
		done
		return
	fi
	ms_seed=$(cmdline dcfs_seed)
	[ -n "$ms_seed" ] || ms_seed=$(cmdline dcfs_ace_seed)
	ms_count=$(cmdline dcfs_ace_count)
	awk -v seed="${ms_seed:-1}" -v n="${ms_count:-20}" -v ops="$OPS" -v events="$MIXED_EVENTS" '
		BEGIN {
			srand(seed)
			no = split(ops, o, " ")
			ne = split(events, e, " ")
			for (i = 0; i < n; i++) {
				line = o[int(rand() * no) + 1]
				for (j = 0; j < 3; j++) line = line " " e[int(rand() * ne) + 1]
				print line
			}
		}'
}

# mixed_check WHEN: the oracles over the state now (a restart just happened);
# what they say is added to MIXED_PROBLEMS.
mixed_check() {
	mc_p=$(problems)
	[ -z "$mc_p" ] || MIXED_PROBLEMS="$MIXED_PROBLEMS [after $1, path oracle: $mc_p]"
	mc_i=$(identity_check "$MNT" "$SRC" "$HANDLES" | tr '\n' '|')
	[ -z "$mc_i" ] || MIXED_PROBLEMS="$MIXED_PROBLEMS [after $1, identity oracle: $mc_i]"
}

# Dropping the kernel's dentries and inodes without `sync` (guest/lib.sh's
# drop_caches syncs every filesystem, the backing one too: a durable point the
# sequence did not ask for).
mixed_drop() { echo 3 >/proc/sys/vm/drop_caches; }

mixed_event_crash() {
	fd_crash
	if ! fd_start "$LOG" --sync_interval_sec=3600; then
		fail "ace-restart" "$MIXED_SEQ: daemon did not mount within 10s after the crash"
		exit "$FAILED"
	fi
	mixed_drop
	mixed_check crash
}
mixed_event_cutahead() {
	# A create in the other directory: its phase 1 fsyncs the cache's WAL, which
	# takes everything the cache recorded so far to the cache disk, while the
	# backing filesystem's journal has committed nothing (the long commit
	# interval): the cache ahead of the backing filesystem, as in
	# guest/fault_power.sh's `ahead`.
	touch "$MNT/t/e/ahead" 2>/dev/null || true
	cut_and_restart "$MIXED_SEQ"
	mixed_check cutahead
}
mixed_event_cutbehind() {
	"$TESTUTIL" syncfs "$SRC" || fail ace-syncfs "syncfs of $SRC failed"
	cut_and_restart "$MIXED_SEQ"
	mixed_check cutbehind
}
mixed_event_sync() { "$TESTUTIL" fsync "$MNT/t" || fail ace-fsync "fsync of $MNT/t failed"; }
# mixed_lookup_names: a lookup (by name) of every name the sequences use, those
# that are not there included.
mixed_lookup_names() {
	for me_n in t t/a t/b t/n t/m t/l t/r t/e t/e/a t/e/x t/f3 t/c3; do
		stat "$MNT/$me_n" >/dev/null 2>&1 || true
	done
}
# handle: the names looked up first (as an NFS server does before it hands a
# handle out), then the handle of everything under t.
mixed_event_handle() {
	mixed_lookup_names
	identity_take "$MNT" "$MNT/t" "$HANDLES"
}
mixed_event_lookup() {
	mixed_drop
	mixed_lookup_names
}
mixed_event_listing() {
	mixed_drop
	ls "$MNT/t" "$MNT/t/e" >/dev/null 2>&1 || true
}

# fail3: a create held in phase 2 (the backing filesystem frozen) while the
# cache filesystem fills, so that its phase 3 cannot commit; as
# guest/enospc_cache.sh's "phase 3 cannot commit". The create happened on the
# backing filesystem; dcfs replies EEXIST.
mixed_event_fail3() {
	ls "$MNT/t" >/dev/null 2>&1
	fd_freeze back || fail ace-fail3 "FIFREEZE of the backing filesystem failed"
	touch "$MNT/t/f3" 2>/dev/null &
	BG_PID=$!
	if ! fd_blocked "$DAEMON_PID" "$SRC"; then
		fail ace-fail3 "$MIXED_SEQ: the daemon never blocked on the frozen backing filesystem"
	fi
	mkdir -p "$FILL"
	fd_fill "$FILL" >/dev/null
	fd_thaw back || fail ace-fail3 "FITHAW of the backing filesystem failed"
	wait "$BG_PID" 2>/dev/null || true
	BG_PID=""
	rm -rf "$FILL"
	sync
}

# crash3: a create held in phase 3 (the backing filesystem frozen in its
# syscall, then the cache filesystem frozen and the backing one thawed: the
# create is on the backing filesystem and the daemon is held before the cache
# has it) and the daemon killed there; the restart finds the dirty marks and
# phase 3 never happened. As guest/fault_power.sh's `phase2`, without the cut.
mixed_event_crash3() {
	ls "$MNT/t" >/dev/null 2>&1
	fd_freeze back || fail ace-crash3 "FIFREEZE of the backing filesystem failed"
	touch "$MNT/t/c3" 2>/dev/null &
	BG_PID=$!
	fd_blocked "$DAEMON_PID" "$SRC" || fail ace-crash3 "$MIXED_SEQ: the daemon never blocked on the frozen backing filesystem"
	fd_freeze cache || fail ace-crash3 "FIFREEZE of the cache filesystem failed"
	fd_thaw back || fail ace-crash3 "FITHAW of the backing filesystem failed"
	fd_blocked "$DAEMON_PID" "$CACHE_DIR" || fail ace-crash3 "$MIXED_SEQ: the daemon never blocked in phase 3 on the frozen cache filesystem"
	# Killed first and thawed after: held by the freeze the daemon dies when its
	# write returns, before phase 3's commit frame.
	kill -KILL "$DAEMON_PID" 2>/dev/null || true
	fd_thaw cache || fail ace-crash3 "FITHAW of the cache filesystem failed"
	fd_crash
	wait "$BG_PID" 2>/dev/null || true
	BG_PID=""
	if ! fd_start "$LOG" --sync_interval_sec=3600; then
		fail "ace-restart" "$MIXED_SEQ: daemon did not mount within 10s after the kill in phase 3"
		exit "$FAILED"
	fi
	mixed_drop
	mixed_check crash3
}

# tamper (the fixtures only): the handle record says every object had another
# inode number, as if a handle had opened a different object after recovery.
mixed_event_tamper() {
	awk '$2 != "?" { $2 = $2 + 1 } { print }' "$HANDLES" >"$HANDLES.new"
	mv "$HANDLES.new" "$HANDLES"
}

# mixed_run OP EVENT...: MIXED_PROBLEMS says what the oracles reported ("": the
# sequence held).
mixed_run() {
	MIXED_SEQ="$*"
	MIXED_PROBLEMS=""
	mr_op=$1
	shift
	begin
	: >"$HANDLES"
	"ace_$mr_op" 2>/dev/null
	for mr_event in "$@"; do
		"mixed_event_$mr_event"
	done
	case "$mr_event" in
	crash | crash3 | cutahead | cutbehind) ;;
	*) mixed_event_cutahead ;;
	esac
	fd_crash
}

kind_mixed() {
	mixed_sequences >/tmp/mixed.seq
	echo "fault_ace.sh: mixed: the sample (seed $(cmdline dcfs_seed)/$(cmdline dcfs_ace_seed), $(wc -l </tmp/mixed.seq) sequences):"
	cat /tmp/mixed.seq
	while IFS= read -r km_seq; do
		# shellcheck disable=SC2086 # one word per event
		mixed_run $km_seq
		ACE_N=$((ACE_N + 1))
		if [ -n "$MIXED_PROBLEMS" ]; then
			ACE_BAD=$((ACE_BAD + 1))
			fail ace-mixed "$km_seq:$MIXED_PROBLEMS"
		fi
	done </tmp/mixed.seq
}

# The negative fixtures of the mixed oracles.
kind_mixedfixtures() {
	# Untouched, the sequence holds (so the rejection below is the tamper's).
	mixed_run create handle cutahead lookup
	if [ -n "$MIXED_PROBLEMS" ]; then
		ACE_BAD=$((ACE_BAD + 1))
		fail ace-mixedfixtures "the untampered fixture sequence was rejected:$MIXED_PROBLEMS"
	else
		echo "fault_ace.sh: the fixture sequence holds untampered"
	fi
	# Injected: the same sequence with the handle record tampered with.
	mixed_run create handle tamper cutahead lookup
	case "$MIXED_PROBLEMS" in
	*"identity oracle"*"opened a different object"*)
		echo "fault_ace.sh: a sequence with an identity violation injected is rejected"
		;;
	*)
		ACE_BAD=$((ACE_BAD + 1))
		fail ace-mixedfixtures "a tampered handle record was not rejected by the identity oracle:$MIXED_PROBLEMS"
		;;
	esac
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
