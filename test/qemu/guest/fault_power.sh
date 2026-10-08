#!/bin/sh
# dcfs step 11.1: power cuts, on real disks (docs/plan/phases/11-crash-stress-
# failure-testing.md; design.md, "A power loss or kernel crash" and "Sync
# points"). guest/power.sh fabricates the state a power loss leaves; here the
# disks themselves lose it. Both disks (backing and cache) sit on dm devices
# (guest/fault_lib.sh); a cut is both switched to drop-writes at one instant
# (fd_cut): what had reached each disk stays, nothing after does. The daemon
# is then killed, both filesystems unmounted (their writes dropped) and
# mounted again healthy, and dcfs started over what the disks hold.
#
# A cut is placed in a mutation's phases with fsfreeze, which holds a mutation
# in its first write to a frozen filesystem:
#
#   before     the cut, then the create: everything is lost
#   phase1     the backing filesystem is frozen, so the create is held in
#              phase 2 with phase 1 durable in the cache: cut there, the
#              create is never durable on the backing filesystem
#   phase2     as phase1, then the cache filesystem is frozen and the backing
#              one thawed and synced: the create is durable on the backing
#              filesystem and the daemon is held in phase 3, before the
#              cache has it: cut there
#   ahead      the backing filesystem drops writes from the start, the cache
#              does not: two creates in different directories, whose phase 1
#              commits fsync the cache WAL, with the first create's phase 3
#              in it: the cache has what the backing filesystem lost
#              ("cache ahead", the case the dirty set is for)
#   syncpoint  the cache filesystem is frozen, and a request runs the
#              periodic sync point: the backing filesystem is synced, and the
#              daemon is held clearing the dirty set: cut there, the cache
#              keeps the dirty rows of a backing change that did survive
#
# After each, the invariant: every entry dcfs serves matches the backing
# filesystem exactly (type, size, mode, and the listing of every directory),
# and the recovery names the dirty rows that survived. A cache ahead of the
# backing filesystem (a name served that the backing filesystem lost) or behind
# it (a name not served that it kept) fails.
#
# Two ways to cut, one script (step 11.2):
#
#   dm    (the default, one boot) the cut is both disks dropping writes, as
#         above, and the five scenarios run one after the other;
#   kill  (run-qemu.sh --power-cut, dcfs_cut=kill on the kernel command line)
#         the cut is a real one: the host kills QEMU when the guest prints
#         DCFS-POWER-CUT-NOW, and a second boot over the same disk images runs
#         the checks. One scenario per pair of boots (dcfs_scenario=,
#         dcfs_boot=1|2). A killed QEMU keeps every write the guest completed
#         and loses its page cache: the state drop-writes models, so the
#         same checks must pass on both, which validates the model.
#
# Run as /tests/fault_power.sh by guest/init when booted with
# dcfs_test=fault_power.sh.
FAILED=0
. "$(dirname "$0")/lib.sh"
. "$(dirname "$0")/fault_lib.sh"
. "$(dirname "$0")/fault_dcfs_lib.sh"

DCFS=/bin/dcfs
TESTUTIL=/bin/testutil
SRC=/src
MNT=/mnt
DB=$CACHE_DIR/dcfs.db
LOGS=""
RUN=0
TOUCH_PID=""
DAEMON_PID=""
MOUNTED=0

cmdline() { sed -n "s/.*\b$1=\([^ ]*\).*/\1/p" /proc/cmdline; }
CUT_MODE=$(cmdline dcfs_cut)
CUT_MODE=${CUT_MODE:-dm}
SCENARIO=$(cmdline dcfs_scenario)
BOOT=$(cmdline dcfs_boot)

cleanup() {
	rc=$?
	if [ "$rc" -ne 0 ] || [ "$FAILED" -ne 0 ]; then
		for log in $LOGS; do
			echo "--- dcfs stderr ($log) ---"
			cat "$log" 2>/dev/null
		done
	fi
	[ -n "$TOUCH_PID" ] && kill -KILL "$TOUCH_PID" 2>/dev/null || true
	fd_thaw back >/dev/null 2>&1 || true
	fd_thaw cache >/dev/null 2>&1 || true
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

echo "fault_power.sh: kernel $(uname -r), cut $CUT_MODE${SCENARIO:+, scenario $SCENARIO boot $BOOT}"
require_commands umount sync find stat diff sort md5sum

# start [flags]: a new run of dcfs, its log kept for the failure dump.
start() {
	RUN=$((RUN + 1))
	LOG=/tmp/dcfs-$RUN.log
	LOGS="$LOGS $LOG"
	fd_start "$LOG" --sync_interval_sec=3600 "$@"
}

# same_as_backing: success if what the daemon serves is what the backing
# filesystem holds, entry by entry (the diff in /tmp/snap.diff if not).
same_as_backing() {
	drop_caches
	snapshot "$SRC" >/tmp/backing.snap
	snapshot "$MNT" >/tmp/served.snap 2>&1
	command diff /tmp/served.snap /tmp/backing.snap >/tmp/snap.diff 2>&1
}

# served_equals_backing NAME
served_equals_backing() {
	if same_as_backing; then
		pass "$1"
	else
		fail "$1" "served (<) and backing (>) differ: $(tr '\n' '|' </tmp/snap.diff)"
	fi
}

# after_cut NAME: the daemon is killed, the filesystems remounted from what
# the disks hold, and a new run started; leaves it running. (In kill mode the
# second boot has done all but the start.)
after_cut() {
	if [ "$CUT_MODE" = kill ]; then
		fd_setup || fail "$1-setup" "wrapping or mounting the disks failed"
	else
		fd_crash
		fd_restore || fail "$1-restore" "remounting after the cut failed"
	fi
	if start; then
		pass "$1-restart"
	else
		fail "$1-restart" "daemon did not mount within 10s"
		exit "$FAILED"
	fi
}

# wait_touch: waits for the background `touch`.
wait_touch() {
	wait "$TOUCH_PID" 2>/dev/null || true
	TOUCH_PID=""
}

# power_cut [thaw-after-cut...]: the power cut. dm: both disks drop writes, then
# the filesystems named (back, cache) are thawed so the held daemon finishes
# into the void, and the background request is waited for. kill: the host
# kills QEMU when it reads the marker; nothing here runs afterwards.
power_cut() {
	if [ "$CUT_MODE" = kill ]; then
		# Boot 1 has no verdict of its own on the host but this one: a check
		# that failed on the way here (a freeze that did not hold, say) means
		# the state is not the one the scenario names, so no cut is made.
		[ "$FAILED" -eq 0 ] || exit "$FAILED"
		# What this boot would otherwise report at its end (guest/init's
		# scan of the kernel log, which a cut boot never reaches), for the
		# host to read before the marker.
		dmesg_oom_lines
		dmesg_kernel_failures
		echo "DCFS-POWER-CUT-NOW"
		# The host kills QEMU on that line; nothing runs after it.
		exec sleep 600
	fi
	fd_cut || fail cut "fd_cut failed"
	for fs in "$@"; do fd_thaw "$fs"; done
	[ -n "$TOUCH_PID" ] && wait_touch
}

# --- the tree, and the scenarios, as setup (before the cut) and check -------

make_tree() {
	for d in d1 d2 d3 d4 d5 d6; do
		mkdir "$SRC/$d"
		echo keep >"$SRC/$d/keep"
	done
	echo u1 >"$SRC/d6/u1"
	echo r1 >"$SRC/d6/r1"
	sync
}

warm() {
	ls "$MNT/d1" "$MNT/d2" "$MNT/d3" "$MNT/d4" "$MNT/d5" "$MNT/d6" >/dev/null
}

# before: a file synced to the backing filesystem survives the cut, and what
# was done after it without a sync (a file made on the backing filesystem, a
# create through dcfs) does not: the witness that a cut, dropped or real, keeps
# what was durable and loses the rest. In dm mode the cut comes before those
# writes (they are dropped); in kill mode after them (they are still in the
# journal's running transaction).
setup_before() {
	echo witness >"$SRC/w-synced"
	"$TESTUTIL" fsync "$SRC/w-synced" && "$TESTUTIL" fsync "$SRC" ||
		fail before-sync "fsync of the witness failed"
	if [ "$CUT_MODE" != kill ]; then
		fd_cut || fail before-cut "fd_cut failed"
	fi
	echo witness >"$SRC/w-unsynced"
	touch "$MNT/d1/before"
}
cut_before() {
	if [ "$CUT_MODE" = kill ]; then power_cut; fi
}
check_before() {
	if [ -e "$SRC/w-synced" ]; then
		pass before-synced-kept
	else
		fail before-synced-kept "the file synced before the cut is gone"
	fi
	if [ ! -e "$SRC/w-unsynced" ]; then
		pass before-unsynced-lost
	else
		fail before-unsynced-lost "a file written without a sync survived the cut"
	fi
	if [ ! -e "$SRC/d1/before" ]; then
		pass before-lost
	else
		fail before-lost "d1/before survived a cut made before it was durable"
	fi
	served_equals_backing before-served
}

# phase1: held in phase 2, phase 1 durable, then the cut.
setup_phase1() {
	fd_freeze back || fail phase1-freeze "FIFREEZE of the backing filesystem failed"
	touch "$MNT/d2/p1" &
	TOUCH_PID=$!
	if fd_blocked "$DAEMON_PID" "$SRC"; then
		echo "fault_power.sh: held: $(fd_where "$DAEMON_PID")"
		pass phase1-held
	else
		fail phase1-held "the daemon never blocked on the frozen backing filesystem"
	fi
}
cut_phase1() { power_cut back; }
check_phase1() {
	if [ ! -e "$SRC/d2/p1" ]; then
		pass phase1-create-lost
	else
		fail phase1-create-lost "d2/p1 survived a cut before its create was durable"
	fi
	if [ "$(fd_recovered "$LOG")" -ge 1 ]; then
		pass phase1-dirty-recovered
	else
		fail phase1-dirty-recovered "phase 1 was durable, yet no dirty row was recovered: $(grep -i cleanly "$LOG")"
	fi
	served_equals_backing phase1-served
}

# frozen_unlink, frozen_rename (step 11.6): a mutation held in phase 2 by a
# frozen backing filesystem (the daemon is in the syscall, state D), the cut
# while it is held. Neither the unlink nor the rename ever reached the backing
# filesystem's disk; phase 1 (names unknown, dirty rows) was durable in the
# cache. After the cut the backing filesystem holds the old names, and the
# restart serves them, recovering the dirty rows.
frozen_hold() {
	# $1: the scenario; $2: the name it holds (u1, r1); the rest: the command
	fh_name=$1
	fh_held=$2
	shift 2
	ls "$MNT/d6" >/dev/null
	fd_freeze back || fail "$fh_name-freeze" "FIFREEZE of the backing filesystem failed"
	"$@" &
	TOUCH_PID=$!
	if fd_blocked "$DAEMON_PID" "$SRC"; then
		echo "fault_power.sh: held: $(fd_where "$DAEMON_PID")"
		pass "$fh_name-held"
	else
		fail "$fh_name-held" "the daemon never blocked on the frozen backing filesystem"
	fi
	fh_unknown=$("$TESTUTIL" sql "$DB" "SELECT CAST(name AS TEXT) FROM dentries WHERE state='unknown'" | tr '\n' ' ')
	case "$fh_unknown" in
	*"$fh_held"*) pass "$fh_name-record-unknown" ;;
	*) fail "$fh_name-record-unknown" "$fh_held is not unknown in the database while the mutation is held: '$fh_unknown'" ;;
	esac
}
setup_frozen_unlink() { frozen_hold frozen_unlink u1 rm "$MNT/d6/u1"; }
cut_frozen_unlink() { power_cut back; }
check_frozen_unlink() {
	if [ -e "$SRC/d6/u1" ]; then
		pass frozen_unlink-file-kept
	else
		fail frozen_unlink-file-kept "d6/u1 is gone: the unlink was held by the freeze and cut before it reached the disk"
	fi
	if [ "$(fd_recovered "$LOG")" -ge 1 ]; then
		pass frozen_unlink-dirty-recovered
	else
		fail frozen_unlink-dirty-recovered "no dirty row recovered: $(grep -i cleanly "$LOG")"
	fi
	served_equals_backing frozen_unlink-served
	if [ -e "$MNT/d6/u1" ]; then
		pass frozen_unlink-name-served
	else
		fail frozen_unlink-name-served "d6/u1 exists on the backing filesystem but is not served"
	fi
}
setup_frozen_rename() { frozen_hold frozen_rename r1 mv "$MNT/d6/r1" "$MNT/d6/r2"; }
cut_frozen_rename() { power_cut back; }
check_frozen_rename() {
	if [ -e "$SRC/d6/r1" ] && [ ! -e "$SRC/d6/r2" ]; then
		pass frozen_rename-old-name-kept
	else
		fail frozen_rename-old-name-kept "d6: $(ls "$SRC/d6" | tr '\n' ' ')"
	fi
	if [ "$(fd_recovered "$LOG")" -ge 1 ]; then
		pass frozen_rename-dirty-recovered
	else
		fail frozen_rename-dirty-recovered "no dirty row recovered: $(grep -i cleanly "$LOG")"
	fi
	served_equals_backing frozen_rename-served
	if [ -e "$MNT/d6/r1" ] && [ ! -e "$MNT/d6/r2" ]; then
		pass frozen_rename-names-served
	else
		fail frozen_rename-names-served "served d6: $(ls "$MNT/d6" 2>&1 | tr '\n' ' ')"
	fi
}

# phase2: the create durable on the backing filesystem, phase 3 not.
setup_phase2() {
	ls "$MNT/d3" >/dev/null
	fd_freeze back || fail phase2-freeze "FIFREEZE of the backing filesystem failed"
	touch "$MNT/d3/p2" &
	TOUCH_PID=$!
	fd_blocked "$DAEMON_PID" "$SRC" || fail phase2-held-1 "the daemon never blocked on the frozen backing filesystem"
	fd_freeze cache || fail phase2-freeze-cache "FIFREEZE of the cache filesystem failed"
	fd_thaw back
	sleep 1
	# The create ran (phase 2) and the daemon is held before phase 3.
	if [ -e "$SRC/d3/p2" ] && kill -0 "$TOUCH_PID" 2>/dev/null; then
		pass phase2-held
	else
		fail phase2-held "d3/p2 on the backing filesystem: $([ -e "$SRC/d3/p2" ] && echo yes || echo no); touch running: $(kill -0 "$TOUCH_PID" 2>/dev/null && echo yes || echo no)"
	fi
	# Make the create durable: a freeze flushes the backing filesystem's journal.
	fd_freeze back && fd_thaw back
}
cut_phase2() { power_cut cache; }
check_phase2() {
	if [ -e "$SRC/d3/p2" ]; then
		pass phase2-create-kept
	else
		fail phase2-create-kept "d3/p2 was synced yet is gone"
	fi
	if [ "$(fd_recovered "$LOG")" -ge 1 ]; then
		pass phase2-dirty-recovered
	else
		fail phase2-dirty-recovered "no dirty row recovered: $(grep -i cleanly "$LOG")"
	fi
	served_equals_backing phase2-served
	if [ -e "$MNT/d3/p2" ] && [ "$(ls "$MNT/d3" | tr '\n' ' ')" = "keep p2 " ]; then
		pass phase2-name-served
	else
		fail phase2-name-served "d3: $(ls "$MNT/d3" 2>&1)"
	fi
}

# ahead: the cache has what the backing filesystem lost. dm: the backing
# filesystem drops writes from the start. kill: it simply has not committed its
# journal yet (the mount's commit interval is long).
setup_ahead() {
	ls "$MNT/d4" "$MNT/d5" >/dev/null
	[ "$CUT_MODE" = kill ] || fault_mode "$FD_BACK" drop-writes || fail ahead-cut-backing "fault_mode failed"
	touch "$MNT/d4/a1"
	# d5's phase 1 is a durable commit (the directory is not yet dirty): the
	# WAL fsync takes d4/a1's phase 3 to the cache disk with it.
	touch "$MNT/d5/a2"
}
cut_ahead() {
	if [ "$CUT_MODE" = kill ]; then
		power_cut
	else
		fault_mode "$FD_CACHE" drop-writes || fail ahead-cut-cache "fault_mode failed"
	fi
}
check_ahead() {
	if [ ! -e "$SRC/d4/a1" ] && [ ! -e "$SRC/d5/a2" ]; then
		pass ahead-creates-lost
	else
		fail ahead-creates-lost "the backing filesystem kept $(ls "$SRC/d4" "$SRC/d5" | tr '\n' ' ') (it was cut before)"
	fi
	if [ "$(fd_recovered "$LOG")" -ge 2 ]; then
		pass ahead-dirty-recovered
	else
		fail ahead-dirty-recovered "recovered $(fd_recovered "$LOG") dirty rows, want at least the two parents: $(grep -i cleanly "$LOG")"
	fi
	served_equals_backing ahead-served
	if [ ! -e "$MNT/d4/a1" ] && [ ! -e "$MNT/d5/a2" ]; then
		pass ahead-names-not-served
	else
		fail ahead-names-not-served "a name the backing filesystem lost is served (cache ahead)"
	fi
}

# syncpoint: held clearing the dirty set, the backing filesystem synced.
setup_syncpoint() {
	touch "$MNT/d1/s1"
	sleep 3
	fd_freeze cache || fail sync-freeze-cache "FIFREEZE of the cache filesystem failed"
	ls "$MNT/d1" >/tmp/ls-sync.out 2>&1 &
	TOUCH_PID=$!
	if fd_blocked "$DAEMON_PID" "$CACHE_DIR"; then
		echo "fault_power.sh: held: $(fd_where "$DAEMON_PID")"
		pass sync-held
	else
		fail sync-held "the daemon was never held writing the cache database (or the request did not start a sync point)"
	fi
}
cut_syncpoint() { power_cut cache; }
check_syncpoint() {
	if [ -e "$SRC/d1/s1" ]; then
		pass sync-backing-kept
	else
		fail sync-backing-kept "d1/s1 was synced to the backing filesystem yet is gone"
	fi
	if [ "$(fd_recovered "$LOG")" -ge 1 ]; then
		pass sync-dirty-set-survived
	else
		fail sync-dirty-set-survived "the clearing of the dirty set was lost with the cut, yet no row was recovered: $(grep -i cleanly "$LOG")"
	fi
	served_equals_backing sync-served
}

# The flags dcfs runs the scenario with.
flags_for() {
	case "$1" in
	syncpoint) echo --sync_interval_sec=2 ;;
	esac
}

# --- kill mode: one scenario, two boots --------------------------------------

if [ "$CUT_MODE" = kill ]; then
	# A long journal commit interval, where the filesystem has one, so that
	# a create stays in the journal's running transaction until the cut.
	FD_LONG_COMMIT=1
	case "$BOOT" in
	1)
		fd_setup || {
			fail setup "wrapping or mounting the disks failed"
			exit "$FAILED"
		}
		make_tree
		if start $(flags_for "$SCENARIO"); then pass mount; else
			fail mount "daemon did not mount within 10s"
			exit "$FAILED"
		fi
		warm
		"setup_$SCENARIO"
		"cut_$SCENARIO"
		exit "$FAILED"
		;;
	2)
		after_cut "$SCENARIO"
		"check_$SCENARIO"
		;;
	*)
		fail boot "dcfs_boot= must be 1 or 2 in kill mode"
		exit "$FAILED"
		;;
	esac
	require_no_reclaim no-reclaim
	exit "$FAILED"
fi

# --- dm mode: all five scenarios in one boot ---------------------------------

fd_setup || {
	fail setup "wrapping or mounting the disks failed"
	exit "$FAILED"
}
make_tree
if start; then pass mount; else
	fail mount "daemon did not mount within 10s"
	exit "$FAILED"
fi
warm

# before: setup makes the cut and the create; no cut_ step in dm mode.
setup_before
after_cut before
check_before

for scenario in phase1 phase2 ahead frozen_unlink frozen_rename; do
	"setup_$scenario"
	"cut_$scenario"
	after_cut "$scenario"
	"check_$scenario"
done

# The sync point runs under a daemon with a short sync interval.
fd_crash
fd_umount_disks
mount "$(fault_dev "$FD_BACK")" "$SRC"
mount "$(fault_dev "$FD_CACHE")" "$CACHE_DIR"
if start --sync_interval_sec=2; then pass sync-run-mount; else
	fail sync-run-mount "daemon did not mount within 10s"
	exit "$FAILED"
fi
ls "$MNT/d1" "$MNT/d2" >/dev/null
setup_syncpoint
cut_syncpoint
after_cut syncpoint
check_syncpoint

# Self-check of the comparison: what it does not see, every check above passes
# for the wrong reason. After dcfs listed and stat'ed d1, each of these changes
# made behind its back must make it fail: a name, a mode, a link count. And on
# two trees that differ only in a file's contents (same size, mode, links), the
# snapshot differs.
ls "$MNT/d1" >/dev/null
stat "$MNT/d1/keep" >/dev/null
behind() {
	# $1: the check's name; $2: the change; $3: the undo
	eval "$2"
	if same_as_backing; then
		fail "comparison-detects-$1" "a change only the backing filesystem has went unnoticed"
	else
		pass "comparison-detects-$1"
	fi
	eval "$3"
}
behind name 'touch "$SRC/d1/behind-its-back"' 'rm "$SRC/d1/behind-its-back"'
behind mode 'chmod 600 "$SRC/d1/keep"' 'chmod 644 "$SRC/d1/keep"'
behind links 'ln "$SRC/d1/keep" "$SRC/d1/keep-link"' 'rm "$SRC/d1/keep-link"'
mkdir /tmp/snap-a /tmp/snap-b
echo aaaa >/tmp/snap-a/f
echo bbbb >/tmp/snap-b/f
if [ "$(snapshot /tmp/snap-a)" != "$(snapshot /tmp/snap-b)" ]; then
	pass snapshot-sees-contents
else
	fail snapshot-sees-contents "two files of one size and mode with other contents snapshot alike"
fi

require_no_reclaim no-reclaim
exit "$FAILED"
