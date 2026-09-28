#!/bin/sh
# dcfs step 4.1 acceptance test: Setattr (chmod/chown/truncate/utimes)
# write-through.
#
# Builds a small backing tree on vdb (a regular file f1, a directory d1, a
# symlink l1 -> f1, and a fifo p1), mounts dcfs over it, and exercises every
# FUSE_SET_ATTR_* combination Setattr handles: mode (on a regular file, a
# directory, and a fifo -- the three dispatch paths in backing::SetAttr --
# plus EOPNOTSUPP on a symlink), uid/gid, size (shrink, grow, and EISDIR on
# a directory), and atime/mtime (an explicit timestamp, "now", and a
# nanosecond-precision timestamp). Every change is checked three ways:
# immediately via /mnt, against /src (the write actually landed on the
# backing filesystem), and again via /mnt after
# `sync; echo 3 >/proc/sys/vm/drop_caches` with vdb's sectors-read counter
# unchanged (the re-read came from dcfs's own cache, not the disk). A
# final whole-tree pass repeats that warm check across every path at once,
# and the whole thing is repeated once more after killing and restarting
# the daemon against the same cache database, to confirm the write-through
# state (not just the read cache) survives a restart.
#
# Uses only busybox applets/options (verified against the exact busybox
# baked into the initramfs: `busybox --list`, `busybox <applet> --help`) --
# no GNU find/stat/coreutils extensions -- plus /bin/testutil (see
# //tools:testutil) for the handful of things busybox's applets cannot do
# precisely enough: busybox `truncate -s N FILE` goes through
# open(O_WRONLY)+ftruncate, and DirCacheFS::Open refuses any non-read-only
# open with EROFS until step 4.4, so it never reaches
# Setattr(FUSE_SET_ATTR_SIZE) at all -- `testutil truncate` calls
# truncate(2) directly, with no open() in between; and busybox chmod has
# no -h/--no-dereference, so it can never target a symlink itself (chmod(2)
# always follows symlinks) -- `testutil lchmod` uses fchmodat(2) with
# AT_SYMLINK_NOFOLLOW instead.
#
# Run as /tests/setattr.sh by guest/init when booted with
# dcfs_test=setattr.sh; prints one "TEST ... PASS/FAIL" line per check and
# exits nonzero if any check failed. init turns that into the final
# ALL-TESTS-PASSED / TEST-FAILED verdict.
FAILED=0
pass() { echo "TEST $1 PASS"; }
fail() { echo "TEST $1 FAIL ($2)"; FAILED=1; }

DCFS=/bin/dcfs
TESTUTIL=/bin/testutil

# busybox on Ubuntu lacks the mountpoint applet; ask the kernel directly.
is_mounted() { grep -q " $1 " /proc/mounts; }
SRC=/src
MNT=/mnt
DB=/cache/dcfs.db
LOG1=/tmp/dcfs-1.log
LOG2=/tmp/dcfs-2.log
CONTENT="Hello, dcfs setattr test content"

DAEMON_PID=""
MOUNTED=0

# See readonly.sh/passthrough.sh for why every command here is
# `|| true`-guarded: this must run to completion (and dump both daemon
# logs on any failure) whether the script is exiting via a tracked fail()
# or via `set -e` (imposed by init's `sh -e`) aborting on something
# unexpected.
cleanup() {
	rc=$?
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

echo "setattr.sh: kernel $(uname -r)"

# --- helpers -----------------------------------------------------------

# Field 3 of /sys/block/<dev>/stat is the cumulative count of sectors read
# from that block device since boot -- see Documentation/ABI/stable/
# sysfs-block.
sectors_read() {
	read -r line <"/sys/block/$1/stat"
	set -- $line
	echo "$3"
}

drop_caches() {
	sync
	echo 3 >/proc/sys/vm/drop_caches
}

# Starts the daemon, logging its stderr to $1, and waits up to 10s for the
# mount to appear. Returns nonzero (and leaves MOUNTED=0) if it doesn't.
start_daemon() {
	"$DCFS" --source="$SRC" --cache_db="$DB" "$MNT" >"$1" 2>&1 &
	DAEMON_PID=$!
	MOUNTED=0
	i=0
	while [ "$i" -lt 10 ]; do
		if is_mounted "$MNT"; then
			MOUNTED=1
			return 0
		fi
		if ! kill -0 "$DAEMON_PID" 2>/dev/null; then
			return 1
		fi
		i=$((i + 1))
		sleep 1
	done
	return 1
}

# Compares the same stat(1) field between $2 (an /mnt path) and $3 (the
# corresponding /src path): a change made through dcfs must actually have
# landed on the backing filesystem.
verify_immediate() {
	desc=$1
	mnt_path=$2
	src_path=$3
	fmt=$4
	mnt_val=$(stat -c "$fmt" "$mnt_path" 2>&1)
	src_val=$(stat -c "$fmt" "$src_path" 2>&1)
	if [ "$mnt_val" = "$src_val" ]; then
		pass "$desc"
	else
		fail "$desc" "mnt='$mnt_val' src='$src_val'"
	fi
}

# Drops every cache, then re-reads $2 (an /mnt path) via stat(1) and checks
# both that it still reports $4 (the expected value for format $3) and
# that doing so moved vdb's sectors-read counter by zero: the answer came
# from dcfs's own sqlite cache, not a fresh read of the backing device.
verify_cached() {
	desc=$1
	path=$2
	fmt=$3
	expected=$4
	drop_caches
	before=$(sectors_read vdb)
	got=$(stat -c "$fmt" "$path" 2>&1)
	after=$(sectors_read vdb)
	delta=$((after - before))
	if [ "$got" = "$expected" ] && [ "$delta" -eq 0 ]; then
		pass "$desc"
	else
		fail "$desc" "got='$got' want='$expected' sectors_delta=$delta"
	fi
}

# A whole-tree version of verify_cached: every path's stat line, taken
# twice with drop_caches in between, must be identical and the second pass
# must move zero sectors.
verify_tree_cached() {
	desc=$1
	drop_caches
	find "$MNT" -exec stat -c '%a %u %g %s %Y %n' {} + | sort >/tmp/setattr-pass1.txt
	before=$(sectors_read vdb)
	find "$MNT" -exec stat -c '%a %u %g %s %Y %n' {} + | sort >/tmp/setattr-pass2.txt
	after=$(sectors_read vdb)
	delta=$((after - before))
	if command diff -q /tmp/setattr-pass1.txt /tmp/setattr-pass2.txt >/dev/null \
			&& [ "$delta" -eq 0 ]; then
		pass "$desc"
	else
		fail "$desc" "sectors_delta=$delta; diff: $(command diff /tmp/setattr-pass1.txt /tmp/setattr-pass2.txt)"
	fi
}

# --- build the backing tree ---------------------------------------------

mount /dev/vdb /src
printf '%s' "$CONTENT" >"$SRC/f1"
mkdir -p "$SRC/d1"
ln -s f1 "$SRC/l1"
mkfifo "$SRC/p1"
sync

mkdir -p /cache /mnt
if start_daemon "$LOG1"; then
	pass mount
else
	fail mount "daemon did not mount within 10s"
	exit "$FAILED"
fi

# --- chmod-file: mode on a regular file (ReopenPathFd + fchmod) ---------

chmod 640 "$MNT/f1"
verify_immediate chmod-file-matches-src "$MNT/f1" "$SRC/f1" '%a'
verify_cached chmod-file-cached "$MNT/f1" '%a' 640

# --- chmod-dir: mode on a directory (same dispatch path as a file) ------

chmod 750 "$MNT/d1"
verify_immediate chmod-dir-matches-src "$MNT/d1" "$SRC/d1" '%a'
verify_cached chmod-dir-cached "$MNT/d1" '%a' 750

# --- chmod-fifo: mode on a fifo (fchmod_opath) ---------------------------

chmod 600 "$MNT/p1"
verify_immediate chmod-fifo-matches-src "$MNT/p1" "$SRC/p1" '%a'
verify_cached chmod-fifo-cached "$MNT/p1" '%a' 600

# --- chown: uid/gid via fchownat(fd, "", ..., AT_EMPTY_PATH) -------------

chown 1000:1000 "$MNT/f1"
verify_immediate chown-matches-src "$MNT/f1" "$SRC/f1" '%u:%g'
verify_cached chown-cached "$MNT/f1" '%u:%g' 1000:1000

# --- truncate-shrink: size down, content check too -----------------------

# busybox's own `truncate -s N FILE` goes through open(O_WRONLY) first,
# which Open() refuses (EROFS) until step 4.4 -- it would never actually
# exercise Setattr, and its exit status would need checking to notice
# that. testutil calls truncate(2) directly, with no open() at all.
out=$("$TESTUTIL" truncate "$MNT/f1" 3)
rc=$?
if [ "$rc" -eq 0 ]; then
	pass truncate-shrink-tool
else
	fail truncate-shrink-tool "rc=$rc out='$out'"
fi
verify_immediate truncate-shrink-matches-src "$MNT/f1" "$SRC/f1" '%s'
verify_cached truncate-shrink-cached "$MNT/f1" '%s' 3

drop_caches
before=$(sectors_read vdb)
shrunk=$(cat "$MNT/f1")
after=$(sectors_read vdb)
want=$(printf '%s' "$CONTENT" | head -c 3)
# Reading a file's *content* (unlike its metadata) always goes through
# open/read/passthrough against the backing device -- see passthrough.sh
# -- so this deliberately does not also assert a sectors_read delta of 0.
if [ "$shrunk" = "$want" ]; then
	pass truncate-shrink-content
else
	fail truncate-shrink-content "got='$shrunk' want='$want'"
fi

# --- truncate-grow: size up -----------------------------------------------

out=$("$TESTUTIL" truncate "$MNT/f1" 100)
rc=$?
if [ "$rc" -eq 0 ]; then
	pass truncate-grow-tool
else
	fail truncate-grow-tool "rc=$rc out='$out'"
fi
verify_immediate truncate-grow-matches-src "$MNT/f1" "$SRC/f1" '%s'
verify_cached truncate-grow-cached "$MNT/f1" '%s' 100

# --- truncate-dir-eisdir: truncating a directory must fail ---------------

out=$("$TESTUTIL" truncate "$MNT/d1" 0)
rc=$?
if [ "$rc" -ne 0 ] && [ "$out" = "ERR EISDIR" ]; then
	pass truncate-dir-eisdir
else
	fail truncate-dir-eisdir "rc=$rc out='$out' (want nonzero rc, 'ERR EISDIR')"
fi

# --- utimes: an explicit atime/mtime --------------------------------------

DATESPEC="2001-02-03 04:05:06"
want_epoch=$(date -d "$DATESPEC" +%s)
touch -d "$DATESPEC" "$MNT/f1"
verify_immediate utimes-matches-src "$MNT/f1" "$SRC/f1" '%Y'
verify_cached utimes-cached "$MNT/f1" '%Y' "$want_epoch"

got_epoch=$(stat -c '%Y' "$MNT/f1")
if [ "$got_epoch" = "$want_epoch" ]; then
	pass utimes-expected-epoch
else
	fail utimes-expected-epoch "got=$got_epoch want=$want_epoch"
fi

# --- touch-now: mtime advances past the value utimes just set ------------

prev_epoch=$got_epoch
sleep 1
touch "$MNT/f1"
new_epoch=$(stat -c '%Y' "$MNT/f1")
if [ "$new_epoch" -gt "$prev_epoch" ]; then
	pass touch-now
else
	fail touch-now "mtime did not advance: prev=$prev_epoch new=$new_epoch"
fi
verify_immediate touch-now-matches-src "$MNT/f1" "$SRC/f1" '%Y'

# --- utimens-nsec: nanosecond-precision timestamp via testutil utimens ---

# busybox touch -d has only second precision. This also becomes f1's final
# mtime, checked again below (warm-after-all, restart-mtime-persists).
NSEC_SEC=1000000000
NSEC_NSEC=123456789
out=$("$TESTUTIL" utimens "$MNT/f1" "$NSEC_SEC" "$NSEC_NSEC")
rc=$?
if [ "$rc" -eq 0 ]; then
	pass utimens-nsec-tool
else
	fail utimens-nsec-tool "rc=$rc out='$out'"
fi
mnt_y=$(stat -c '%y' "$MNT/f1")
src_y=$(stat -c '%y' "$SRC/f1")
if [ "$mnt_y" = "$src_y" ]; then
	pass utimens-nsec-matches-src
else
	fail utimens-nsec-matches-src "mnt='$mnt_y' src='$src_y'"
fi
case "$mnt_y" in
*.123456789*)
	pass utimens-nsec-precision
	;;
*)
	fail utimens-nsec-precision "got '$mnt_y', want nanoseconds .123456789"
	;;
esac
verify_cached utimens-nsec-cached "$MNT/f1" '%Y' "$NSEC_SEC"
final_mtime_epoch=$NSEC_SEC

# --- symlink-chmod-eopnotsupp: chmod on a symlink itself -----------------

# chmod(2) has no way to change a symlink's own mode on Linux (there is no
# lchmod syscall: fchmodat(2) with AT_SYMLINK_NOFOLLOW always fails with
# EOPNOTSUPP for a symlink target, verified even on a plain tmpfs symlink
# -- this is generic kernel behavior, not something specific to dcfs), so
# backing::SetAttr rejects FUSE_SET_ATTR_MODE on a symlink the same way.
# busybox's chmod has no -h/--no-dereference (verified: `busybox chmod
# --help` lists only -Rcvf) so it can never even ask the kernel to target
# the symlink itself rather than following it -- testutil lchmod uses
# fchmodat(2) with AT_SYMLINK_NOFOLLOW directly.
before_f1_mode=$(stat -c '%a' "$SRC/f1")
out=$("$TESTUTIL" lchmod "$MNT/l1" 600)
rc=$?
if [ "$rc" -ne 0 ] && [ "$out" = "ERR EOPNOTSUPP" ]; then
	pass symlink-chmod-eopnotsupp
else
	fail symlink-chmod-eopnotsupp "rc=$rc out='$out' (want nonzero rc, 'ERR EOPNOTSUPP')"
fi
after_f1_mode=$(stat -c '%a' "$SRC/f1")
if [ "$before_f1_mode" != "$after_f1_mode" ]; then
	fail symlink-chmod-no-side-effect "f1's mode changed from $before_f1_mode to $after_f1_mode even though the chmod failed"
else
	pass symlink-chmod-no-side-effect
fi

# --- warm-after-all: the whole tree, from cache, twice --------------------

verify_tree_cached warm-after-all

# --- restart-persists: kill, restart against the same db, recheck --------

kill -TERM "$DAEMON_PID" 2>/dev/null || true
wait "$DAEMON_PID" 2>/dev/null || true
DAEMON_PID=""
if is_mounted "$MNT"; then
	echo "setattr.sh: /mnt still mounted after SIGTERM; forcing umount"
	umount "$MNT" 2>/dev/null || true
fi
MOUNTED=0

if start_daemon "$LOG2"; then
	pass restart-mount
else
	fail restart-mount "daemon did not remount within 10s"
	exit "$FAILED"
fi

verify_cached restart-mode-persists "$MNT/f1" '%a' 640
verify_cached restart-owner-persists "$MNT/f1" '%u:%g' 1000:1000
verify_cached restart-size-persists "$MNT/f1" '%s' 100
verify_cached restart-mtime-persists "$MNT/f1" '%Y' "$final_mtime_epoch"
verify_cached restart-dir-mode-persists "$MNT/d1" '%a' 750
verify_cached restart-fifo-mode-persists "$MNT/p1" '%a' 600
verify_tree_cached restart-tree-cached

exit "$FAILED"
