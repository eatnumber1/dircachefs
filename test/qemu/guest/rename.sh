#!/bin/sh
# dcfs step 4.3 acceptance test: the remove/rename write-through ops
# (unlink/rmdir/rename, including RENAME_NOREPLACE and RENAME_EXCHANGE) and
# the inode-row lifetime rule (a row goes away once its backing nlink is 0
# and dcfs holds no open file on it).
#
# Builds a small tree on vdb, mounts dcfs over it, warms the whole cache
# once, and then drives every op *through* /mnt. Each check is verified
# twice: against /src (the change really landed on the backing filesystem),
# and via /mnt after dropping the page/dentry/inode caches with *zero*
# sectors read from the backing device -- i.e. dcfs's phase 3 recorded the
# new state (including negative entries and restored directory
# completeness) without anything having to be re-read. For the ops that are
# expected to FAIL, phase 1 marks the names involved "unknown" (the
# write-through rule) and dcfs re-resolves them from the backing filesystem
# before replying, so those are held to the same zero-read standard.
# Finally, the daemon is killed and restarted against the same cache
# database and the results (and the warm cache) must persist.
#
# Also step 4.8's runtime submount refusal (amendment 12), using vdc as a
# second filesystem mounted below the source after dcfs starts: the
# boundary is a stub directory (step 23.5); a rename into it fails with
# ENOTSUP (the kernel looks the target up in the stub first), and renaming
# the stub itself with EXDEV (rename-boundary-refused, rename-into-boundary,
# rename-stub-exdev below) -- see README's Limitations and dcfs/backing.cc's
# ProbeChild/PopulateDirectory. (The startup-refusal half of amendment 12 is
# exercised once, in readonly.sh.)
#
# Uses only busybox applets plus //tools:testutil (renameat2 with explicit
# flags: busybox has no way to ask for RENAME_NOREPLACE/RENAME_EXCHANGE,
# and busybox mv silently falls back to copy+delete on EXDEV) and
# //tools:fhtest (handles, to observe row deletion from the guest).
#
# Run as /tests/rename.sh by guest/init when booted with dcfs_test=rename.sh.
FAILED=0
. "$(dirname "$0")/lib.sh"

DCFS=/bin/dcfs
FHTEST=/bin/fhtest
TESTUTIL=/bin/testutil

SRC=/src
MNT=/mnt
DB=/cache/dcfs.db
LOG1=/tmp/dcfs-1.log
LOG2=/tmp/dcfs-2.log

DAEMON_PID=""
MOUNTED=0

# See readonly.sh for why every cleanup command here is `|| true`-guarded.
cleanup() {
	rc=$?
	exec 3<&- 2>/dev/null
	if [ "$rc" -ne 0 ] || [ "$FAILED" -ne 0 ]; then
		echo "--- dcfs stderr (first run) ---"
		cat "$LOG1" 2>/dev/null
		echo "--- dcfs stderr (second run) ---"
		cat "$LOG2" 2>/dev/null
	fi
	if [ "$MOUNTED" -eq 1 ]; then
		umount "$MNT" 2>/dev/null || true
	fi
	if [ -n "$DAEMON_PID" ] && kill -0 "$DAEMON_PID" 2>/dev/null; then
		kill "$DAEMON_PID" 2>/dev/null || true
		wait "$DAEMON_PID" 2>/dev/null || true
	fi
}
trap cleanup EXIT

echo "rename.sh: kernel $(uname -r)"

# --- helpers -------------------------------------------------------------

# check NAME COND: evaluates the shell condition COND (which should only
# look at /src) and reports NAME-src.
check_src() {
	if eval "$2"; then
		pass "$1-src"
	else
		fail "$1-src" "condition failed: $2"
	fi
}

# check_cold NAME COND: drops every kernel cache, then evaluates COND (which
# must only look at /mnt, metadata only -- no content reads) and requires
# both that it holds and that neither backing device was read meanwhile.
# Reports NAME-mnt.
check_cold() {
	quiesce_backing
	drop_caches_quiesced
	b_vdb=$(sectors_read vdb)
	b_vdc=$(sectors_read vdc)
	if eval "$2"; then ok=1; else ok=0; fi
	a_vdb=$(sectors_read vdb)
	a_vdc=$(sectors_read vdc)
	if [ "$ok" -ne 1 ]; then
		fail "$1-mnt" "condition failed: $2"
	elif [ "$a_vdb" != "$b_vdb" ] || [ "$a_vdc" != "$b_vdc" ]; then
		fail "$1-mnt" "backing reads: vdb $b_vdb -> $a_vdb, vdc $b_vdc -> $a_vdc"
	else
		pass "$1-mnt"
	fi
}

# Runs "$@", expecting failure with $2 somewhere in its combined output.
expect_fail() {
	name=$1
	shift
	want=$1
	shift
	out=$("$@" 2>&1)
	rc=$?
	if [ "$rc" -eq 0 ]; then
		fail "$name" "unexpectedly succeeded"
		return
	fi
	case "$out" in
	*"$want"*) pass "$name" ;;
	*) fail "$name" "want '$want', got: $out" ;;
	esac
}

ino() { stat -c %i "$1" 2>/dev/null; }
nlink() { stat -c %h "$1" 2>/dev/null; }
absent() { ! ls -d "$1" >/dev/null 2>&1; }
# Whether directory $1's listing has entry $2.
listed() { ls -a "$1" 2>/dev/null | grep -qx "$2"; }

# "<type> <hex>" for $1's file handle (via /mnt), or "ERR".
handle_of() {
	out=$("$FHTEST" handle "$1") || true
	case "$out" in
	ERR* | "")
		echo "ERR"
		return 1
		;;
	esac
	set -- $out
	echo "$1 $3"
}
open_handle() {
	set -- $1
	"$FHTEST" open "$MNT" "$1" "$2" || true
}

error_lines() { grep -c '^E' "$LOG1" 2>/dev/null; }

normalize_stat() {
	file=$1
	prefix=$2
	while IFS= read -r line; do
		set -- $line
		if [ "$#" -lt 7 ]; then
			echo "$line"
			continue
		fi
		inode=$1
		perms=$2
		links=$3
		owner=$4
		group=$5
		size=$6
		shift 6
		rest="$*"
		rest=${rest#\'$prefix}
		rest=${rest#$prefix}
		echo "$inode $perms $links $owner $group $size $rest"
	done <"$file"
}

run_pass() {
	find "$1" -exec stat -c '%i %A %h %u %g %s %N' {} + >/tmp/pass_stat.txt
}

# --- build the backing tree (before dcfs ever sees it) ---------------------

mount /dev/vdb /src
# "d" is for rename-boundary-refused/rename-exdev below: it must never be
# listed through dcfs before vdc is mounted under it at runtime, and in
# particular must not be touched by the warm-up run_pass below.
mkdir -p /src/d

cd /src || exit 1
for f in f1 f2 f3 f5; do echo "content-$f" >"$f"; done
echo r1 >r1
echo r2 >r2
echo r3 >r3
echo r4 >r4
echo nr_a >nr_a
echo nr_b >nr_b
echo AAAA >ex_a
echo BBBB >ex_b
mkdir e1 n1 dA d1 d1/sub1 dsrc dempty
echo x >n1/x
echo a >d1/a
echo b >d1/b
echo c >d1/sub1/c
ln -s a d1/la
echo s >dsrc/child
cd /
sync

mkdir -p /cache /mnt
if start_daemon "$LOG1"; then
	pass mount
else
	fail mount "daemon did not mount within 10s"
	exit "$FAILED"
fi

# --- rename-boundary-refused: a filesystem mounted below the source at ----
# --- runtime is refused before the general warm-up below ever lists "d" --
# (amendment 12; see readonly.sh's boundary-* checks for the full
# explanation of why "d" must be listed through dcfs for the first time
# only after vdc is mounted under it.)
mkdir /src/d/mp
mount /dev/vdc /src/d/mp
listing=$(ls -1 "$MNT/d" 2>&1)
case "$listing" in
*mp*) pass boundary-listed-as-stub ;;
*) fail boundary-listed-as-stub "mp missing from /mnt/d: $listing" ;;
esac
errors=$(grep -c "refusing to cache mp" "$LOG1")
if [ "$errors" -eq 1 ]; then
	pass boundary-error-logged-once
else
	fail boundary-error-logged-once "want 1 ERROR line, got $errors"
fi
umount /src/d/mp

# Warm the whole cache once: every directory listed (so complete) and every
# entry's attributes cached. Everything below then starts from cached state.
run_pass "$MNT"

# --- unlink ------------------------------------------------------------

if rm "$MNT/f1"; then
	check_src unlink 'absent /src/f1'
	check_cold unlink 'absent /mnt/f1 &&
		ls /mnt/f1 2>&1 | grep -q "No such file" &&
		! listed /mnt f1 && listed /mnt f2'
else
	fail unlink "rm failed"
fi

# --- unlink-hardlink -------------------------------------------------------

if ln "$MNT/f2" "$MNT/f2h" && rm "$MNT/f2h"; then
	check_src unlink-hardlink '[ "$(nlink /src/f2)" = 1 ] && absent /src/f2h'
	check_cold unlink-hardlink '[ "$(nlink /mnt/f2)" = 1 ] &&
		absent /mnt/f2h && ! listed /mnt f2h'
else
	fail unlink-hardlink "ln or rm failed"
fi

# --- unlink-open: the row outlives the last link while dcfs has the file
# open, and goes away with the last close ----------------------------------

F3_HANDLE=$(handle_of "$MNT/f3")
errs_before=$(error_lines)
exec 3<"$MNT/f3"
if rm "$MNT/f3"; then
	content=$(cat <&3)
	if [ "$content" = "content-f3" ]; then
		pass unlink-open-read
	else
		fail unlink-open-read "read '$content' from the open fd"
	fi
	if absent /mnt/f3 && absent /src/f3; then
		pass unlink-open-gone
	else
		fail unlink-open-gone "f3 still visible after rm"
	fi
	# (Not checked here: opening F3_HANDLE while fd 3 is still open. That
	# is a second concurrent open of the inode, which dcfs's Open currently
	# gives its own passthrough backing file; the kernel rejects a second,
	# different backing file for one inode with EIO. A pre-existing
	# passthrough limitation, independent of unlink.)
else
	fail unlink-open "rm failed"
fi
exec 3<&-
if ls "$MNT" >/dev/null && [ "$(error_lines)" = "$errs_before" ]; then
	pass unlink-open-close
else
	fail unlink-open-close "ls failed or daemon logged errors"
fi
# Closed with nlink 0: Release deleted the row, so the handle is stale.
res=$(open_handle "$F3_HANDLE")
case "$res" in
"ERR ESTALE") pass unlink-open-row-deleted ;;
*) fail unlink-open-row-deleted "fhtest open -> '$res' (want ERR ESTALE)" ;;
esac
check_cold unlink-open 'absent /mnt/f3 && ! listed /mnt f3'

# --- rmdir -------------------------------------------------------------

E1_HANDLE=$(handle_of "$MNT/e1")
if rmdir "$MNT/e1"; then
	check_src rmdir-empty 'absent /src/e1'
	check_cold rmdir-empty 'absent /mnt/e1 && ! listed /mnt e1'
	res=$(open_handle "$E1_HANDLE")
	case "$res" in
	"ERR ESTALE") pass rmdir-empty-row-deleted ;;
	*) fail rmdir-empty-row-deleted "fhtest open -> '$res' (want ERR ESTALE)" ;;
	esac
else
	fail rmdir-empty "rmdir failed"
fi

expect_fail rmdir-enotempty "not empty" rmdir "$MNT/n1"
check_src rmdir-enotempty '[ -f /src/n1/x ]'
# The failed rmdir must leave n1 fully usable through the kernel's
# still-cached dentry (its ".." included).
if ls "$MNT/n1" >/dev/null; then
	pass rmdir-enotempty-dir-usable
else
	fail rmdir-enotempty-dir-usable "ls /mnt/n1 failed after the failed rmdir"
fi
check_cold rmdir-enotempty '[ -d /mnt/n1 ] && listed /mnt/n1 x'

expect_fail rmdir-enoent "No such file" rmdir "$MNT/nonexistent"

# --- rename ------------------------------------------------------------

R1_INO=$(ino /src/r1)
if mv "$MNT/r1" "$MNT/r1b"; then
	check_src rename-same-dir 'absent /src/r1 && [ "$(ino /src/r1b)" = "$R1_INO" ]'
	check_cold rename-same-dir 'absent /mnt/r1 &&
		[ "$(ino /mnt/r1b)" = "$R1_INO" ] &&
		listed /mnt r1b && ! listed /mnt r1'
else
	fail rename-same-dir "mv failed"
fi

R2_INO=$(ino /src/r2)
if mv "$MNT/r2" "$MNT/dA/r2"; then
	check_src rename-across-dirs 'absent /src/r2 && [ "$(ino /src/dA/r2)" = "$R2_INO" ]'
	check_cold rename-across-dirs 'absent /mnt/r2 &&
		[ "$(ino /mnt/dA/r2)" = "$R2_INO" ] &&
		listed /mnt/dA r2 && ! listed /mnt r2'
else
	fail rename-across-dirs "mv failed"
fi

R3_INO=$(ino /src/r3)
R4_INO=$(ino /src/r4)
R4_HANDLE=$(handle_of "$MNT/r4")
if mv "$MNT/r3" "$MNT/r4"; then
	check_src rename-replace 'absent /src/r3 && [ "$(ino /src/r4)" = "$R3_INO" ] &&
		[ "$(cat /src/r4)" = r3 ]'
	check_cold rename-replace 'absent /mnt/r3 &&
		[ "$(ino /mnt/r4)" = "$R3_INO" ] && [ "$(nlink /mnt/r4)" = 1 ] &&
		! listed /mnt r3'
	# The replaced r4's last link is gone and nothing had it open: its row
	# must have been deleted.
	res=$(open_handle "$R4_HANDLE")
	case "$res" in
	"ERR ESTALE") pass rename-replace-old-target-gone ;;
	*) fail rename-replace-old-target-gone "fhtest open -> '$res' (want ERR ESTALE)" ;;
	esac
else
	fail rename-replace "mv failed"
fi

NRA_INO=$(ino /src/nr_a)
NRB_INO=$(ino /src/nr_b)
out=$("$TESTUTIL" rename2 "$MNT/nr_a" "$MNT/nr_b" noreplace)
if [ "$out" = "ERR EEXIST" ]; then
	pass rename-noreplace-eexist
else
	fail rename-noreplace-eexist "testutil rename2 noreplace -> '$out'"
fi
check_src rename-noreplace-eexist '[ "$(ino /src/nr_a)" = "$NRA_INO" ] &&
	[ "$(ino /src/nr_b)" = "$NRB_INO" ] &&
	[ "$(cat /src/nr_a)" = nr_a ] && [ "$(cat /src/nr_b)" = nr_b ]'
check_cold rename-noreplace-eexist '[ "$(ino /mnt/nr_a)" = "$NRA_INO" ] &&
	[ "$(ino /mnt/nr_b)" = "$NRB_INO" ]'

EXA_INO=$(ino /src/ex_a)
EXB_INO=$(ino /src/ex_b)
out=$("$TESTUTIL" rename2 "$MNT/ex_a" "$MNT/ex_b" exchange)
if [ -z "$out" ]; then
	check_src rename-exchange '[ "$(cat /src/ex_a)" = BBBB ] &&
		[ "$(cat /src/ex_b)" = AAAA ] &&
		[ "$(ino /src/ex_a)" = "$EXB_INO" ] && [ "$(ino /src/ex_b)" = "$EXA_INO" ]'
	if [ "$(cat /mnt/ex_a)" = BBBB ] && [ "$(cat /mnt/ex_b)" = AAAA ]; then
		pass rename-exchange-content
	else
		fail rename-exchange-content "contents via /mnt not swapped"
	fi
	check_cold rename-exchange '[ "$(ino /mnt/ex_a)" = "$EXB_INO" ] &&
		[ "$(ino /mnt/ex_b)" = "$EXA_INO" ]'
else
	fail rename-exchange "testutil rename2 exchange -> '$out'"
fi

# A directory rename moves only its own dentry: the whole cached subtree
# must come along with it, served with zero backing reads.
find "$MNT/d1" -exec stat -c '%i %n' {} + | sed "s|^\([0-9]*\) $MNT/d1|\1 |" |
	sort >/tmp/d1_before.txt
if mv "$MNT/d1" "$MNT/d2"; then
	check_src rename-dir-with-children 'absent /src/d1 && [ -f /src/d2/sub1/c ]'
	check_cold rename-dir-with-children 'absent /mnt/d1 &&
		find /mnt/d2 -exec stat -c "%i %n" {} + |
			sed "s|^\([0-9]*\) /mnt/d2|\1 |" | sort >/tmp/d2_after.txt &&
		[ -s /tmp/d2_after.txt ] && cmp -s /tmp/d1_before.txt /tmp/d2_after.txt &&
		[ "$(stat -c %i /mnt/d2/sub1/..)" = "$(stat -c %i /mnt/d2)" ] &&
		[ "$(readlink /mnt/d2/la)" = a ]'
else
	fail rename-dir-with-children "mv failed"
fi

DSRC_INO=$(ino /src/dsrc)
DEMPTY_HANDLE=$(handle_of "$MNT/dempty")
out=$("$TESTUTIL" rename2 "$MNT/dsrc" "$MNT/dempty" 0)
if [ -z "$out" ]; then
	check_src rename-dir-over-empty-dir 'absent /src/dsrc &&
		[ "$(ino /src/dempty)" = "$DSRC_INO" ] && [ -f /src/dempty/child ]'
	check_cold rename-dir-over-empty-dir 'absent /mnt/dsrc &&
		[ "$(ino /mnt/dempty)" = "$DSRC_INO" ] && listed /mnt/dempty child'
	res=$(open_handle "$DEMPTY_HANDLE")
	case "$res" in
	"ERR ESTALE") pass rename-dir-over-empty-dir-old-gone ;;
	*) fail rename-dir-over-empty-dir-old-gone "fhtest open -> '$res' (want ERR ESTALE)" ;;
	esac
else
	fail rename-dir-over-empty-dir "testutil rename2 -> '$out'"
fi

# Renaming into a boundary stub fails with ENOTSUP: the kernel looks the
# target name up in the stub before sending the rename, and the stub
# refuses every lookup inside it (see create.sh's mkdir-boundary-refused
# for the same reasoning). Renaming the stub itself reaches dcfs, which
# refuses it with EXDEV. Both reuse the refusal boundary-* above already
# established ("d" is already cached complete with "mp" a stub), so they
# need no fresh mount.
out=$("$TESTUTIL" rename2 "$MNT/f5" "$MNT/d/mp/f5" 0)
if [ "$out" = "ERR EOPNOTSUPP" ]; then
	pass rename-into-boundary
else
	fail rename-into-boundary "testutil rename2 into a boundary stub -> '$out'"
fi
check_src rename-into-boundary '[ -f /src/f5 ] && absent /src/d/mp/f5'
check_cold rename-into-boundary '[ -f /mnt/f5 ] && absent /mnt/d/mp/f5'
out=$("$TESTUTIL" rename2 "$MNT/d/mp" "$MNT/d/mp2" 0)
if [ "$out" = "ERR EXDEV" ]; then
	pass rename-stub-exdev
else
	fail rename-stub-exdev "testutil rename2 of a boundary stub -> '$out'"
fi
check_src rename-stub-exdev '[ -d /src/d/mp ] && absent /src/d/mp2'

# --- listing-matches: "d" is excluded -- see readonly.sh's identity-checks
# comment on why /src/d and the cached /mnt/d deliberately diverge after the
# boundary-* checks above. -------------------------------------------------

find /src -path /src/d -prune -o -exec stat -c '%i %A %h %u %g %s %N' {} + >/tmp/src_stat.txt
find /mnt -path /mnt/d -prune -o -exec stat -c '%i %A %h %u %g %s %N' {} + >/tmp/mnt_stat.txt
normalize_stat /tmp/src_stat.txt "$SRC" | sort >/tmp/src_stat_norm.txt
normalize_stat /tmp/mnt_stat.txt "$MNT" | sort >/tmp/mnt_stat_norm.txt
if cmp -s /tmp/src_stat_norm.txt /tmp/mnt_stat_norm.txt; then
	pass listing-matches
else
	fail listing-matches "normalized stat listing differs"
	echo "--- src_stat_norm.txt ---"
	cat /tmp/src_stat_norm.txt
	echo "--- mnt_stat_norm.txt ---"
	cat /tmp/mnt_stat_norm.txt
fi

# --- warm-after-all ------------------------------------------------------

drop_caches_quiesced
before_vdb=$(sectors_read vdb)
run_pass "$MNT"
after_vdb=$(sectors_read vdb)
if [ "$after_vdb" = "$before_vdb" ]; then
	pass warm-after-all-vdb
else
	fail warm-after-all-vdb "sectors_read(vdb) $before_vdb -> $after_vdb"
fi
cp /tmp/pass_stat.txt /tmp/pass_before_restart.txt

# --- restart-persists ------------------------------------------------------

kill -TERM "$DAEMON_PID" 2>/dev/null || true
wait "$DAEMON_PID" 2>/dev/null || true
DAEMON_PID=""
if is_mounted "$MNT"; then
	echo "rename.sh: /mnt still mounted after SIGTERM; forcing umount"
	umount "$MNT" 2>/dev/null || true
fi
if is_mounted "$MNT"; then
	fail restart-unmount "mountpoint still mounted after kill+umount"
	MOUNTED=1
else
	pass restart-unmount
	MOUNTED=0
fi

if start_daemon "$LOG2"; then
	pass restart-mount
else
	fail restart-mount "daemon did not remount within 10s"
	exit "$FAILED"
fi

drop_caches_quiesced
before_vdb=$(sectors_read vdb)
run_pass "$MNT"
after_vdb=$(sectors_read vdb)
if cmp -s /tmp/pass_before_restart.txt /tmp/pass_stat.txt &&
	absent /mnt/f1 && absent /mnt/f3 && absent /mnt/e1 && absent /mnt/d1 &&
	[ "$(ino /mnt/r4)" = "$R3_INO" ] && [ "$(ino /mnt/ex_a)" = "$EXB_INO" ]; then
	pass restart-persists
else
	fail restart-persists "tree differs after restart"
fi
if [ "$after_vdb" = "$before_vdb" ]; then
	pass restart-warm
else
	fail restart-warm "vdb $before_vdb -> $after_vdb"
fi

# --- phase3-failure-still-succeeds (audit-races F7) --------------------------
#
# Once the backing rename has happened, a failure of dcfs's own bookkeeping
# afterwards (here: its cache database is locked by someone else for longer
# than its busy timeout) must not be reported as the rename failing: the
# caller would believe nothing happened, and the kernel would keep the old
# names. /src is frozen so that the rename sits inside its backing syscall
# (phase 1 already committed) while the database lock is taken.
echo p3 >"$MNT/p3_a"
if "$TESTUTIL" fsfreeze "$SRC" freeze; then
	mv "$MNT/p3_a" "$MNT/p3_b" 2>/tmp/p3_mv.err &
	MV_PID=$!
	sleep 1
	"$TESTUTIL" sqlite-lock "$DB" 8 >/tmp/p3_lock.out 2>&1 &
	LOCK_PID=$!
	i=0
	while [ "$i" -lt 10 ] && ! grep -q READY /tmp/p3_lock.out 2>/dev/null; do
		i=$((i + 1))
		sleep 1
	done
	"$TESTUTIL" fsfreeze "$SRC" thaw
	wait "$MV_PID"
	mv_rc=$?
	wait "$LOCK_PID" 2>/dev/null || true
	if [ -e "$SRC/p3_b" ] && [ ! -e "$SRC/p3_a" ]; then
		pass phase3-failure-renamed-src
	else
		fail phase3-failure-renamed-src "the backing rename did not happen"
	fi
	if [ "$mv_rc" -eq 0 ] && [ -e "$MNT/p3_b" ] && [ ! -e "$MNT/p3_a" ]; then
		pass phase3-failure-still-succeeds
	else
		fail phase3-failure-still-succeeds "mv rc=$mv_rc ($(cat /tmp/p3_mv.err)); mnt: $(ls "$MNT" | grep p3_)"
	fi
else
	skip phase3-failure-still-succeeds "cannot freeze $SRC"
fi

exit "$FAILED"
