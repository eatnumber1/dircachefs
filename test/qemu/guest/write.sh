#!/bin/sh
# dcfs step 4.4 acceptance test: write-through file I/O (Write/Flush/Fsync/
# Fallocate/Setxattr/Removexattr) and the "one backing file per inode" fix.
#
# Before this step, Open() gave every FUSE open of a file its own
# passthrough registration; the kernel refuses a second, different backing
# file for one inode (fs/fuse/iomode.c fuse_inode_uncached_io_start), so two
# concurrent opens of the same file (e.g. two `cat`s) failed. dcfs now keeps
# one shared backing fd per inode (opened O_RDWR when possible), registered
# for passthrough at most once, and reused by every concurrent open
# regardless of its own access mode -- this test opens the same file twice
# at once, and once with a reader and a writer overlapping, to prove it.
#
# It also exercises: plain writes (create, append, overwrite-in-place,
# truncate-via-open, a large passthrough write); that a writer's close makes
# its effect on size visible immediately even while another open of the
# same file remains outstanding (Flush(), not just Release(), refreshes
# cached attributes); fsync; fallocate (plain, KEEP_SIZE, PUNCH_HOLE);
# setxattr/getxattr/listxattr/removexattr on a file, a directory, and (EPERM
# for "user."  namespace) a symlink; and 4.2's create-family failure paths
# now re-resolving names instead of leaving them unknown (a second `mkdir`
# of an already-existing directory must not break that directory's own
# ".."). Every result is checked against /src (it actually landed on the
# backing filesystem) and, where the check is metadata-only, again via /mnt
# after dropping every cache with zero sectors read from either backing
# block device -- then as a whole tree, and again after a daemon restart.
#
# Uses only busybox applets/options (verified against the exact busybox
# baked into the initramfs: `busybox --list`, `busybox <applet> --help`) --
# no GNU find/stat/coreutils extensions -- plus //tools:testutil for
# renameat2 flags, fallocate(2), and the xattr syscalls, none of which
# busybox has any applet for at all.
#
# Also step 4.8's runtime submount refusal (amendment 12), using vdc as a
# second filesystem mounted below the source after dcfs starts: a write
# targeting a name behind a refused boundary fails with EXDEV
# (write-boundary-refused below) -- see README's Limitations and
# dcfs/backing.cc's ProbeChild/PopulateDirectory. (The startup-refusal half
# of amendment 12 is exercised once, in readonly.sh.)
#
# Run as /tests/write.sh by guest/init when booted with dcfs_test=write.sh;
# prints one "TEST ... PASS/FAIL" line per check and exits nonzero if any
# check failed. init turns that into the final ALL-TESTS-PASSED /
# TEST-FAILED verdict.
FAILED=0
pass() { echo "TEST $1 PASS"; }
fail() { echo "TEST $1 FAIL ($2)"; FAILED=1; }

DCFS=/bin/dcfs
TESTUTIL=/bin/testutil

is_mounted() { grep -q " $1 " /proc/mounts; }
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
	exec 4<&- 2>/dev/null
	exec 5<&- 2>/dev/null
	exec 6<&- 2>/dev/null
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

echo "write.sh: kernel $(uname -r)"

# --- helpers -------------------------------------------------------------

# Field 3 of /sys/block/<dev>/stat: cumulative sectors read since boot.
sectors_read() {
	read -r line <"/sys/block/$1/stat"
	set -- $line
	echo "$3"
}

# utime (field 14) + stime (field 15) of pid $1, in clock ticks -- see
# proc(5).
cpu_ticks() {
	awk '{print $14 + $15}' "/proc/$1/stat"
}

drop_caches() {
	sync
	echo 3 >/proc/sys/vm/drop_caches
}

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

# check_src NAME COND: evaluates the shell condition COND (which should only
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
check_cold() {
	sync
	echo 3 >/proc/sys/vm/drop_caches
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

run_pass() {
	find "$1" -exec stat -c '%i %A %h %U %G %s %N' {} + >/tmp/pass_stat.txt
}

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

# --- build the backing tree (before dcfs ever sees it) ---------------------

mount /dev/vdb /src
# "d" is for write-boundary-refused below: it must never be listed through
# dcfs before vdc is mounted under it at runtime.
mkdir -p /src/d
sync

mkdir -p /cache /mnt
if start_daemon "$LOG1"; then
	pass mount
else
	fail mount "daemon did not mount within 10s"
	exit "$FAILED"
fi

# --- write-boundary-refused: a filesystem mounted below the source at -----
# --- runtime is refused (amendment 12), so writing into it fails with -----
# --- EXDEV, exactly as it would across a real device boundary -------------

mkdir /src/d/mp
mount /dev/vdc /src/d/mp
# 2>&1 must precede the target redirect: see create.sh's create-excl for why
# (a failed ">" target is reported by the shell itself, using whatever fd 2
# points to *at the moment that redirect is attempted*).
out=$(echo subcontent 2>&1 >"$MNT/d/mp/subfile")
rc=$?
if [ "$rc" -ne 0 ]; then
	case "$out" in
	*cross-device*) pass write-boundary-refused ;;
	*) fail write-boundary-refused "want cross-device in error, got: $out" ;;
	esac
else
	fail write-boundary-refused "echo redirect unexpectedly succeeded"
fi
listing=$(ls -1 "$MNT/d" 2>&1)
case "$listing" in
*mp*) fail boundary-not-listed "mp appeared in /mnt/d: $listing" ;;
*) pass boundary-not-listed ;;
esac
errors=$(grep -c "refusing to cache mp" "$LOG1")
if [ "$errors" -eq 1 ]; then
	pass boundary-error-logged-once
else
	fail boundary-error-logged-once "want 1 ERROR line, got $errors"
fi
umount /src/d/mp

# --- write-create: a fresh file via a shell redirect ------------------------

if echo hi >"$MNT/f"; then
	pass write-create-echo
else
	fail write-create-echo "echo failed"
fi
check_src write-create '[ "$(cat /src/f)" = hi ] && [ "$(stat -c %s /src/f)" = 3 ]'
check_cold write-create '[ "$(stat -c %s /mnt/f)" = 3 ]'
if [ "$(cat "$MNT/f")" = hi ]; then
	pass write-create-content
else
	fail write-create-content "got '$(cat "$MNT/f")'"
fi

# --- write-append ----------------------------------------------------------

if echo more >>"$MNT/f"; then
	pass write-append-echo
else
	fail write-append-echo "echo >> failed"
fi
want_append=$(printf 'hi\nmore\n')
check_src write-append "[ \"\$(cat /src/f)\" = \"$want_append\" ]"
check_cold write-append '[ "$(stat -c %s /mnt/f)" = 8 ]'

# --- write-overwrite-open-rdwr: overwrite in place, no truncate -------------

dd if=/dev/zero of="$MNT/f" bs=1k count=4 conv=notrunc 2>/dev/null
rc=$?
if [ "$rc" -eq 0 ]; then
	pass write-overwrite-tool
else
	fail write-overwrite-tool "dd rc=$rc"
fi
check_src write-overwrite '[ "$(stat -c %s /src/f)" = 4096 ]'
check_cold write-overwrite '[ "$(stat -c %s /mnt/f)" = 4096 ]'

# --- write-truncate-open: O_TRUNC on an existing file -----------------------

# FUSE_CAP_ATOMIC_O_TRUNC is deliberately not granted (see DirCacheFS::Init):
# this exercises the kernel sending a plain SETATTR(size=0) ahead of the
# O_TRUNC open, not an atomic-truncate-in-OPEN path.
: >"$MNT/f"
check_src write-truncate-open '[ "$(stat -c %s /src/f)" = 0 ]'
check_cold write-truncate-open '[ "$(stat -c %s /mnt/f)" = 0 ]'

# --- write-large-passthrough: a 64 MiB write, low daemon CPU proves the -----
# kernel (not dcfs) moved the bytes -----------------------------------------

dd if=/dev/urandom of=/tmp/big_src.bin bs=1M count=64 2>/dev/null
src_want_md5=$(md5sum /tmp/big_src.bin | cut -d' ' -f1)

drop_caches
before_ticks=$(cpu_ticks "$DAEMON_PID")
dd if=/tmp/big_src.bin of="$MNT/big" bs=1M 2>/dev/null
rc=$?
after_ticks=$(cpu_ticks "$DAEMON_PID")
tick_delta=$((after_ticks - before_ticks))
if [ "$rc" -eq 0 ]; then
	pass write-large-tool
else
	fail write-large-tool "dd rc=$rc"
fi
if [ "$tick_delta" -lt 20 ]; then
	pass write-large-passthrough-cpu
else
	fail write-large-passthrough-cpu "cpu ticks delta=$tick_delta (want < 20)"
fi
check_src write-large-content "[ \"\$(md5sum /src/big | cut -d' ' -f1)\" = \"$src_want_md5\" ]"
mnt_md5=$(md5sum "$MNT/big" | cut -d' ' -f1)
if [ "$mnt_md5" = "$src_want_md5" ]; then
	pass write-large-mnt-matches
else
	fail write-large-mnt-matches "src=$src_want_md5 mnt=$mnt_md5"
fi

# --- concurrent-opens: two readers of one file, both must succeed ----------

# Before step 4.4 the second of these would fail: the kernel refuses a
# second, different passthrough backing file for one inode
# (fs/fuse/iomode.c), and Open() gave every open its own.
exec 3<"$MNT/f"
exec 4<"$MNT/f"
c1=$(cat <&3)
rc1=$?
c2=$(cat <&4)
rc2=$?
exec 3<&-
exec 4<&-
if [ "$rc1" -eq 0 ] && [ "$rc2" -eq 0 ] && [ "$c1" = "$c2" ]; then
	pass concurrent-opens-two-readers
else
	fail concurrent-opens-two-readers "rc1=$rc1 rc2=$rc2 c1='$c1' c2='$c2'"
fi

# --- concurrent-opens: a reader stays open while a writer opens, writes and
# closes the same file -------------------------------------------------------

exec 5<"$MNT/f"
if echo newcontent >"$MNT/f"; then
	wr_ok=1
else
	wr_ok=0
fi
reader_still_ok=0
if head -c 1 <&5 >/dev/null 2>&1; then reader_still_ok=1; fi
exec 5<&-
if [ "$wr_ok" -eq 1 ] && [ "$reader_still_ok" -eq 1 ]; then
	pass concurrent-reader-and-writer
else
	fail concurrent-reader-and-writer "wr_ok=$wr_ok reader_still_ok=$reader_still_ok"
fi
check_src concurrent-reader-and-writer '[ "$(cat /src/f)" = newcontent ]'

# --- size-visible-after-close-with-other-open -------------------------------

# A reader stays open across a second, writing open of the same file; the
# writer's close (Flush(), not just Release() -- refs is still > 1) must
# make its effect on size visible right away.
exec 6<"$MNT/f"
printf '%s' "0123456789" >"$MNT/f"
got_size=$(stat -c %s "$MNT/f")
exec 6<&-
if [ "$got_size" = 10 ]; then
	pass size-visible-after-close-with-other-open
else
	fail size-visible-after-close-with-other-open "got size $got_size, want 10"
fi
check_src size-visible-after-close-with-other-open '[ "$(stat -c %s /src/f)" = 10 ] &&
	[ "$(cat /src/f)" = 0123456789 ]'

# --- fsync -------------------------------------------------------------

dd if=/dev/zero of="$MNT/fsyncfile" bs=4096 count=1 conv=fsync 2>/dev/null
rc=$?
if [ "$rc" -eq 0 ]; then
	pass fsync-tool
else
	fail fsync-tool "dd rc=$rc"
fi
check_src fsync '[ "$(stat -c %s /src/fsyncfile)" = 4096 ]'
check_cold fsync '[ "$(stat -c %s /mnt/fsyncfile)" = 4096 ]'

# --- fallocate ---------------------------------------------------------

: >"$MNT/allocfile"
out=$("$TESTUTIL" fallocate "$MNT/allocfile" 0 0 1048576)
rc=$?
if [ "$rc" -eq 0 ]; then
	pass fallocate-tool
else
	fail fallocate-tool "rc=$rc out='$out'"
fi
check_src fallocate '[ "$(stat -c %s /src/allocfile)" = 1048576 ]'
check_cold fallocate '[ "$(stat -c %s /mnt/allocfile)" = 1048576 ]'

out=$("$TESTUTIL" fallocate "$MNT/allocfile" keep_size 1048576 1048576)
rc=$?
if [ "$rc" -eq 0 ]; then
	pass fallocate-keep-size-tool
else
	fail fallocate-keep-size-tool "rc=$rc out='$out'"
fi
check_src fallocate-keep-size '[ "$(stat -c %s /src/allocfile)" = 1048576 ]'
check_cold fallocate-keep-size '[ "$(stat -c %s /mnt/allocfile)" = 1048576 ]'
blocks_before_punch=$(stat -c %b "$SRC/allocfile")

out=$("$TESTUTIL" fallocate "$MNT/allocfile" punch_hole 0 1048576)
rc=$?
if [ "$rc" -eq 0 ]; then
	pass fallocate-punch-hole-tool
else
	fail fallocate-punch-hole-tool "rc=$rc out='$out'"
fi
blocks_after_punch=$(stat -c %b "$SRC/allocfile")
if [ "$blocks_after_punch" -lt "$blocks_before_punch" ]; then
	pass fallocate-punch-hole-src-blocks
else
	fail fallocate-punch-hole-src-blocks "blocks $blocks_before_punch -> $blocks_after_punch"
fi
check_src fallocate-punch-hole-size '[ "$(stat -c %s /src/allocfile)" = 1048576 ]'
check_cold fallocate-punch-hole-size '[ "$(stat -c %s /mnt/allocfile)" = 1048576 ]'
mnt_blocks_after_punch=$(stat -c %b "$MNT/allocfile")
if [ "$mnt_blocks_after_punch" = "$blocks_after_punch" ]; then
	pass fallocate-punch-hole-mnt-blocks
else
	fail fallocate-punch-hole-mnt-blocks "src=$blocks_after_punch mnt=$mnt_blocks_after_punch"
fi

# --- setxattr/getxattr/listxattr on a regular file --------------------------

: >"$MNT/xf"
out=$("$TESTUTIL" setxattr "$MNT/xf" user.a v)
rc=$?
if [ "$rc" -eq 0 ]; then
	pass setxattr-tool
else
	fail setxattr-tool "rc=$rc out='$out'"
fi
got_src=$("$TESTUTIL" getxattr "$SRC/xf" user.a)
if [ "$got_src" = v ]; then
	pass setxattr-matches-src
else
	fail setxattr-matches-src "got '$got_src'"
fi
drop_caches
before=$(sectors_read vdb)
got_mnt=$("$TESTUTIL" getxattr "$MNT/xf" user.a)
after=$(sectors_read vdb)
delta=$((after - before))
if [ "$got_mnt" = v ] && [ "$delta" -eq 0 ]; then
	pass getxattr-cached
else
	fail getxattr-cached "got='$got_mnt' sectors_delta=$delta"
fi
src_list=$("$TESTUTIL" listxattr "$SRC/xf" | sort)
mnt_list=$("$TESTUTIL" listxattr "$MNT/xf" | sort)
if [ "$src_list" = "$mnt_list" ]; then
	pass listxattr-matches
else
	fail listxattr-matches "src='$src_list' mnt='$mnt_list'"
fi

# setxattr XATTR_CREATE/XATTR_REPLACE flags are skipped: testutil's setxattr
# always passes flags 0 (see tools/testutil.c), and adding flag support
# there is not worth it just for this one check.

# --- removexattr, and ENODATA on a second remove ----------------------------

out=$("$TESTUTIL" removexattr "$MNT/xf" user.a)
rc=$?
if [ "$rc" -eq 0 ]; then
	pass removexattr-tool
else
	fail removexattr-tool "rc=$rc out='$out'"
fi
out=$("$TESTUTIL" getxattr "$SRC/xf" user.a)
if [ "$out" = "ERR ENODATA" ]; then
	pass removexattr-gone-src
else
	fail removexattr-gone-src "got '$out'"
fi
out=$("$TESTUTIL" removexattr "$MNT/xf" user.a)
rc=$?
if [ "$rc" -ne 0 ] && [ "$out" = "ERR ENODATA" ]; then
	pass removexattr-second-enodata
else
	fail removexattr-second-enodata "rc=$rc out='$out'"
fi

# --- xattr-on-dir --------------------------------------------------------

mkdir -p "$MNT/xdir"
out=$("$TESTUTIL" setxattr "$MNT/xdir" user.d yes)
rc=$?
if [ "$rc" -eq 0 ]; then
	pass xattr-on-dir-set
else
	fail xattr-on-dir-set "rc=$rc out='$out'"
fi
got=$("$TESTUTIL" getxattr "$SRC/xdir" user.d)
if [ "$got" = yes ]; then
	pass xattr-on-dir-matches-src
else
	fail xattr-on-dir-matches-src "got '$got'"
fi

# --- xattr-on-symlink-eperm ------------------------------------------------

ln -s xf "$MNT/xlink"
out=$("$TESTUTIL" setxattr "$MNT/xlink" user.a v)
rc=$?
if [ "$rc" -ne 0 ] && [ "$out" = "ERR EPERM" ]; then
	pass xattr-on-symlink-eperm
else
	fail xattr-on-symlink-eperm "rc=$rc out='$out' (want nonzero rc, 'ERR EPERM')"
fi

# --- xattrs the backing filesystem stores differently (step 4.11) ----------

# xattr_matches NAME PATH XATTR: getxattr through /mnt must answer exactly
# what the backing filesystem does (hex value, or the same ERR errno).
xattr_matches() {
	x_src=$("$TESTUTIL" getxattrhex "$SRC/$2" "$3")
	x_mnt=$("$TESTUTIL" getxattrhex "$MNT/$2" "$3")
	if [ "$x_src" = "$x_mnt" ]; then
		pass "$1"
	else
		fail "$1" "src='$x_src' mnt='$x_mnt'"
	fi
}

# Binary POSIX ACL xattrs (struct posix_acl_xattr_header, version 2, then
# 8-byte entries: le16 tag, le16 perm, le32 id; ids ffffffff for the
# owner/group/other/mask entries), since busybox has no setfacl.
# u::rw- g::r-- o::r--: exactly equivalent to mode 0644, so ext4 stores no
# xattr at all (posix_acl_update_mode) and only sets the mode.
ACL_MINIMAL_644=0200000001000600ffffffff04000400ffffffff20000400ffffffff

# setxattr of an ACL equivalent to the mode: what is cached must be what the
# backing filesystem stored (nothing), not what the client sent.
: >"$MNT/acl_min"
chmod 644 "$MNT/acl_min"
out=$("$TESTUTIL" setxattrhex "$MNT/acl_min" system.posix_acl_access "$ACL_MINIMAL_644")
rc=$?
out_src=$("$TESTUTIL" getxattrhex "$SRC/acl_min" system.posix_acl_access)
if [ "$rc" -eq 0 ] && [ "$out_src" = "ERR ENODATA" ]; then
	pass acl-minimal-set
else
	fail acl-minimal-set "rc=$rc out='$out' src='$out_src' (want the backing to store no ACL)"
fi
xattr_matches acl-minimal-getxattr-matches-src acl_min system.posix_acl_access

# --- mkdir-eexist-parent-still-resolves (step 4.2 failure-path fix) --------

# A failed mkdir/mknod/symlink/create must re-resolve the name it marked
# unknown in phase 1, rather than leaving it that way: a directory whose own
# dentry is unknown has no cached parent, so the kernel (which still holds
# its dentry, since the op failed) could not even list it ("..").
mkdir "$MNT/d1"
rc1=$?
out2=$(mkdir "$MNT/d1" 2>&1)
rc2=$?
if [ "$rc1" -eq 0 ] && [ "$rc2" -ne 0 ]; then
	pass mkdir-eexist
else
	fail mkdir-eexist "rc1=$rc1 rc2=$rc2 out2='$out2'"
fi
sync
echo 3 >/proc/sys/vm/drop_caches
b_vdb=$(sectors_read vdb)
b_vdc=$(sectors_read vdc)
ok1=0
ls "$MNT/d1/.." >/dev/null 2>&1 && ok1=1
ok2=0
(cd "$MNT/d1" && ls .. >/dev/null 2>&1) && ok2=1
a_vdb=$(sectors_read vdb)
a_vdc=$(sectors_read vdc)
if [ "$ok1" -eq 1 ] && [ "$ok2" -eq 1 ] && [ "$a_vdb" = "$b_vdb" ] && [ "$a_vdc" = "$b_vdc" ]; then
	pass mkdir-eexist-parent-still-resolves
else
	fail mkdir-eexist-parent-still-resolves \
		"ok1=$ok1 ok2=$ok2 vdb $b_vdb->$a_vdb vdc $b_vdc->$a_vdc"
fi

# --- listing-matches: "d" is excluded -- see readonly.sh's identity-checks
# comment on why /src/d and the cached /mnt/d deliberately diverge after the
# write-boundary-refused check above. -----------------------------------

find /src -path /src/d -prune -o -exec stat -c '%i %A %h %U %G %s %N' {} + >/tmp/src_stat.txt
find /mnt -path /mnt/d -prune -o -exec stat -c '%i %A %h %U %G %s %N' {} + >/tmp/mnt_stat.txt
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

sync
echo 3 >/proc/sys/vm/drop_caches
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
	echo "write.sh: /mnt still mounted after SIGTERM; forcing umount"
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

sync
echo 3 >/proc/sys/vm/drop_caches
before_vdb=$(sectors_read vdb)
run_pass "$MNT"
after_vdb=$(sectors_read vdb)
if cmp -s /tmp/pass_before_restart.txt /tmp/pass_stat.txt; then
	pass restart-persists
else
	fail restart-persists "tree differs after restart"
fi
if [ "$after_vdb" = "$before_vdb" ]; then
	pass restart-warm
else
	fail restart-warm "vdb $before_vdb -> $after_vdb"
fi

check_cold restart-content-persists '[ "$(stat -c %s /mnt/f)" = 10 ] &&
	[ "$(stat -c %s /mnt/allocfile)" = 1048576 ]'
if [ "$(cat "$MNT/f")" = "0123456789" ]; then
	pass restart-content-value
else
	fail restart-content-value "got '$(cat "$MNT/f")'"
fi

drop_caches
before=$(sectors_read vdb)
got=$("$TESTUTIL" getxattr "$MNT/xdir" user.d)
after=$(sectors_read vdb)
delta=$((after - before))
if [ "$got" = yes ] && [ "$delta" -eq 0 ]; then
	pass restart-xattr-cached
else
	fail restart-xattr-cached "got='$got' sectors_delta=$delta"
fi

exit "$FAILED"
