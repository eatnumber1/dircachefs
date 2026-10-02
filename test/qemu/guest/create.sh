#!/bin/sh
# dcfs step 4.2 acceptance test: the create-family write-through ops
# (mkdir/mknod/symlink/link/create).
#
# Builds a tree on vdb, mounts dcfs over it, and exercises every
# create-family op *through* dcfs (never directly against /src, except to
# confirm the backing side agrees): mkdir (plain, nested, EEXIST,
# ENOENT-missing-parent, and refused by a mount/subvolume boundary -- see
# below), create via a shell redirect (content, size right after close,
# O_EXCL/noclobber -> EEXIST), mknod (a FIFO; a regular file is Create, not
# mknod -- busybox mknod cannot make one), symlink (resolving and dangling),
# link (nlink/inode agreement, and EXDEV against a boundary). Each check
# compares /src and /mnt directly. Finally, a full metadata pass over the
# whole tree -- with the page/dentry/inode caches dropped first -- causes
# *zero* additional block reads on the backing device (RecordNewChild/
# RecordNewLink already wrote everything the create needed into the cache; a
# subsequent stat should never touch the backing filesystem again), and this
# stays true after killing and restarting the daemon against the same cache
# database.
#
# Also step 4.8's runtime submount refusal (amendment 12), using vdc as a
# second filesystem mounted below the source after dcfs starts: every
# create-family op against a refused boundary fails with EXDEV, exactly as
# it would fail if attempted directly on the backing filesystem across a
# real device boundary (see mkdir-boundary-refused/link-exdev below) -- see
# README's Limitations and dcfs/backing.cc's ProbeChild/PopulateDirectory.
# (The startup-refusal half of amendment 12 is exercised once, in
# readonly.sh.)
#
# Uses only busybox applets/options (verified against the exact busybox
# baked into the initramfs: `busybox --list`, `busybox <applet> --help`) --
# no GNU find/stat/coreutils extensions.
#
# Run as /tests/create.sh by guest/init when booted with dcfs_test=create.sh;
# prints one "TEST ... PASS/FAIL" line per check and exits nonzero if any
# check failed. init turns that into the final ALL-TESTS-PASSED / TEST-FAILED
# verdict.
FAILED=0
. "$(dirname "$0")/lib.sh"

DCFS=/bin/dcfs

SRC=/src
MNT=/mnt
DB=/cache/dcfs.db
LOG1=/tmp/dcfs-1.log
LOG2=/tmp/dcfs-2.log

DAEMON_PID=""
MOUNTED=0

# See readonly.sh for why every cleanup command here is `|| true`-guarded:
# this must run to completion (and dump both daemon logs on any failure)
# regardless of how the script is exiting.
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

echo "create.sh: kernel $(uname -r)"

# --- helpers -------------------------------------------------------------

# Runs "$@", expecting it to fail with a message containing $2 (a fragment
# of the errno's strerror() text -- e.g. "File exists" for EEXIST, "No such
# file or directory" for ENOENT, "cross-device" for EXDEV). Reports as test
# $1.
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
	*) fail "$name" "want '$want' in error, got: $out" ;;
	esac
}

# A stat listing (one line per entry: "inode perms nlink owner group size
# name", symlinks rendered "name -> target" via busybox stat's %N) of $1,
# with the path prefix $2 rewritten out of the name so a /src listing and
# the matching /mnt listing compare equal. The inode number is deliberately
# kept (dcfs reports the *backing* inode as st_ino, so it must already agree
# between the two sides).
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
		rest=${rest#\'$prefix} # symlink name half, quoted by %N
		rest=${rest#$prefix}   # regular file/dir name, unquoted
		echo "$inode $perms $links $owner $group $size $rest"
	done <"$file"
}

# One full metadata pass over the whole tree via $1 (/src or /mnt): every
# entry's stat, plus resolving every symlink. No content reads.
run_pass() {
	dir=$1
	find "$dir" -exec stat -c '%i %A %h %U %G %s %N' {} + >/tmp/pass_stat.txt
	readlink "$dir/l1" >/dev/null
	readlink "$dir/l2" >/dev/null
}

# --- build the backing tree -------------------------------------------------

mount /dev/vdb /src
# "d" is for mkdir-boundary-refused/link-exdev below: it must never be
# listed through dcfs before vdc is mounted under it at runtime.
mkdir -p /src/d
sync

mkdir -p /cache /mnt
if start_daemon "$LOG1"; then
	pass mount
else
	fail mount "daemon did not mount within 10s"
	exit "$FAILED"
fi

umask 022

# --- mkdir ------------------------------------------------------------

if mkdir "$MNT/d1"; then
	mode_src=$(stat -c %a /src/d1)
	mode_mnt=$(stat -c %a /mnt/d1)
	type_src=$(stat -c %F /src/d1)
	type_mnt=$(stat -c %F /mnt/d1)
	ino_src=$(stat -c %i /src/d1)
	ino_mnt=$(stat -c %i /mnt/d1)
	if [ "$mode_src" = "755" ] && [ "$mode_mnt" = "755" ] &&
		[ "$type_src" = "directory" ] && [ "$type_mnt" = "directory" ] &&
		[ "$ino_src" = "$ino_mnt" ]; then
		pass mkdir
	else
		fail mkdir "mode=$mode_src/$mode_mnt type=$type_src/$type_mnt ino=$ino_src/$ino_mnt"
	fi
else
	fail mkdir "mkdir failed"
fi

if mkdir "$MNT/d1/d2" && [ -d /src/d1/d2 ] && [ -d /mnt/d1/d2 ]; then
	pass mkdir-nested
else
	fail mkdir-nested "mkdir or type check failed"
fi

expect_fail mkdir-eexist "File exists" mkdir "$MNT/d1"
expect_fail mkdir-enoent-parent "No such file or directory" mkdir "$MNT/nope/x"

# --- create (via a shell redirect) -----------------------------------------

if echo hello >"$MNT/f1"; then
	content_src=$(cat /src/f1)
	size_mnt=$(stat -c %s /mnt/f1)
	if [ "$content_src" = "hello" ] && [ "$size_mnt" = "6" ]; then
		pass create-write
	else
		fail create-write "content_src='$content_src' size_mnt=$size_mnt"
	fi
else
	fail create-write "echo redirect failed"
fi

set -C
# 2>&1 must precede the noclobber-protected redirect: ash reports a failed
# ">" target as a shell-level error (not something the never-started `echo`
# writes), using whatever fd 2 points to *at the moment that redirect is
# attempted* -- so it has to already be pointed at this substitution's
# capture pipe before ">$MNT/f1" is even tried, or the message escapes to
# the real stderr instead of $excl_out.
excl_out=$(echo x 2>&1 >"$MNT/f1")
excl_rc=$?
set +C
if [ "$excl_rc" -ne 0 ]; then
	case "$excl_out" in
	*[Ee]xist*) pass create-excl ;;
	*) fail create-excl "want EEXIST-ish message, got: $excl_out" ;;
	esac
else
	fail create-excl "noclobber redirect unexpectedly succeeded"
fi
# f1 must still hold its original content -- the clobber must not have
# gone through.
content_after_excl=$(cat /src/f1)
if [ "$content_after_excl" != "hello" ]; then
	fail create-excl-unchanged "f1 content became '$content_after_excl'"
else
	pass create-excl-unchanged
fi

# --- mknod ------------------------------------------------------------

if mknod "$MNT/p1" p && [ -p /src/p1 ] && [ -p /mnt/p1 ]; then
	pass mknod-fifo
else
	fail mknod-fifo "mknod or type check failed"
fi

# busybox mknod cannot make a regular file (TYPE must be b/c/u/p); Create is
# exercised via a plain redirect (create-write) and via touch here.
if touch "$MNT/f2" && [ -f /src/f2 ] && [ -f /mnt/f2 ]; then
	pass mknod-reg
else
	fail mknod-reg "touch or type check failed"
fi

# --- symlink ------------------------------------------------------------

if ln -s f1 "$MNT/l1"; then
	target_src=$(readlink /src/l1)
	target_mnt=$(readlink /mnt/l1)
	if [ "$target_src" = "f1" ] && [ "$target_mnt" = "f1" ]; then
		pass symlink
	else
		fail symlink "src target='$target_src' mnt target='$target_mnt'"
	fi
else
	fail symlink "ln -s failed"
fi

if ln -s nowhere "$MNT/l2"; then
	target_src=$(readlink /src/l2)
	target_mnt=$(readlink /mnt/l2)
	if [ "$target_src" = "nowhere" ] && [ "$target_mnt" = "nowhere" ]; then
		pass symlink-dangling
	else
		fail symlink-dangling "src target='$target_src' mnt target='$target_mnt'"
	fi
else
	fail symlink-dangling "ln -s failed"
fi

# --- link ------------------------------------------------------------

if ln "$MNT/f1" "$MNT/f1h"; then
	ok=1
	for f in /src/f1 /src/f1h /mnt/f1 /mnt/f1h; do
		nlink=$(stat -c %h "$f")
		[ "$nlink" = "2" ] || { ok=0; echo "info: $f nlink=$nlink"; }
	done
	ino_mnt_f1=$(stat -c %i /mnt/f1)
	ino_mnt_f1h=$(stat -c %i /mnt/f1h)
	ino_src_f1=$(stat -c %i /src/f1)
	ino_src_f1h=$(stat -c %i /src/f1h)
	[ "$ino_mnt_f1" = "$ino_mnt_f1h" ] || ok=0
	[ "$ino_src_f1" = "$ino_src_f1h" ] || ok=0
	[ "$ino_mnt_f1" = "$ino_src_f1" ] || ok=0
	if [ "$ok" -eq 1 ]; then
		pass link
	else
		fail link "nlink or inode mismatch (see info lines above)"
	fi
else
	fail link "ln failed"
fi

# --- mkdir-boundary-refused / link-exdev: create-family ops refused by a --
# --- mount/subvolume boundary discovered at runtime (amendment 12) --------
#
# vdc is mounted below the source (under "d", never listed through dcfs
# before this point -- see readonly.sh's boundary-* checks for why that
# ordering matters) after dcfs is already running. The kernel's own VFS
# always looks a create-family op's target name up first (to confirm it
# does not already exist) before issuing the create itself, so a name the
# LOOKUP path refuses with EXDEV (see backing::LookupOrPopulate) makes the
# create-family syscall itself fail with EXDEV too, without dcfs's Mkdir/
# Link ops ever running -- exactly as it would if mkdir/ln were attempted
# straight across a real device boundary on the backing filesystem.
mkdir /src/d/mp
mount /dev/vdc /src/d/mp
expect_fail mkdir-boundary-refused "cross-device" mkdir "$MNT/d/mp"
expect_fail link-exdev "cross-device" ln "$MNT/f1" "$MNT/d/mp"
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

# --- listing-matches: everything created above agrees between /src/ and
# /mnt -- "d" is excluded: see readonly.sh's identity-checks comment on why
# /src/d and the cached /mnt/d deliberately diverge after the checks above.
# -----------------------------------------------------------------------

find /src -path /src/d -prune -o -exec stat -c '%i %A %h %U %G %s %N' {} + >/tmp/src_stat.txt
find /mnt -path /mnt/d -prune -o -exec stat -c '%i %A %h %U %G %s %N' {} + >/tmp/mnt_stat.txt
normalize_stat /tmp/src_stat.txt "$SRC" | sort >/tmp/src_stat_norm.txt
normalize_stat /tmp/mnt_stat.txt "$MNT" | sort >/tmp/mnt_stat_norm.txt
set -- $(md5sum /tmp/src_stat_norm.txt)
src_sum=$1
set -- $(md5sum /tmp/mnt_stat_norm.txt)
mnt_sum=$1
if [ "$src_sum" = "$mnt_sum" ]; then
	pass listing-matches
else
	fail listing-matches "normalized stat listing differs (src=$src_sum mnt=$mnt_sum)"
	echo "--- src_stat_norm.txt ---"
	cat /tmp/src_stat_norm.txt
	echo "--- mnt_stat_norm.txt ---"
	cat /tmp/mnt_stat_norm.txt
fi

# --- warm-after-all: a full re-stat of everything just created must not
# touch either backing device -- RecordNewChild/RecordNewLink already wrote
# it all into the cache -------------------------------------------------

drop_caches
before_vdb=$(sectors_read vdb)

run_pass "$MNT"

after_vdb=$(sectors_read vdb)
if [ "$after_vdb" = "$before_vdb" ]; then
	pass warm-after-all-vdb
else
	fail warm-after-all-vdb "sectors_read(vdb) $before_vdb -> $after_vdb"
fi

# --- restart-persists: kill+restart against the same cache db, everything
# created above is still there, and the cache is still warm ----------------

kill -TERM "$DAEMON_PID" 2>/dev/null || true
wait "$DAEMON_PID" 2>/dev/null || true
DAEMON_PID=""
if is_mounted "$MNT"; then
	echo "create.sh: /mnt still mounted after SIGTERM; forcing umount"
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

# Correctness first (a content read -- cat -- legitimately moves a backing
# device's counter, same as passthrough_test documents, so this must happen
# *before* the metadata-only warm measurement below, not mixed into it).
persisted=1
[ "$(stat -c %h /mnt/f1)" = "2" ] || persisted=0
[ "$(stat -c %h /mnt/f1h)" = "2" ] || persisted=0
[ "$(readlink /mnt/l1)" = "f1" ] || persisted=0
[ "$(readlink /mnt/l2)" = "nowhere" ] || persisted=0
[ -p /mnt/p1 ] || persisted=0
[ -d /mnt/d1/d2 ] || persisted=0
if [ "$persisted" -eq 1 ]; then
	pass restart-persists
else
	fail restart-persists "one or more created objects did not survive the restart"
fi

drop_caches
before_vdb=$(sectors_read vdb)

run_pass "$MNT" # metadata only -- no content reads.

after_vdb=$(sectors_read vdb)
if [ "$after_vdb" = "$before_vdb" ]; then
	pass restart-warm-vdb
else
	fail restart-warm-vdb "sectors_read(vdb) $before_vdb -> $after_vdb"
fi

exit "$FAILED"
