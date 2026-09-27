#!/bin/sh
# dcfs step 3.2 acceptance test: read-only ops served from the cache.
#
# Builds a small tree spanning two backing filesystems (vdb, with vdc
# mounted as a real submount below it), mounts dcfs over it, and checks:
# metadata matches the backing filesystems (inode numbers, a stat-based
# directory listing, ENOENT on a missing name); a second listing pass -- with
# the page/dentry/inode caches dropped in between -- causes *zero* additional
# block reads on either backing device (dcfs's own sqlite cache answers it,
# per step 3.2's design); and that this stays true after killing and
# restarting the daemon against the same cache database (i.e. the cache
# persisted).
#
# Uses only busybox applets/options (verified against the exact busybox
# baked into the initramfs: `busybox --list`, `busybox <applet> --help`) --
# no GNU find/stat/coreutils extensions.
#
# Run as /tests/readonly.sh by guest/init when booted with
# dcfs_test=readonly.sh; prints one "TEST ... PASS/FAIL" line per check and
# exits nonzero if any check failed. init turns that into the final
# ALL-TESTS-PASSED / TEST-FAILED verdict.
FAILED=0
pass() { echo "TEST $1 PASS"; }
fail() { echo "TEST $1 FAIL ($2)"; FAILED=1; }

DCFS=/bin/dcfs

# busybox on Ubuntu lacks the mountpoint applet; ask the kernel directly.
is_mounted() { grep -q " $1 " /proc/mounts; }
SRC=/src
MNT=/mnt
DB=/cache/dcfs.db
LOG1=/tmp/dcfs-1.log
LOG2=/tmp/dcfs-2.log

DAEMON_PID=""
MOUNTED=0

# Runs on every exit, including one `set -e` (imposed by init's `sh -e`)
# triggers on an unguarded failing command -- so every command in here is
# guarded (`|| true`): a failure while cleaning up must never mask, or cut
# short, the cleanup itself. Dumps both daemon logs whenever the script is
# exiting non-zero, whether via an explicit `exit` after a tracked fail() or
# via `set -e` aborting on something unexpected -- either way "$?" here is
# that exit's status.
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

echo "readonly.sh: kernel $(uname -r)"

# --- helpers -----------------------------------------------------------

# Field 3 of /sys/block/<dev>/stat is the cumulative count of sectors read
# from that block device since boot -- see Documentation/ABI/stable/
# sysfs-block. Unchanged across a pass means dcfs made no backing I/O.
sectors_read() {
	read -r line <"/sys/block/$1/stat"
	set -- $line
	echo "$3"
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

# Populates $1 with a 3-level directory tree, ~50 regular files with
# content, a symlink, a hard-link pair, and an empty directory.
populate_tree() {
	root=$1
	mkdir -p "$root/d1/d2/d3"
	mkdir -p "$root/empty"
	i=0
	while [ "$i" -lt 50 ]; do
		case $((i % 3)) in
		0) f="$root/file_$i.txt" ;;
		1) f="$root/d1/file_$i.txt" ;;
		*) f="$root/d1/d2/file_$i.txt" ;;
		esac
		echo "content $i in $root" >"$f"
		i=$((i + 1))
	done
	echo "deep content" >"$root/d1/d2/d3/deep.txt"
	ln -s d1/file_1.txt "$root/link_to_file"
	echo "hardlinked content" >"$root/hardlink1"
	ln "$root/hardlink1" "$root/hardlink2"
}

# A stat listing (read from file $1; see run_pass -- one line per entry,
# "inode perms nlink owner group size name", where a symlink's name is
# 'name' -> 'target' per busybox stat's %N) with the path prefix $2 rewritten
# out of the name, so two trees' listings compare equal regardless of which
# of /src / /mnt they were taken from. The inode number is deliberately kept
# (not dropped): dcfs reports the *backing* inode as st_ino, so it must
# already agree between the two sides. A relative symlink's target half
# never starts with the prefix, so it passes through untouched.
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

# One listing pass: everything read-only that step 3.2 implements. No
# content reads -- Open/Read are still ENOSYS stubs as of this step. busybox
# find has no -ls, so this uses -exec stat -c ... {} + instead (also
# replacing the old separate "stat every file" pass: this stats every entry,
# not just regular files).
run_pass() {
	dir=$1
	find "$dir" -exec stat -c '%i %A %h %U %G %s %N' {} + >/tmp/pass_stat.txt
	readlink "$dir/link_to_file" >/dev/null
	readlink "$dir/sub/link_to_file" >/dev/null
}

# --- build the backing tree, across a real submount ---------------------

mount /dev/vdb /src
mkdir -p /src/sub
mount /dev/vdc /src/sub
populate_tree /src
populate_tree /src/sub
sync

# --- mount dcfs and run the cold pass ------------------------------------

mkdir -p /cache /mnt
if start_daemon "$LOG1"; then
	pass mount
else
	fail mount "daemon did not mount within 10s"
	exit "$FAILED"
fi

run_pass "$MNT" # pass 1 (cold): populates the cache from the backing tree.

# --- warm pass: dcfs must not touch the backing devices at all ----------

sync
echo 3 >/proc/sys/vm/drop_caches
before_vdb=$(sectors_read vdb)
before_vdc=$(sectors_read vdc)

run_pass "$MNT" # pass 2: everything above should already be cached.

after_vdb=$(sectors_read vdb)
after_vdc=$(sectors_read vdc)
if [ "$after_vdb" = "$before_vdb" ]; then
	pass warm-metadata-vdb
else
	fail warm-metadata-vdb "sectors_read(vdb) $before_vdb -> $after_vdb"
fi
if [ "$after_vdc" = "$before_vdc" ]; then
	pass warm-metadata-vdc
else
	fail warm-metadata-vdc "sectors_read(vdc) $before_vdc -> $after_vdc"
fi

# Prove sectors_read actually notices real I/O: reading a file's data
# directly off vdb (dcfs has no Read yet, so this bypasses it entirely)
# must move the counter that pass 2 just proved dcfs leaves alone.
before_sanity=$after_vdb
cat /src/file_0.txt >/dev/null
after_sanity=$(sectors_read vdb)
if [ "$after_sanity" -gt "$before_sanity" ]; then
	pass counter-sanity
else
	fail counter-sanity "sectors_read(vdb) $before_sanity -> $after_sanity"
fi

# --- identity checks ------------------------------------------------------

src_ino=$(stat -c %i /src/file_0.txt)
mnt_ino=$(stat -c %i /mnt/file_0.txt)
if [ "$src_ino" = "$mnt_ino" ]; then
	pass st-ino-backing
else
	fail st-ino-backing "src=$src_ino mnt=$mnt_ino"
fi

sub_src_ino=$(stat -c %i /src/sub/file_0.txt)
sub_mnt_ino=$(stat -c %i /mnt/sub/file_0.txt)
if [ "$sub_src_ino" = "$sub_mnt_ino" ]; then
	pass st-ino-submount
else
	fail st-ino-submount "src=$sub_src_ino mnt=$sub_mnt_ino"
fi

find /src -exec stat -c '%i %A %h %U %G %s %N' {} + >/tmp/src_stat.txt
find /mnt -exec stat -c '%i %A %h %U %G %s %N' {} + >/tmp/mnt_stat.txt
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
fi

if ls /mnt/nope >/dev/null 2>&1; then
	fail lookup-enoent "ls /mnt/nope unexpectedly succeeded"
else
	pass lookup-enoent
fi

# --- restart with the same cache db: the cache must have persisted ------

kill -TERM "$DAEMON_PID" 2>/dev/null || true
wait "$DAEMON_PID" 2>/dev/null || true
DAEMON_PID=""
if is_mounted "$MNT"; then
	# libfuse's signal handler should have unmounted on its own; fall back
	# to forcing it so the restart below isn't blocked by a stale mount.
	echo "readonly.sh: /mnt still mounted after SIGTERM; forcing umount"
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
before_vdc=$(sectors_read vdc)

run_pass "$MNT" # pass 3: same cache db, fresh process -- still cached.

after_vdb=$(sectors_read vdb)
after_vdc=$(sectors_read vdc)
if [ "$after_vdb" = "$before_vdb" ]; then
	pass warm-after-restart-vdb
else
	fail warm-after-restart-vdb "sectors_read(vdb) $before_vdb -> $after_vdb"
fi
if [ "$after_vdc" = "$before_vdc" ]; then
	pass warm-after-restart-vdc
else
	fail warm-after-restart-vdc "sectors_read(vdc) $before_vdc -> $after_vdc"
fi

exit "$FAILED"
