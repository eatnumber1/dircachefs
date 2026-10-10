#!/bin/sh
# dcfs step 11.3: a failed BACKING operation leaves the record unknown (never
# cached as having succeeded), goes back to the caller as an error, and the
# cache is consistent once the device recovers (design.md, "The write-through
# protocol", phases 2 and 3; docs/plan/phases/11-crash-stress-failure-
# testing.md, 11.3). The gap table of what guest/fault_backing.sh and the
# other fault tests already cover is in test/qemu/README.md, "Fault
# injection"; this script closes the rest of it.
#
# Five failure modes, each one run over the same operations:
#
#   error-reads   every backing read fails (dm-flakey error_reads)
#   window        the same, for a window of 8 s that ends by itself (dm-flakey
#                 with an up and a down interval): nothing is switched back
#   error-writes  every backing write fails and the filesystem has shut down
#                 or aborted (dm-flakey error_writes; a journal meets the
#                 errors at its next commit: a change and a sync)
#   error-io      both
#   dead          there is no device behind the filesystem (dm-error), and
#                 the filesystem has shut down
#
# Per mode, on a tree of its own made on the backing filesystem first (names
# end in the mode's number, so a query by name finds one record):
#
#   1. dcfs's cache is primed (the parents listed, the targets looked up) and
#      the kernel's caches dropped, so every operation reaches the backing
#      filesystem for something it must now read or write;
#   2. the failure is injected;
#   3. the operations run through dcfs: lookup, getattr and readdir fills;
#      create, mkdir, unlink, rename, setattr, setxattr, link, symlink; a
#      write-through data write (and its fsync). Each must return an error
#      (a fill need only return the truth where reads still work: error-writes);
#   4. the cache database, read by `testutil sql` while the daemon is idle,
#      must not say any of them succeeded (a created name present, a removed
#      name absent, the new mode valid), a failed listing must not be marked
#      complete, and the daemon is alive (the checking build aborts on an
#      invariant broken by an error path);
#   5. the device recovers: the table back to healthy (the window ends by
#      itself) and, if the backing filesystem stayed failed (ext4 read-only,
#      xfs shut down, btrfs aborted), the daemon killed and the filesystem
#      remounted;
#   6. the same operations succeed, what dcfs serves equals the backing
#      filesystem's, and the daemon is alive.
#
# The lines `fault_recover:` give what each filesystem answered (the README's
# table of errors is made from them).
#
# Run as /tests/fault_recover.sh by guest/init when booted with
# dcfs_test=fault_recover.sh.
FAILED=0
. "$(dirname "$0")/lib.sh"
. "$(dirname "$0")/fault_lib.sh"
. "$(dirname "$0")/fault_dcfs_lib.sh"

DCFS=/bin/dcfs
TESTUTIL=/bin/testutil
SRC=/src
MNT=/mnt
DB=$CACHE_DIR/dcfs.db
DAEMON_PID=""
MOUNTED=0
RUN=0
LOGS=""
WRITER_PID=""
PIN_PIDS=""
# dcfs_pin=0 on the kernel command line: btrfs unpinned (the reproducer of the
# kernel warning, //test/qemu:fault_recover_btrfs_unpinned_test).
PIN=1
grep -q 'dcfs_pin=0' /proc/cmdline && PIN=0
MODES="error-reads window error-writes error-io dead"
# The window: up for UP seconds from the table switch, then down for DOWN.
WINDOW_UP=6
WINDOW_DOWN=20

cleanup() {
	rc=$?
	if [ "$rc" -ne 0 ] || [ "$FAILED" -ne 0 ]; then
		for log in $LOGS; do
			echo "--- dcfs stderr ($log) ---"
			cat "$log" 2>/dev/null
		done
	fi
	[ -n "$WRITER_PID" ] && kill -KILL "$WRITER_PID" 2>/dev/null
	for p in $PIN_PIDS; do kill -KILL "$p" 2>/dev/null; done
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

echo "fault_recover.sh: kernel $(uname -r)"
require_commands umount sync find stat diff sort md5sum head cat dd tr grep touch mv cut rm mkdir ln ls sleep kill awk timeout

alive() {
	[ -n "$DAEMON_PID" ] && kill -0 "$DAEMON_PID" 2>/dev/null
}

# start: a new run of dcfs (no periodic sync point).
start() {
	RUN=$((RUN + 1))
	LOG=/tmp/dcfs-$RUN.log
	LOGS="$LOGS $LOG"
	fd_start "$LOG" --sync_interval_sec=3600 "$@"
}

# sql QUERY: the cache database's answer, rows joined by ";" (read only, and
# only while the daemon is idle: a reader can make a sync point's checkpoint
# see SQLITE_BUSY).
sql() { "$TESTUTIL" sql "$DB" "$1" | tr '\n' ';'; }

# now: the guest's uptime in seconds, two decimals.
now() { cut -d' ' -f1 /proc/uptime; }

# sleep_until T: sleeps until the uptime is T.
sleep_until() {
	su_left=$(awk -v t="$1" -v n="$(now)" 'BEGIN { d = t - n; if (d < 0) d = 0; print d }')
	sleep "$su_left"
}

# quiet: the kernel's caches dropped without a sync (a sync would wait on, or
# fail with, the failed disk) and the daemon past the FORGETs that sent, so
# that the next request for a name reaches the daemon.
quiet() {
	echo 3 >/proc/sys/vm/drop_caches
	quiesce_daemon "$DAEMON_PID"
}

# stop_writer: ends the process holding a file open for writing.
stop_writer() {
	if [ -n "$WRITER_PID" ]; then
		kill -KILL "$WRITER_PID" 2>/dev/null
		wait "$WRITER_PID" 2>/dev/null
		WRITER_PID=""
	fi
}

# start_writer FILE: `testutil writehold` appends 4096 bytes to FILE and holds
# it open (a dirty page of the backing file, written by the kernel directly,
# the daemon not involved).
start_writer() {
	: >/tmp/writer.out
	"$TESTUTIL" writehold "$1" append "$WRITE_BYTES" >/tmp/writer.out 2>&1 &
	WRITER_PID=$!
	sw_n=0
	while ! grep -q READY /tmp/writer.out && [ "$sw_n" -lt 50 ]; do
		sleep 0.1
		sw_n=$((sw_n + 1))
	done
	grep -q READY /tmp/writer.out
}

# data_write: a write through dcfs to the data file, held open, and an fsync
# of it, both of which work.
data_write() {
	: >/tmp/fsync.out
	start_writer "$M/wd/data$S" && "$TESTUTIL" fsync "$M/wd/data$S" >/tmp/fsync.out 2>&1
}

# check_not NAME OUT PATTERN...: passes unless OUT matches one of the case patterns
# (one per argument): the records that would say an operation succeeded.
check_not() {
	cn_name=$1
	cn_out=$2
	shift 2
	for cn_bad in "$@"; do
		# shellcheck disable=SC2254 # a case pattern
		case "$cn_out" in
		$cn_bad)
			fail "$cn_name" "the database says '$cn_out' (matches '$cn_bad')"
			return 1
			;;
		esac
	done
	pass "$cn_name"
}

# gone_reply OUT: OUT says the object does not exist (ENOENT) or is stale
# (ESTALE). Every object of a cell exists, so such a reply is wrong. On xfs
# and btrfs a cold inode the device cannot read makes open_by_handle_at fail
# with ESTALE; dcfs used to forget the row and reply ESTALE or ENOENT for a
# name that exists. Since step 11.3b it asks the parent by name and replies
# EIO (the row kept, unknown) unless the name is positively gone.
gone_reply() {
	case "$1" in
	*"No such file"* | *ENOENT* | *ESTALE* | *"Stale file"*) return 0 ;;
	esac
	return 1
}

# error_cell NAME RC OUT: the cell NAME got an error reply OUT.
error_cell() {
	echo "fault_recover: $MODE: $1: error (rc $2): $3"
	if gone_reply "$3"; then
		fail "$1" "dcfs replied ENOENT or ESTALE for an existing file: $3"
	else
		pass "$1"
	fi
}

# fails NAME CMD: CMD, through dcfs, must return an error.
fails() {
	f_out=$(timeout 30 sh -c "$2" 2>&1)
	f_rc=$?
	if [ "$f_rc" -ne 0 ]; then
		error_cell "$1" "$f_rc" "$f_out"
		return 0
	fi
	fail "$1" "succeeded ($f_out) with the backing filesystem failing"
	return 1
}

# may_succeed NAME CMD: CMD, through dcfs, fails (a pass, or a skip for a gone
# reply) or works (a skip: it needed no device I/O, so the failure was not
# met; the record checks still run); returns 0 if it worked.
may_succeed() {
	ms_out=$(timeout 30 sh -c "$2" 2>&1)
	ms_rc=$?
	if [ "$ms_rc" -eq 0 ]; then
		skip "$1" "succeeded: it needed no device I/O, so no failure was met"
		echo "fault_recover: $MODE: $1: succeeded"
		return 0
	fi
	error_cell "$1" "$ms_rc" "$ms_out"
	return 1
}

# fill_fails NAME CMD TRUTH: a fill (lookup, getattr, readdir) returns an error
# (counted in FILL_ERRORS), or answers TRUTH, what the backing filesystem
# holds, from a cache it still has: xfs keeps an inode cluster, ext4 a
# directory block, after the caches were dropped. An answer with no read
# reaching the device is a SKIP (no failure was met), one after a read that
# did reach it a pass; an answer that is not the truth is a failure. Sets
# FILL_OK to 1 if it answered.
fill_fails() {
	ff_name=$1
	FILL_OK=0
	ff_before=$(sectors_read "$(fault_dev "$FD_BACK")")
	ff_out=$(timeout 30 sh -c "$2" 2>&1)
	ff_rc=$?
	ff_after=$(sectors_read "$(fault_dev "$FD_BACK")")
	if [ "$ff_rc" -ne 0 ]; then
		FILL_ERRORS=$((FILL_ERRORS + 1))
		error_cell "$ff_name" "$ff_rc" "$ff_out"
	elif [ "$ff_out" = "$3" ]; then
		FILL_OK=1
		echo "fault_recover: $MODE: $ff_name: answered the truth: $ff_out"
		if [ "$ff_after" = "$ff_before" ]; then
			skip "$ff_name" "answered from caches, no read reached the device"
		else
			pass "$ff_name"
		fi
	else
		fail "$ff_name" "answered '$ff_out', the backing filesystem holds '$3'"
	fi
}

# succeeds NAME CMD: CMD, through dcfs, must work.
succeeds() {
	s_out=$(timeout 30 sh -c "$2" 2>&1)
	s_rc=$?
	if [ "$s_rc" -eq 0 ]; then
		pass "$1"
	else
		fail "$1" "failed (rc $s_rc): $s_out"
	fi
}

# answers NAME CMD TRUTH: CMD, through dcfs, must answer TRUTH.
answers() {
	a_out=$(timeout 30 sh -c "$2" 2>&1)
	if [ "$a_out" = "$3" ]; then
		pass "$1"
	else
		fail "$1" "answered '$a_out', want '$3'"
	fi
}

# name_state NAME PARENT-INO: what the database says of NAME in the directory
# whose backing inode is PARENT-INO: the dentry's state, else norow-complete
# (no row, the directory complete: the name is recorded absent) or
# norow-incomplete (no row, so unknown). The dentry is found by NAME alone,
# which is safe because every name of the test ends in the number of its mode
# and no two directories of a tree share one; PARENT-INO is for the
# completeness of the directory only.
name_state() {
	sql "SELECT COALESCE((SELECT state FROM dentries WHERE name = CAST('$1' AS BLOB)), (SELECT 'norow-complete' FROM directories WHERE inode = (SELECT id FROM inodes WHERE backing_ino = $2) AND children_complete = 1), 'norow-incomplete')"
}

# daemon_ok NAME: the daemon is alive (the checking build aborts on a broken
# invariant, and run-qemu.sh fails the run on its marker).
daemon_ok() {
	if alive; then pass "$1"; else fail "$1" "the daemon died"; fi
}

# prep_tree: the operations' files on the backing filesystem, healthy, for
# the mode numbered S.
prep_tree() {
	mkdir "$SRC/$R"
	for d in lk rd cr mk ul rn sa sx ln sy wd; do mkdir "$SRC/$R/$d"; done
	echo known >"$SRC/$R/lk/known$S"
	echo aa >"$SRC/$R/rd/a$S"
	echo bb >"$SRC/$R/rd/b$S"
	echo x >"$SRC/$R/cr/x$S"
	echo x >"$SRC/$R/mk/x$S"
	echo victim >"$SRC/$R/ul/victim$S"
	echo src >"$SRC/$R/rn/src$S"
	echo plain >"$SRC/$R/sa/ch$S"
	chmod 644 "$SRC/$R/sa/ch$S"
	echo plain >"$SRC/$R/sx/xa$S"
	echo target >"$SRC/$R/ln/target$S"
	echo x >"$SRC/$R/sy/x$S"
	dd if=/dev/zero of="$SRC/$R/wd/data$S" bs=1k count=4 2>/dev/null
}

# inject: the failure of MODE, from now on.
inject() {
	case "$MODE" in
	error-reads) fault_mode "$FD_BACK" error-reads ;;
	error-writes) fault_mode "$FD_BACK" error-writes ;;
	error-io) fault_mode "$FD_BACK" error-io ;;
	dead) fault_mode "$FD_BACK" dead ;;
	window)
		fault_window "$FD_BACK" "$WINDOW_UP" "$WINDOW_DOWN" &&
			WINDOW_START=$FAULT_WINDOW_START
		;;
	esac
}

# pin_inodes: on btrfs, every inode of the mode's tree held in the backing
# filesystem's cache (O_PATH descriptors on the files, working directories for
# the directories) so that the failure meets metadata it must read, never the
# read of an inode, which warns in btrfs_destroy_inode (fs/btrfs/inode.c:8047)
# and the harness fails a boot on a kernel WARNING. The other filesystems do
# not. Cause (proven by tools/kernel_bugs/btrfs_failed_inode_read, analysis in
# docs/plan/notes/kernel-bugs-2026-10-09.md): a directory whose inode item was
# updated only in memory (a relatime atime update from a listing, here dcfs's
# `ls` of the parents) takes btrfs_fill_inode's fast path, which sets
# index_cnt = -1; when the on-disk read then fails (here through dcfs's
# open_by_handle_at), iget_failed's make_bad_inode sets i_mode = S_IFREG and
# btrfs_destroy_inode reads index_cnt as csum_bytes (shared storage since
# d9891ae28b0d, v6.11). A spurious warning on an expected error path, not a
# data bug.
pin_inodes() {
	: >/tmp/pin.out
	"$TESTUTIL" opath-hold-tree "$SRC/$R" >/tmp/pin.out 2>&1 &
	PIN_PIDS="$!"
	for pi_dir in "$SRC/$R" "$SRC/$R"/*; do
		[ -d "$pi_dir" ] || continue
		sh -c 'cd "$1" && exec sleep 100000' pin "$pi_dir" &
		PIN_PIDS="$PIN_PIDS $!"
	done
	pi_n=0
	while ! grep -q READY /tmp/pin.out && [ "$pi_n" -lt 50 ]; do
		sleep 0.1
		pi_n=$((pi_n + 1))
	done
	grep -q READY /tmp/pin.out || fail "$MODE-pin" "opath-hold-tree: $(cat /tmp/pin.out)"
}

unpin_inodes() {
	for ui_pid in $PIN_PIDS; do
		kill -KILL "$ui_pid" 2>/dev/null
		wait "$ui_pid" 2>/dev/null
	done
	PIN_PIDS=""
}

# kernel_warns: fails, saying "kernel: <the warning>", if the kernel has logged
# the btrfs_destroy_inode warning.
kernel_warns() {
	if dmesg | grep -q "WARNING: CPU.*btrfs_destroy_inode"; then
		echo "kernel: $(dmesg | grep -m 1 'WARNING: CPU.*btrfs_destroy_inode' | sed 's/^[^]]*] //')"
		return 1
	fi
	return 0
}

# remount_backing: the daemon killed, both filesystems mounted again on healthy
# devices (the journals replay), the daemon started over them (it recovers the
# dirty rows).
remount_backing() {
	RECOVERED_IN_PLACE=0
	stop_writer
	fd_crash
	fd_restore || fail "$MODE-remount" "cannot remount the healed filesystems"
	if start; then
		pass "$MODE-restart"
	else
		fail "$MODE-restart" "daemon did not mount within 10s"
		exit "$FAILED"
	fi
	echo "fault_recover: $MODE: recovered $(fd_recovered "$LOG") dirty rows"
}

# run_mode N MODE: steps 1 to 6 for one mode.
run_mode() {
	S=$1
	MODE=$2
	R=m$S
	M=$MNT/$R
	FSTYPE=$(backing_fstype "$SRC")
	WRITE_BYTES=4096
	FILL_ERRORS=0
	DATA_FSYNC_FAILED=0
	# testutil readdir-ino, not ls or find: busybox ls ends a listing at a
	# readdir error without saying so, and busybox find ignores ESTALE.
	LISTRD="l=\$($TESTUTIL readdir-ino $M/rd) || { echo \"\$l\"; exit 1; }; echo \"\$l\" | cut -d' ' -f1 | grep -v '^[.][.]*\$' | sort"
	echo "fault_recover: === $MODE ($(backing_fstype "$SRC"))"
	# 1. prime: parents listed, targets looked up; the lookup's and the
	# readdir's subjects and the getattr's are left for the failure.
	ls "$M/cr" "$M/mk" "$M/ul" "$M/rn" "$M/sa" "$M/sx" "$M/ln" "$M/sy" "$M/wd" >/dev/null
	stat "$M/lk" "$M/rd" >/dev/null
	stat "$M/ul/victim$S" "$M/rn/src$S" "$M/sa/ch$S" "$M/sx/xa$S" "$M/ln/target$S" "$M/wd/data$S" >/dev/null
	ino_ch=$(stat -c %i "$SRC/$R/sa/ch$S")
	ino_xa=$(stat -c %i "$SRC/$R/sx/xa$S")
	ino_ta=$(stat -c %i "$SRC/$R/ln/target$S")
	ino_rd=$(stat -c %i "$SRC/$R/rd")
	ino_wd=$(stat -c %i "$SRC/$R/wd/data$S")
	ino_ulp=$(stat -c %i "$SRC/$R/ul")
	ino_rnp=$(stat -c %i "$SRC/$R/rn")
	ino_lkp=$(stat -c %i "$SRC/$R/lk")
	ino_sap=$(stat -c %i "$SRC/$R/sa")
	ino_sxp=$(stat -c %i "$SRC/$R/sx")
	ino_lnp=$(stat -c %i "$SRC/$R/ln")
	ino_wdp=$(stat -c %i "$SRC/$R/wd")
	if start_writer "$M/wd/data$S"; then
		pass "$MODE-writer-open"
	else
		fail "$MODE-writer-open" "testutil writehold did not report READY: $(cat /tmp/writer.out)"
	fi
	# The writer's page stays dirty: the failure must meet it.
	[ "$FSTYPE" = btrfs ] && [ "$PIN" = 1 ] && pin_inodes
	quiet
	# 2. inject. A mode that fails writes needs the filesystem's journal to
	# meet the errors: a change and a sync (as guest/fault_backing.sh).
	if inject; then
		pass "$MODE-inject"
	else
		fail "$MODE-inject" "the table switch failed"
	fi
	case "$MODE" in
	window)
		# The window opens WINDOW_UP s after the switch: wait for it, then
		# drop the caches (no sync: writes work, but the point is reads).
		sleep_until "$(awk -v s="$WINDOW_START" -v u="$WINDOW_UP" 'BEGIN { print s + u + 0.5 }')"
		;;
	error-writes | error-io | dead)
		echo trigger >"$SRC/trigger$S" 2>/dev/null || true
		sync
		sleep 1
		;;
	esac
	echo 3 >/proc/sys/vm/drop_caches
	# 3. the operations.
	fill_fails "$MODE-lookup-fails" "stat -c %s $M/lk/known$S" 6
	fill_fails "$MODE-readdir-fails" "$LISTRD" "a$S
b$S"
	READDIR_OK=$FILL_OK
	fails "$MODE-create-fails" "touch $M/cr/new$S"
	fails "$MODE-mkdir-fails" "mkdir $M/mk/newdir$S"
	fails "$MODE-unlink-fails" "rm $M/ul/victim$S"
	fails "$MODE-rename-fails" "mv $M/rn/src$S $M/rn/dst$S"
	# A chmod and a setxattr of an inode the kernel still holds (dcfs pins it
	# with a descriptor) need no read, and a filesystem that has not yet
	# aborted takes them into its journal: they may succeed, and then the
	# record must say what the backing filesystem now holds (the next sync
	# meets the error).
	SETATTR_OK=0
	SETXATTR_OK=0
	PRE_MODE=644
	may_succeed "$MODE-setattr" "chmod 600 $M/sa/ch$S" && SETATTR_OK=1
	[ "$SETATTR_OK" = 1 ] && PRE_MODE=600
	fill_fails "$MODE-getattr-fails" "stat -c %a $M/sa/ch$S" "$PRE_MODE"
	may_succeed "$MODE-setxattr" "$TESTUTIL setxattr $M/sx/xa$S user.k v" && SETXATTR_OK=1
	fails "$MODE-link-fails" "ln $M/ln/target$S $M/ln/hard$S"
	fails "$MODE-symlink-fails" "ln -s target$S $M/sy/sym$S"
	# Write-through data: the writer's page is dirty. fsync of the file through
	# dcfs must say so where writes fail; where only reads fail it may still
	# need one (a block bitmap, to allocate), so either is recorded. A new
	# writable open of the file may work (the inode is held).
	out=$("$TESTUTIL" fsync "$M/wd/data$S" 2>&1)
	echo "fault_recover: $MODE: data fsync says: ${out:-ok}"
	case "$out" in
	ERR*)
		DATA_FSYNC_FAILED=1
		error_cell "$MODE-data-fsync-fails" 1 "$out"
		;;
	*)
		case "$MODE" in
		error-reads | window) skip "$MODE-data-fsync-fails" "the fsync worked: no write-back needed a read" ;;
		*) fail "$MODE-data-fsync-fails" "fsync of the written file returned '${out:-ok}' on a failed filesystem" ;;
		esac
		;;
	esac
	may_succeed "$MODE-data-open" "echo y >>$M/wd/data$S"
	# A mode that fails reads must be met by at least one fill (the rest may be
	# answered from caches).
	case "$MODE" in
	error-reads | window | error-io | dead)
		if [ "$FILL_ERRORS" -ge 1 ]; then
			pass "$MODE-some-fill-fails"
		else
			fail "$MODE-some-fill-fails" "no lookup, getattr or readdir met the failing reads"
		fi
		;;
	esac
	if [ "$MODE" = window ]; then
		if [ "$(awk -v n="$(now)" -v s="$WINDOW_START" -v u="$WINDOW_UP" -v d="$WINDOW_DOWN" 'BEGIN { print (n < s + u + d - 1) }')" = 1 ]; then
			pass window-operations-inside-the-window
		else
			fail window-operations-inside-the-window "the down interval ended (uptime $(now), window $WINDOW_START + $WINDOW_UP + $WINDOW_DOWN) while the operations ran"
		fi
	fi
	# 4. what the database says, while the daemon is idle.
	quiesce_daemon "$DAEMON_PID"
	daemon_ok "$MODE-daemon-alive-after-errors"
	# Every name the failed operations touched exists on the backing
	# filesystem, so none may be recorded absent (an unknown one is right).
	check_not "$MODE-lookup-not-recorded-absent" "$(name_state "known$S" "$ino_lkp")" '*absent*' '*norow-complete*'
	check_not "$MODE-listed-names-not-recorded-absent" "$(name_state "a$S" "$ino_rd")$(name_state "b$S" "$ino_rd")" '*absent*' '*norow-complete*'
	check_not "$MODE-setattr-name-not-recorded-absent" "$(name_state "ch$S" "$ino_sap")" '*absent*' '*norow-complete*'
	check_not "$MODE-setxattr-name-not-recorded-absent" "$(name_state "xa$S" "$ino_sxp")" '*absent*' '*norow-complete*'
	check_not "$MODE-link-target-not-recorded-absent" "$(name_state "target$S" "$ino_lnp")" '*absent*' '*norow-complete*'
	check_not "$MODE-data-name-not-recorded-absent" "$(name_state "data$S" "$ino_wdp")" '*absent*' '*norow-complete*'
	if [ "$READDIR_OK" = 1 ]; then
		pass "$MODE-readdir-answered-so-may-be-complete"
	else
		check_not "$MODE-readdir-not-marked-complete" "$(sql "SELECT children_complete FROM directories WHERE inode = (SELECT id FROM inodes WHERE backing_ino = $ino_rd)")" '1;'
	fi
	check_not "$MODE-create-not-recorded-present" "$(sql "SELECT state FROM dentries WHERE name = CAST('new$S' AS BLOB)")" '*present*'
	check_not "$MODE-mkdir-not-recorded-present" "$(sql "SELECT state FROM dentries WHERE name = CAST('newdir$S' AS BLOB)")" '*present*'
	check_not "$MODE-unlink-not-recorded-absent" "$(name_state "victim$S" "$ino_ulp")" '*absent*' '*norow-complete*'
	check_not "$MODE-rename-source-not-recorded-absent" "$(name_state "src$S" "$ino_rnp")" '*absent*' '*norow-complete*'
	check_not "$MODE-rename-target-not-recorded-present" "$(sql "SELECT state FROM dentries WHERE name = CAST('dst$S' AS BLOB)")" '*present*'
	if [ "$SETATTR_OK" = 1 ]; then
		check_not "$MODE-setattr-record-is-the-new-mode" "$(sql "SELECT 1 FROM inodes WHERE backing_ino = $ino_ch AND attrs_valid = 1 AND (mode & 4095) <> 384")" '1;'
	else
		# A record valid with the new mode is wrong unless the backing
		# filesystem itself holds that mode now (btrfs changes the inode in
		# memory before its transaction fails, and returns EIO).
		if [ "$(sql "SELECT 1 FROM inodes WHERE backing_ino = $ino_ch AND attrs_valid = 1 AND (mode & 4095) = 384")" != '1;' ]; then
			pass "$MODE-setattr-new-mode-not-valid"
		elif [ "$(stat -c %a "$SRC/$R/sa/ch$S" 2>&1)" = 600 ]; then
			echo "fault_recover: $MODE: the failed chmod left the backing filesystem's own inode at 600; the record says so"
			pass "$MODE-setattr-new-mode-not-valid"
		else
			fail "$MODE-setattr-new-mode-not-valid" "the record is valid with the new mode, the backing filesystem says '$(stat -c %a "$SRC/$R/sa/ch$S" 2>&1)'"
		fi
	fi
	if [ "$SETXATTR_OK" = 1 ]; then
		check_not "$MODE-setxattr-succeeded-not-recorded-absent" "$(sql "SELECT state FROM xattrs WHERE name = CAST('user.k' AS BLOB) AND inode = (SELECT id FROM inodes WHERE backing_ino = $ino_xa)")" '*absent*'
	else
		check_not "$MODE-setxattr-not-recorded-present" "$(sql "SELECT state FROM xattrs WHERE name = CAST('user.k' AS BLOB) AND inode = (SELECT id FROM inodes WHERE backing_ino = $ino_xa)")" '*present*'
	fi
	check_not "$MODE-link-not-recorded-present" "$(sql "SELECT state FROM dentries WHERE name = CAST('hard$S' AS BLOB)")" '*present*'
	check_not "$MODE-link-count-not-valid-as-2" "$(sql "SELECT nlink FROM inodes WHERE backing_ino = $ino_ta AND attrs_valid = 1 AND nlink = 2")" '2;'
	check_not "$MODE-symlink-not-recorded-present" "$(sql "SELECT state FROM dentries WHERE name = CAST('sym$S' AS BLOB)")" '*present*'
	check_not "$MODE-data-attributes-not-valid-while-open" "$(sql "SELECT attrs_valid FROM inodes WHERE backing_ino = $ino_wd")" '1;'
	# 5. the device recovers.
	stop_writer
	unpin_inodes
	case "$MODE" in
	window)
		sleep_until "$(awk -v s="$WINDOW_START" -v u="$WINDOW_UP" -v d="$WINDOW_DOWN" 'BEGIN { print s + u + d + 0.5 }')"
		# The table cycles: the next down interval must not meet the rest of
		# the run.
		fault_mode "$FD_BACK" healthy || fail "$MODE-heal" "fault_mode failed"
		;;
	*)
		fault_mode "$FD_BACK" healthy || fail "$MODE-heal" "fault_mode failed"
		;;
	esac
	if echo probe >"$SRC/probe$S" 2>/dev/null && sync; then
		echo "fault_recover: $MODE: the backing filesystem works again with the device, no remount"
		RECOVERED_IN_PLACE=1
	else
		echo "fault_recover: $MODE: the backing filesystem stays failed after the device recovered: remounting it"
		remount_backing
	fi
	if [ "$MODE" = window ]; then
		if [ "$RECOVERED_IN_PLACE" = 1 ]; then
			pass window-recovered-by-itself
		elif [ "$FSTYPE" = ext4 ]; then
			fail window-recovered-by-itself "ext4 needed a remount after the window ended"
		else
			skip window-recovered-by-itself "$FSTYPE stays failed after a read error: it needs a remount"
		fi
	fi
	# A write-back that failed lost the file's pages: the inode must still be
	# in the dirty set (no sync point cleared it), so that a restart's
	# recovery refreshes what dcfs recorded.
	if [ "$RECOVERED_IN_PLACE" = 1 ] && [ "$DATA_FSYNC_FAILED" = 1 ]; then
		check_not "$MODE-failed-write-inode-still-dirty" "$(sql "SELECT count(*) FROM dirty WHERE inode = (SELECT id FROM inodes WHERE backing_ino = $ino_wd)")" '0;'
	fi
	daemon_ok "$MODE-daemon-alive-after-recovery"
	# 6. the same operations, now.
	quiet
	answers "$MODE-lookup-after-recovery" "stat -c %s $M/lk/known$S" 6
	answers "$MODE-readdir-after-recovery" "$LISTRD" "a$S
b$S"
	succeeds "$MODE-create-after-recovery" "touch $M/cr/new$S"
	succeeds "$MODE-mkdir-after-recovery" "mkdir $M/mk/newdir$S"
	succeeds "$MODE-unlink-after-recovery" "rm $M/ul/victim$S"
	succeeds "$MODE-rename-after-recovery" "mv $M/rn/src$S $M/rn/dst$S"
	# What the backing filesystem holds now: the chmod that worked into a
	# journal that was then aborted is not on it after a remount.
	BACKING_MODE=$(stat -c %a "$SRC/$R/sa/ch$S")
	answers "$MODE-getattr-before-setattr" "stat -c %a $M/sa/ch$S" "$BACKING_MODE"
	succeeds "$MODE-setattr-after-recovery" "chmod 600 $M/sa/ch$S"
	answers "$MODE-getattr-after-recovery" "stat -c %a $M/sa/ch$S" 600
	succeeds "$MODE-setxattr-after-recovery" "$TESTUTIL setxattr $M/sx/xa$S user.k v"
	succeeds "$MODE-link-after-recovery" "ln $M/ln/target$S $M/ln/hard$S"
	succeeds "$MODE-symlink-after-recovery" "ln -s target$S $M/sy/sym$S"
	if data_write; then
		pass "$MODE-data-after-recovery"
	elif grep -q EUCLEAN /tmp/writer.out /tmp/fsync.out; then
		# ext4 marks a block group whose bitmap it could not read as
		# corrupt, in memory, and fails every allocation in it with
		# EUCLEAN until the filesystem is mounted again.
		echo "fault_recover: $MODE: ext4 still refuses to allocate in the group it could not read (EUCLEAN): remounting"
		remount_backing
		quiet
		if data_write; then
			pass "$MODE-data-after-remount"
		else
			fail "$MODE-data-after-remount" "write or fsync failed: $(cat /tmp/writer.out /tmp/fsync.out)"
		fi
	else
		fail "$MODE-data-after-recovery" "write or fsync failed: $(cat /tmp/writer.out /tmp/fsync.out)"
	fi
	stop_writer
	# What reached the backing filesystem is what was asked for now, and dcfs
	# serves it.
	[ -e "$SRC/$R/cr/new$S" ] && [ -d "$SRC/$R/mk/newdir$S" ] && [ ! -e "$SRC/$R/ul/victim$S" ] &&
		[ -e "$SRC/$R/rn/dst$S" ] && [ ! -e "$SRC/$R/rn/src$S" ] && [ "$(stat -c %a "$SRC/$R/sa/ch$S")" = 600 ] &&
		[ "$(stat -c %h "$SRC/$R/ln/target$S")" = 2 ] && [ "$(readlink "$SRC/$R/sy/sym$S")" = "target$S" ] &&
		[ "$("$TESTUTIL" getxattr "$SRC/$R/sx/xa$S" user.k)" = v ]
	if [ $? -eq 0 ]; then
		pass "$MODE-effects-on-backing"
	else
		fail "$MODE-effects-on-backing" "the operations after recovery did not all reach the backing filesystem"
	fi
	drop_caches
	snapshot "$SRC/$R" atime >/tmp/backing.snap 2>&1
	snapshot "$MNT/$R" atime >/tmp/served.snap 2>&1
	# Without a restart (nothing recovered the dirty rows) after a failed
	# write-back, the written file is compared on its own and its difference
	# printed: its pages were lost, the filesystem's own size reverts when the
	# inode is evicted, and dcfs serves the size it read until the restart's
	# recovery (the inode is still dirty: checked above). README.md,
	# "Limitations".
	if [ "$RECOVERED_IN_PLACE" = 1 ] && [ "$DATA_FSYNC_FAILED" = 1 ]; then
		grep "wd/data" /tmp/backing.snap >/tmp/backing.data
		grep "wd/data" /tmp/served.snap >/tmp/served.data
		if ! cmp -s /tmp/backing.data /tmp/served.data; then
			echo "fault_recover: $MODE: the written file after the failed write-back: served $(cat /tmp/served.data) / backing $(cat /tmp/backing.data)"
		fi
		grep -v "wd/data" /tmp/backing.snap >/tmp/backing.snap2
		grep -v "wd/data" /tmp/served.snap >/tmp/served.snap2
		mv /tmp/backing.snap2 /tmp/backing.snap
		mv /tmp/served.snap2 /tmp/served.snap
	fi
	if command diff /tmp/served.snap /tmp/backing.snap >/tmp/snap.diff 2>&1; then
		pass "$MODE-served-equals-backing"
	else
		fail "$MODE-served-equals-backing" "served (<) and backing (>) differ: $(tr '\n' '|' </tmp/snap.diff)"
	fi
	check_not "$MODE-no-unknown-name-left" "$(sql "SELECT CAST(name AS TEXT) FROM dentries WHERE state = 'unknown' AND name IN (CAST('new$S' AS BLOB), CAST('newdir$S' AS BLOB), CAST('dst$S' AS BLOB), CAST('hard$S' AS BLOB), CAST('sym$S' AS BLOB))")" '?*'
	daemon_ok "$MODE-daemon-alive-at-end"
}

main() {
	fd_setup || {
		fail setup "wrapping or mounting the disks failed"
		exit "$FAILED"
	}
	# Thousands of files first, so that what a mode's tree needs lies in
	# metadata that is not already in a page the filesystem reads whole (a
	# small btrfs tree is one leaf): the trees are made after the padding.
	mkdir "$SRC/pad"
	/bin/dcfs_bench mktree "$SRC/pad" 6000 0 >/dev/null 2>&1 || fail pad "could not make the padding tree"
	# Every tree before the first mode: a name made behind dcfs's back after
	# its parent was listed would be served as absent.
	n=0
	for mode in $MODES; do
		n=$((n + 1))
		S=$n
		R=m$S
		prep_tree
	done
	sync
	if start; then pass mount; else
		fail mount "daemon did not mount within 10s"
		exit "$FAILED"
	fi
	n=0
	for mode in $MODES; do
		n=$((n + 1))
		run_mode "$n" "$mode"
	done
	if [ "$PIN" = 0 ]; then
		disabled BTRFS_FAILED_INODE_READ_WARNS "btrfs's iget error path warns in btrfs_destroy_inode when a cold inode cannot be read (kernel 6.18)" kernel_warns
	fi
	require_no_reclaim no-reclaim
	exit "$FAILED"
}

main
