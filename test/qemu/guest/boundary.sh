#!/bin/sh
# dcfs step 23.5 (Phase 15's decision 3, pulled forward): a mount point or
# btrfs subvolume below the source is presented as a stub directory.
#
# The stub is listed by readdir, a lookup of it succeeds (a directory with
# the boundary root's mode, owner and times, and a nodeid -- shown as its
# inode number -- from the range at or above 2^63 that no backing inode
# number may use), and anything inside it fails with ENOTSUP, logged at
# ERROR once per stub. Renaming the stub itself fails with EXDEV. A link or
# rename *into* the stub fails with ENOTSUP rather than EXDEV: the kernel
# looks the target name up in the stub first, and that lookup is "anything
# inside". The stub keeps its inode number across a dcfs restart, and
# nothing reaches the filesystem on the other side of the boundary or the
# directory it covers.
#
# The boundary appears while dcfs runs (a mount below the source at startup
# makes dcfs refuse to start; see readonly.sh): vdc mounted below the
# source on every backing filesystem, and on btrfs also a subvolume.
#
# Run as /tests/boundary.sh by guest/init when booted with
# dcfs_test=boundary.sh; prints one "TEST ... PASS/FAIL" line per check
# and exits nonzero if any check failed.
FAILED=0
. "$(dirname "$0")/lib.sh"

DCFS=/bin/dcfs
TESTUTIL=/bin/testutil

SRC=/src
MNT=/mnt
DB=/cache/dcfs.db
LOG1=/tmp/dcfs-1.log
LOG2=/tmp/dcfs-2.log

DAEMON_PID=""
MOUNTED=0

cleanup() {
	rc=$?
	if [ "$rc" -ne 0 ] || [ "$FAILED" -ne 0 ]; then
		echo "--- dcfs stderr (first run) ---"
		cat "$LOG1" 2>/dev/null
		echo "--- dcfs stderr (second run) ---"
		cat "$LOG2" 2>/dev/null
	fi
	if [ "$MOUNTED" -eq 1 ]; then
		umount "$MNT" 2>/dev/null || umount -l "$MNT" 2>/dev/null || true
	fi
	if [ -n "$DAEMON_PID" ] && kill -0 "$DAEMON_PID" 2>/dev/null; then
		kill "$DAEMON_PID" 2>/dev/null || true
		wait "$DAEMON_PID" 2>/dev/null || true
	fi
	umount /src/d/mp 2>/dev/null || true
}
trap cleanup EXIT

echo "boundary.sh: kernel $(uname -r)"

# expect_fail NAME WANT CMD...: CMD must fail with WANT (a fragment of the
# error message: "not supported" for ENOTSUP, "cross-device" for EXDEV, or
# testutil's errno name) in its output.
expect_fail() {
	name=$1
	shift
	want=$1
	shift
	out=$("$@" 2>&1)
	rc=$?
	if [ "$rc" -eq 0 ]; then
		fail "$name" "unexpectedly succeeded: $out"
		return
	fi
	case "$out" in
	*"$want"*) pass "$name" ;;
	*) fail "$name" "want '$want' in the error, got: $out" ;;
	esac
}

# in_reserved_range INO: whether the decimal INO is at least 2^63
# (9223372036854775808) and below 2^64: 19 digits from 9223372036854775808
# up, or 20 digits from 1. busybox's arithmetic is signed 64-bit, so this
# compares digits.
in_reserved_range() {
	case "$1" in
	*[!0-9]*|"") return 1 ;;
	esac
	case "${#1}" in
	19) [ "$1" \> "9223372036854775807" ] ;;
	20) case "$1" in 1*) return 0 ;; *) return 1 ;; esac ;;
	*) return 1 ;;
	esac
}

# check_stub TAG DIR NAME: the stub checks for boundary NAME in DIR (both
# under $MNT), as test names prefixed TAG.
check_stub() {
	tag=$1
	dir=$2
	name=$3
	stub="$MNT/$dir/$name"

	case " $(ls -a "$MNT/$dir" 2>&1 | tr '\n' ' ') " in
	*" $name "*) pass "$tag-listed" ;;
	*) fail "$tag-listed" "$name missing from ls $MNT/$dir: $(ls -a "$MNT/$dir" 2>&1)" ;;
	esac

	type=$(stat -c %F "$stub" 2>&1)
	if [ "$type" = "directory" ]; then
		pass "$tag-stat-directory"
	else
		fail "$tag-stat-directory" "stat -c %F: $type"
	fi

	ino=$(stat -c %i "$stub" 2>&1)
	if in_reserved_range "$ino"; then
		pass "$tag-ino-reserved"
	else
		fail "$tag-ino-reserved" "inode number $ino is not >= 2^63"
	fi
	eval "STUB_INO_$tag=\$ino"

	# readdir reports the same number (d_ino) as stat (st_ino).
	d_ino=$("$TESTUTIL" readdir-ino "$MNT/$dir" | awk -v n="$name" '$1 == n {print $2}')
	if [ "$d_ino" = "$ino" ]; then
		pass "$tag-d-ino"
	else
		fail "$tag-d-ino" "readdir d_ino '$d_ino', stat st_ino '$ino'"
	fi

	# Mode and owner are the boundary root's.
	want=$(stat -c '%a %u %g' "$SRC/$dir/$name")
	got=$(stat -c '%a %u %g' "$stub")
	if [ "$want" = "$got" ]; then
		pass "$tag-mode-owner"
	else
		fail "$tag-mode-owner" "boundary root '$want', stub '$got'"
	fi

	expect_fail "$tag-ls-inside" "not supported" ls "$stub/"
	expect_fail "$tag-stat-inside" "not supported" stat "$stub/inner"
	expect_fail "$tag-touch-inside" "not supported" touch "$stub/new"
	expect_fail "$tag-mkdir-inside" "not supported" mkdir "$stub/newdir"
	expect_fail "$tag-ln-into" "not supported" ln "$MNT/f" "$stub/f"
	expect_fail "$tag-rename-into" "EOPNOTSUPP" \
		"$TESTUTIL" rename2 "$MNT/f" "$stub/f" 0
	expect_fail "$tag-rename-stub" "EXDEV" \
		"$TESTUTIL" rename2 "$stub" "$MNT/$dir/$name.moved" 0
	expect_fail "$tag-chmod-stub" "not supported" chmod 700 "$stub"

	# Nothing reached the other side of the boundary.
	if [ -e "$SRC/$dir/$name/new" ] || [ -e "$SRC/$dir/$name/newdir" ] ||
		[ -e "$SRC/$dir/$name/f" ] || [ -e "$SRC/$dir/$name.moved" ]; then
		fail "$tag-untouched" "$(ls -a "$SRC/$dir/$name" "$SRC/$dir")"
	else
		pass "$tag-untouched"
	fi
	if [ -e "$SRC/f" ]; then
		pass "$tag-source-kept"
	else
		fail "$tag-source-kept" "$SRC/f is gone"
	fi
}

# log_once TAG LOG PATTERN: exactly one line of LOG matches PATTERN.
log_once() {
	n=$(grep -c "$3" "$2")
	if [ "$n" -eq 1 ]; then
		pass "$1"
	else
		fail "$1" "want 1 line matching '$3', got $n: $(grep "$3" "$2")"
	fi
}

# --- build the backing tree -------------------------------------------------

mount /dev/vdb /src
FSTYPE=$(backing_fstype "$SRC")
echo data >/src/f
# Never listed through dcfs before the boundaries appear in it.
mkdir /src/d /src/d/mp
echo covered >/src/d/mp/covered
sync

mkdir -p /cache /mnt
if start_daemon "$LOG1"; then
	pass mount
else
	fail mount "daemon did not mount within 10s"
	exit "$FAILED"
fi
stat "$MNT/f" >/dev/null

mount /dev/vdc /src/d/mp
chmod 751 /src/d/mp
echo inner >/src/d/mp/inner
subvol=0
if [ "$FSTYPE" = btrfs ]; then
	if "$TESTUTIL" btrfs-subvol-create /src/d/subvol >/tmp/subvol.out 2>&1; then
		echo inner >/src/d/subvol/inner
		chmod 705 /src/d/subvol
		subvol=1
	else
		fail btrfs-subvol-create "$(cat /tmp/subvol.out)"
	fi
fi
sync

check_stub mount d mp
if [ "$subvol" -eq 1 ]; then
	check_stub subvol d subvol
else
	skip subvol "backing filesystem is $FSTYPE, not btrfs"
fi

log_once mount-refusal-logged-once "$LOG1" "refusing to cache mp"
log_once mount-enotsup-logged-once "$LOG1" "inside the boundary stub mp"
if [ "$subvol" -eq 1 ]; then
	log_once subvol-refusal-logged-once "$LOG1" "refusing to cache subvol"
	log_once subvol-enotsup-logged-once "$LOG1" "inside the boundary stub subvol"
fi

# --- after a restart: the same stubs, from the cache -------------------------
#
# dcfs refuses to start with a mount below the source, so vdc goes first.
# The cached stub stays (nothing relists d), as any cached entry stays
# after a change behind dcfs's back; the subvolume on btrfs is still there.

umount /src/d/mp
if restart_daemon restart "$LOG2"; then
	ino=$(stat -c %i "$MNT/d/mp" 2>&1)
	if [ "$ino" = "$STUB_INO_mount" ]; then
		pass restart-same-ino
	else
		fail restart-same-ino "before $STUB_INO_mount, after $ino"
	fi
	case " $(ls -a "$MNT/d" | tr '\n' ' ') " in
	*" mp "*) pass restart-listed ;;
	*) fail restart-listed "$(ls -a "$MNT/d")" ;;
	esac
	expect_fail restart-ls-inside "not supported" ls "$MNT/d/mp/"
	if [ "$subvol" -eq 1 ]; then
		ino=$(stat -c %i "$MNT/d/subvol" 2>&1)
		if [ "$ino" = "$STUB_INO_subvol" ]; then
			pass restart-subvol-same-ino
		else
			fail restart-subvol-same-ino "before $STUB_INO_subvol, after $ino"
		fi
	fi
	# The cache answered: the boundary was not probed again.
	if grep -q "refusing to cache" "$LOG2"; then
		fail restart-from-cache "$(grep "refusing to cache" "$LOG2")"
	else
		pass restart-from-cache
	fi
fi

# The covered directory's own entry is never served.
if [ -e "$MNT/d/mp/covered" ]; then
	fail covered-hidden "$MNT/d/mp/covered is visible"
else
	pass covered-hidden
fi

exit "$FAILED"
