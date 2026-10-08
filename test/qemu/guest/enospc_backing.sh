#!/bin/sh
# dcfs step 11.4 (a): the BACKING filesystem out of space (docs/plan/phases/
# 11-crash-stress-failure-testing.md, 11.4; design.md, "The write-through
# protocol", "Phase 2"). Before dcfs starts, the backing filesystem is filled
# (fallocate, then blocks appended until ENOSPC; as root, so ext4's reserved
# blocks are taken too); dcfs then lists the tree, and through it, with no
# space left:
#
#   create   names in one directory until a create fails (ext4: the
#            directory's block is full and a new one cannot be had; xfs: no
#            free inode in an allocated chunk)
#   write    64 KiB written past a file's end (no block to reserve)
#   mkdir    a new directory (ext4 needs a block, xfs an inode)
#   setxattr a 2000-byte value (larger than the room in the inode)
#   rename   a long name into the full directory
#
# Each that fails must fail with ENOSPC, leave nothing cached as done (the
# name, the size, the attribute: dcfs serves what the backing filesystem
# holds, entry by entry) and leave the daemon serving; each that succeeds
# must have happened on the backing filesystem. The checking build's
# invariant checks run on every request. Then the fill is removed through
# dcfs, the same operations succeed, and after a restart (its full check of
# the database) everything served still matches the backing filesystem.
#
# btrfs reserves metadata space ahead and keeps it apart from data: with
# the data space full, metadata-only operations (an empty create, a
# mkdir, a small xattr, a rename) can go on succeeding, so on btrfs only the
# write must fail (what each did is printed).
#
# Run as /tests/enospc_backing.sh by guest/init when booted with
# dcfs_test=enospc_backing.sh.
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
DAEMON_PID=""
MOUNTED=0
# A name long enough that a few dozen fill an ext4 directory block.
LONG=entry-with-a-name-long-enough-to-fill-blocks

cleanup() {
	rc=$?
	if [ "$rc" -ne 0 ] || [ "$FAILED" -ne 0 ]; then
		for log in $LOGS; do
			echo "--- dcfs stderr ($log) ---"
			cat "$log" 2>/dev/null
		done
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

echo "enospc_backing.sh: kernel $(uname -r)"
require_commands umount sync find stat grep dd head

alive() {
	[ -n "$DAEMON_PID" ] && kill -0 "$DAEMON_PID" 2>/dev/null
}

start() {
	RUN=$((RUN + 1))
	LOG=/tmp/dcfs-$RUN.log
	LOGS="$LOGS $LOG"
	fd_start "$LOG" --sync_interval_sec=3600 "$@"
}

# nospace NAME OUTPUT: the failure's message is ENOSPC's.
nospace() {
	case "$2" in
	*"No space left on device"*) pass "$1-enospc" ;;
	*) fail "$1-enospc" "failed, but not with ENOSPC: $2" ;;
	esac
}

# served_equals_backing NAME
served_equals_backing() {
	if fd_same_as_backing; then
		pass "$1"
	else
		fail "$1" "served (<) and backing (>) differ: $(tr '\n' '|' </tmp/snap.diff)"
	fi
}

# outcome NAME RC OUTPUT: records whether the operation failed for want of
# space (FAILS gets NAME) and checks the error if it did.
FAILS=""
outcome() {
	if [ "$2" -ne 0 ]; then
		echo "enospc_backing.sh: $1 failed: $3"
		nospace "$1" "$3"
		FAILS="$FAILS $1"
	else
		echo "enospc_backing.sh: $1 succeeded with the filesystem full"
	fi
}

# must_have_failed NAME: on ext4 and xfs every operation needs space; on
# btrfs only the write (see the header).
must_have_failed() {
	case " $FAILS " in
	*" $1 "*) pass "$1-failed-full" ;;
	*)
		if [ "$FSTYPE" = btrfs ] && [ "$1" != write ]; then
			echo "enospc_backing.sh: $1 did not need space on btrfs"
		else
			fail "$1-failed-full" "it succeeded with the filesystem full"
		fi
		;;
	esac
}

fd_setup || {
	fail setup "wrapping or mounting the disks failed"
	exit "$FAILED"
}
FSTYPE=$(backing_fstype "$SRC")
echo "enospc_backing.sh: backing filesystem $FSTYPE"
mkdir "$SRC/d" "$SRC/from" "$SRC/fill"
echo keep >"$SRC/d/keep"
echo moved >"$SRC/from/r1"
sync
fd_fill "$SRC/fill"

if start; then pass mount; else
	fail mount "daemon did not mount within 10s"
	exit "$FAILED"
fi
ls -R "$MNT" >/dev/null
served_equals_backing full-before

# --- the operations, with no space left ---------------------------------------

i=0
create_out=""
while [ "$i" -lt 400 ]; do
	if ! create_out=$(touch "$MNT/d/$LONG-$i" 2>&1); then
		break
	fi
	i=$((i + 1))
done
if [ "$i" -lt 400 ]; then
	echo "enospc_backing.sh: $i creates fitted, the next failed"
	outcome create 1 "$create_out"
	if [ ! -e "$MNT/d/$LONG-$i" ] && [ ! -e "$SRC/d/$LONG-$i" ]; then
		pass create-not-cached
	else
		fail create-not-cached "the failed create's name is served or on the backing filesystem"
	fi
else
	outcome create 0 ""
fi

# 64 KiB at offset 64 KiB (dd: busybox head reports any write error as
# EIO).
write_out=$(dd if=/dev/zero of="$MNT/d/keep" bs=65536 count=1 seek=1 \
	conv=notrunc 2>&1)
outcome write $? "$write_out"
mkdir_out=$(mkdir "$MNT/d/newdir" 2>&1)
outcome mkdir $? "$mkdir_out"
big=$(head -c 2000 /dev/zero | tr '\0' x)
xattr_out=$("$TESTUTIL" setxattr "$MNT/d/keep" user.big "$big" 2>&1)
xattr_rc=$?
[ "$xattr_out" = "ERR ENOSPC" ] && xattr_out="No space left on device ($xattr_out)"
outcome setxattr "$xattr_rc" "$xattr_out"
rename_out=$(mv "$MNT/from/r1" "$MNT/d/$LONG-renamed" 2>&1)
outcome rename $? "$rename_out"
for op in create write mkdir setxattr rename; do
	must_have_failed "$op"
done
if alive; then pass daemon-alive; else fail daemon-alive "the daemon died"; fi

# Nothing failed is cached as done: entry by entry (names, sizes), and the
# xattr, the daemon serves what the backing filesystem holds.
served_equals_backing full-served-equals-backing
served_x=$("$TESTUTIL" getxattr "$MNT/d/keep" user.big 2>&1 | head -c 20)
backing_x=$("$TESTUTIL" getxattr "$SRC/d/keep" user.big 2>&1 | head -c 20)
if [ "$served_x" = "$backing_x" ]; then
	pass full-xattr-as-backing
else
	fail full-xattr-as-backing "served $served_x, backing $backing_x"
fi
case " $FAILS " in
*" rename "*)
	if [ -e "$MNT/from/r1" ] && [ ! -e "$MNT/d/$LONG-renamed" ]; then
		pass rename-not-cached
	else
		fail rename-not-cached "the failed rename is served as done"
	fi
	;;
esac

# --- space freed: the same operations succeed ---------------------------------

if rm -r "$MNT/fill"; then pass free-space; else fail free-space "rm -r fill through dcfs failed"; fi
sync
echo "enospc_backing.sh: freed: $(stat -f -c '%f free blocks of %b' "$SRC")"
if touch "$MNT/d/$LONG-after" && [ -e "$SRC/d/$LONG-after" ]; then
	pass create-after-free
else
	fail create-after-free "touch after freeing space"
fi
if dd if=/dev/zero of="$MNT/d/keep" bs=65536 count=1 seek=1 conv=notrunc \
	2>/dev/null && [ "$(stat -c %s "$MNT/d/keep")" = 131072 ] &&
	[ "$(stat -c %s "$SRC/d/keep")" = 131072 ]; then
	pass write-after-free
else
	fail write-after-free "append after freeing space"
fi
if mkdir "$MNT/d/newdir2" && [ -d "$SRC/d/newdir2" ]; then
	pass mkdir-after-free
else
	fail mkdir-after-free "mkdir after freeing space"
fi
if "$TESTUTIL" setxattr "$MNT/d/keep" user.big "$big" &&
	[ "$("$TESTUTIL" getxattr "$MNT/d/keep" user.big)" = "$big" ]; then
	pass setxattr-after-free
else
	fail setxattr-after-free "setxattr after freeing space"
fi
if [ -e "$MNT/from/r1" ]; then
	if mv "$MNT/from/r1" "$MNT/d/$LONG-renamed" && [ -e "$SRC/d/$LONG-renamed" ]; then
		pass rename-after-free
	else
		fail rename-after-free "rename after freeing space"
	fi
fi
served_equals_backing freed-served-equals-backing

# A clean restart: the checking build checks the whole database at start.
kill -TERM "$DAEMON_PID"
wait "$DAEMON_PID"
echo "enospc_backing.sh: the daemon exited with $?"
DAEMON_PID=""
if start; then pass restart; else
	fail restart "daemon did not mount within 10s"
	exit "$FAILED"
fi
served_equals_backing restart-served-equals-backing

# Self-check of the comparison: a name added behind dcfs's back, after it
# listed the directory, makes it fail.
ls "$MNT/d" >/dev/null
touch "$SRC/d/behind-its-back"
if fd_same_as_backing; then
	fail comparison-detects-divergence "a name only the backing filesystem has went unnoticed"
else
	pass comparison-detects-divergence
fi
rm "$SRC/d/behind-its-back"

require_no_reclaim no-reclaim
exit "$FAILED"
