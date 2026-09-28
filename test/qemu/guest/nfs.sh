#!/bin/sh
# dcfs step 5.3 acceptance test: dcfs actually exported over real NFSv4,
# not just a synthetic fhtest client.
#
# Unlike every other guest/*.sh script, this one runs chrooted into a small
# Debian tree (test/qemu/scripts/mkrootfs-debian.sh, attached via
# qemu_test's rootfs= attribute; see guest/init's dcfs_rootfs= branch), so
# GNU coreutils/findutils and nfs-utils (rpc.nfsd, rpc.mountd, exportfs,
# mount.nfs4) are available -- none of which fit in the busybox-only
# initramfs every other test runs from.
#
# Builds a tree on vdb, mounts dcfs over it, exports the dcfs mount itself
# over loopback NFSv4 (fsid=0, so it's the v4 pseudo-root -- no extra path
# segment to strip on the client side), and loopback-mounts it back in.
# Checks:
#
#   - nfs-mount / nfs-listing-matches: the NFS mount succeeds and a
#     normalized find+stat listing through it matches the same listing
#     taken directly on /src.
#   - nfs-warm-metadata-zero: after dropping every cache (page cache,
#     dentries, inodes -- the NFS client's own attribute/dentry cache
#     included, since it's ordinary VFS state on this same kernel), a
#     second metadata pass through NFS causes zero additional block reads
#     on vdb: dcfs answered every GETATTR/LOOKUP/READDIR nfsd forwarded to
#     it from its own cache, exactly as readonly.sh already proved for
#     local FUSE access.
#   - nfs-content-read: `cat` through NFS matches /src and -- unlike
#     metadata -- does move vdb's block-read counter.
#   - nfs-handle-survives-restart: the core of this step. Opens a file over
#     NFS and keeps the fd, reads part of it, SIGTERMs dcfs, waits for it to
#     exit, starts a fresh dcfs against the *same* cache database (nfsd
#     itself is never touched -- only the FUSE backend under its export
#     bounces), and keeps reading from the same fd: the kernel's FUSE
#     export_operations reconnect the stale nodeid with a LOOKUP(nodeid,
#     ".") against the new daemon (see README.md's "Identity model"), so
#     this must succeed with no ESTALE/EIO. A fresh listing must also still
#     work.
#   - nfs-write: write-through (step 4.4, merged into main since this
#     script was first drafted against a pre-4.4 dcfs that refused every
#     non-read-only open with EROFS) works through NFS too: a write
#     succeeds, lands on the backing file, and reads back correctly both
#     from the backing path and back through NFS.
#   - nfs-db-wipe-estale: the one case that does *not* survive (see
#     README.md's "Identity model" again): closes and reopens the handle,
#     stops dcfs, wipes the cache database, starts a cold dcfs, and reads
#     from the pre-wipe fd -- this must fail, and specifically with ESTALE
#     ("Stale file handle"), not merely "some error". A brand new mount and
#     `cat` afterwards must still work fine against the cold cache.
#
# Also step 4.8's runtime submount refusal (amendment 12), using vdc as a
# second filesystem mounted below the source after dcfs starts: this is the
# one script that can show the refusal is transparent through a real NFS
# re-export, not just local FUSE access. `exportfs`'s crossmnt option is
# left on (it is what would make a submount visible to an NFS client
# without a second export) specifically to demonstrate that it is now
# inert: dcfs itself never crosses the boundary, so there is nothing left
# for crossmnt to reveal (nfs-boundary-* below). (The startup-refusal half
# of amendment 12 is exercised once, in readonly.sh.)
#
# Run as /tests/nfs.sh (chrooted; see guest/init) when booted with
# dcfs_test=nfs.sh; prints one "TEST ... PASS/FAIL" line per check and
# exits nonzero if any check failed.
FAILED=0
pass() { echo "TEST $1 PASS"; }
fail() { echo "TEST $1 FAIL ($2)"; FAILED=1; }

DCFS=/usr/local/bin/dcfs

is_mounted() { grep -q " $1 " /proc/mounts; }
SRC=/src
MNT=/mnt
NFS=/nfs
DB=/cache/dcfs.db
LOG1=/tmp/dcfs-1.log
LOG2=/tmp/dcfs-2.log
LOG3=/tmp/dcfs-3.log

A_CONTENT="dcfs_nfs_handle_survives_restart_0123456789"

DAEMON_PID=""
MOUNTED=0
NFS_MOUNTED=0
MOUNTD_PID=""
IDMAPD_PID=""
FD3_OPEN=0

# See readonly.sh for why every command here is `|| true`-guarded: this
# must run to completion (and dump every daemon log on any failure)
# regardless of how the script is exiting.
cleanup() {
	rc=$?
	if [ "$rc" -ne 0 ] || [ "$FAILED" -ne 0 ]; then
		echo "--- dcfs stderr (run 1) ---"
		cat "$LOG1" 2>/dev/null
		echo "--- dcfs stderr (run 2) ---"
		cat "$LOG2" 2>/dev/null
		echo "--- dcfs stderr (run 3) ---"
		cat "$LOG3" 2>/dev/null
	fi
	if [ "$FD3_OPEN" -eq 1 ]; then
		exec 3<&- 2>/dev/null || true
	fi
	if [ "$NFS_MOUNTED" -eq 1 ]; then
		umount -f "$NFS" 2>/dev/null || true
	fi
	if [ -n "$MOUNTD_PID" ] && kill -0 "$MOUNTD_PID" 2>/dev/null; then
		kill "$MOUNTD_PID" 2>/dev/null || true
		wait "$MOUNTD_PID" 2>/dev/null || true
	fi
	# The supervisor loop's own PID above may be blocked in wait() for
	# whichever rpc.mountd child is currently running when the signal
	# arrives; make sure that child dies too.
	pkill -TERM -x rpc.mountd 2>/dev/null || true
	if [ -n "$IDMAPD_PID" ] && kill -0 "$IDMAPD_PID" 2>/dev/null; then
		kill "$IDMAPD_PID" 2>/dev/null || true
		wait "$IDMAPD_PID" 2>/dev/null || true
	fi
	exportfs -ua 2>/dev/null || true
	rpc.nfsd 0 2>/dev/null || true
	if [ "$MOUNTED" -eq 1 ]; then
		umount "$MNT" 2>/dev/null || true
	fi
	if [ -n "$DAEMON_PID" ] && kill -0 "$DAEMON_PID" 2>/dev/null; then
		kill "$DAEMON_PID" 2>/dev/null || true
		wait "$DAEMON_PID" 2>/dev/null || true
	fi
}
trap cleanup EXIT

echo "nfs.sh: kernel $(uname -r)"

# --- helpers ---------------------------------------------------------------

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
	"$DCFS" --source="$SRC" --cache_db="$DB" --allow_other "$MNT" >"$1" 2>&1 &
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

# SIGTERM's the running daemon, waits for it, force-umounts if libfuse's own
# signal handler didn't already unmount, then starts a fresh daemon against
# the same cache database, logging to $2. Emits "$1-unmount" / "$1-mount"
# PASS/FAIL lines. Returns nonzero if the daemon did not come back up.
# Deliberately does NOT touch $NFS, rpc.nfsd or rpc.mountd: the whole point
# is that only the FUSE backend under the export bounces.
restart_daemon() {
	kill -TERM "$DAEMON_PID" 2>/dev/null || true
	wait "$DAEMON_PID" 2>/dev/null || true
	DAEMON_PID=""
	if is_mounted "$MNT"; then
		echo "nfs.sh: /mnt still mounted after SIGTERM; forcing umount"
		umount "$MNT" 2>/dev/null || true
	fi
	if is_mounted "$MNT"; then
		fail "$1-unmount" "mountpoint still mounted after kill+umount"
		MOUNTED=1
	else
		pass "$1-unmount"
		MOUNTED=0
	fi
	if start_daemon "$2"; then
		pass "$1-mount"
		# nfsd's own export cache (distinct from anything dcfs does) can
		# hold a reference tied to the *old* /mnt vfsmount/dentry; without
		# this, every NFS request -- held-fd reads included, since this
		# is nfsd-side state, not the FUSE-level nodeid reconnection --
		# comes back EIO until that cache entry's own (long) TTL expires.
		# -f (flush) forces it to re-resolve the export path fresh, which
		# picks up the new mount. This is ordinary NFS server administration
		# after remounting a re-exported filesystem, not a workaround for
		# anything dcfs-specific.
		exportfs -f 2>/dev/null || true
		return 0
	fi
	fail "$1-mount" "daemon did not remount within 10s"
	return 1
}

# A stat listing normalized the same way readonly.sh does it, so two trees'
# listings compare equal regardless of which prefix they were taken from.
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

# "d" is excluded: it exists only to host the nfs-boundary-* checks below,
# and by design diverges between /src (real, once vdc is unmounted) and the
# view through dcfs (permanently missing "mp": see the boundary-* checks) --
# see readonly.sh's identity-checks comment for the full explanation.
find_stat_tree() {
	root=$1
	fmt=$2
	find "$root" -path "$root/d" -prune -o -exec stat -c "$fmt" {} + 2>/dev/null
}

listing_matches() {
	label=$1
	dir_a=$2
	prefix_a=$3
	dir_b=$4
	prefix_b=$5
	find_stat_tree "$dir_a" '%i %A %h %U %G %s %N' >/tmp/nfs_stat_a.txt
	find_stat_tree "$dir_b" '%i %A %h %U %G %s %N' >/tmp/nfs_stat_b.txt
	normalize_stat /tmp/nfs_stat_a.txt "$prefix_a" | sort >/tmp/nfs_stat_a_norm.txt
	normalize_stat /tmp/nfs_stat_b.txt "$prefix_b" | sort >/tmp/nfs_stat_b_norm.txt
	if diff -u /tmp/nfs_stat_a_norm.txt /tmp/nfs_stat_b_norm.txt >/tmp/nfs_stat_diff.txt; then
		pass "$label"
	else
		fail "$label" "normalized listings differ; see nfs_stat_diff.txt in the log"
		cat /tmp/nfs_stat_diff.txt
	fi
}

# 20 files spread under $1, for the listing-matches / warm-metadata checks.
populate_tree() {
	root=$1
	mkdir -p "$root/d1/d2"
	i=0
	while [ "$i" -lt 20 ]; do
		case $((i % 2)) in
		0) f="$root/file_$i.txt" ;;
		*) f="$root/d1/file_$i.txt" ;;
		esac
		echo "tree content $i in $root" >"$f"
		i=$((i + 1))
	done
}

# --- build the backing tree -------------------------------------------------
# (unlike the busybox-only initramfs, guest/init does not pre-create these
# inside the Debian chroot)

mkdir -p /src /cache /mnt /nfs
mount /dev/vdb /src
populate_tree /src
printf '%s' "$A_CONTENT" >/src/a.txt
dd if=/dev/urandom of=/src/content.bin bs=1M count=4 2>/dev/null
# "d" is for nfs-boundary-* below: it must never be listed through dcfs
# before vdc is mounted under it at runtime.
mkdir -p /src/d
sync
set -- $(md5sum /src/content.bin)
src_content_md5=$1

# --- mount dcfs -------------------------------------------------------------

mkdir -p /cache /mnt
if start_daemon "$LOG1"; then
	pass mount
else
	fail mount "daemon did not mount within 10s"
	exit "$FAILED"
fi

# --- nfs-boundary-*: a filesystem mounted below the source at runtime -----
# --- is refused locally (amendment 12) before it is ever exported --------

mkdir /src/d/mp
mount /dev/vdc /src/d/mp
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

# --- export dcfs over NFSv4, loopback --------------------------------------
#
# None of rpcbind/nfsd-fs/rpc_pipefs get set up by anything else here (no
# systemd), and all three turn out to matter even for a v4-only export:
#   - /proc/fs/nfsd (the "nfsd" pseudo-filesystem) is how rpc.nfsd/exportfs
#     talk to the kernel's nfsd at all.
#   - rpc_pipefs is how the kernel's sunrpc cache upcalls (auth.unix.ip,
#     nfsd.export, nfsd.fh -- used to decide whether a client's request
#     matches an export) reach rpc.mountd for an answer. Without it
#     mounted, an upcall a client's first request triggers has no listener
#     and just sits unanswered -- this is what made the very first
#     mount.nfs4 attempt hang indefinitely (a hard mount retries forever)
#     rather than fail.
#   - rpcbind: rpc.mountd registers itself (and would register the MOUNT
#     v2/v3 services, harmless failures here since only v4 is enabled) via
#     rpcbind; without it running, rpc.mountd's log fills with "Connection
#     refused" registration failures. Not required by the NFSv4 protocol
#     itself (v4 has no separate MOUNT/portmapper step), but starting it
#     anyway removes a source of rpc.mountd startup noise/uncertainty.
mkdir -p /proc/fs/nfsd
mount -t nfsd nfsd /proc/fs/nfsd 2>/dev/null || true

mkdir -p /var/lib/nfs/rpc_pipefs
mount -t rpc_pipefs sunrpc /var/lib/nfs/rpc_pipefs 2>/dev/null || true

rpcbind 2>/tmp/rpcbind.log || true

# NFSv4 (unlike v2/v3) represents ownership as strings ("1000@localdomain"),
# which needs rpc.idmapd running to translate; nothing else here starts it
# (no systemd). Cheap to start unconditionally even though sec=sys/AUTH_SYS
# mostly sidesteps it -- absent, some client-attach-adjacent codepath may
# be part of what crashes rpc.mountd below.
rpc.idmapd -f >/tmp/idmapd.log 2>&1 &
IDMAPD_PID=$!
sleep 1

# Client recovery tracking: nfsd needs somewhere to persist NFSv4
# client-recovery state. test/qemu/scripts/build-kernel.sh enables
# CONFIG_NFSD_LEGACY_CLIENT_TRACKING for exactly this test, so nfsd
# manages /var/lib/nfs/v4recovery itself with no upcall to any userland
# daemon -- see that script's comment for the full story: the upcall
# path (nfsdcld, or the older rpc.mountd-handled upcall it falls back to)
# is what this guest had before that kernel config existed, and
# rpc.mountd's handling of that legacy upcall segfaulted reproducibly on
# the first client's SETCLIENTID, hanging the client's mount(2) forever.
if rpc.nfsd --no-nfs-version 3 --nfs-version 4 8; then
	pass nfsd-start
else
	fail nfsd-start "rpc.nfsd exited nonzero"
	exit "$FAILED"
fi

if exportfs -o rw,fsid=0,no_subtree_check,crossmnt,no_root_squash 127.0.0.1:"$MNT"; then
	pass exportfs
else
	fail exportfs "exportfs exited nonzero"
	exit "$FAILED"
fi

rpc.mountd --no-nfs-version 3 --nfs-version 4 --foreground >/tmp/mountd.log 2>&1 &
MOUNTD_PID=$!
sleep 1
if ! kill -0 "$MOUNTD_PID" 2>/dev/null; then
	fail mountd-start "rpc.mountd exited immediately; see mountd.log"
	cat /tmp/mountd.log
	exit "$FAILED"
fi
pass mountd-start

mkdir -p "$NFS"
i=0
nfs_mount_ok=0
while [ "$i" -lt 10 ]; do
	if timeout 10 mount -t nfs4 -o vers=4.0,nolock 127.0.0.1:/ "$NFS" 2>/tmp/nfs_mount.err; then
		nfs_mount_ok=1
		break
	fi
	i=$((i + 1))
	sleep 1
done
if [ "$nfs_mount_ok" -eq 1 ]; then
	NFS_MOUNTED=1
	pass nfs-mount
else
	fail nfs-mount "mount -t nfs4 did not succeed within 10s"
	cat /tmp/nfs_mount.err
	echo "--- diag: nfsd versions ---"
	cat /proc/fs/nfsd/versions 2>&1
	echo "--- diag: dmesg tail ---"
	dmesg | tail -60
	echo "--- diag: mountd.log ---"
	cat /tmp/mountd.log
	exit "$FAILED"
fi

# --- nfs-boundary-crossmnt: crossmnt (enabled in the exportfs call above)
# reveals nothing for "mp", since dcfs itself already refused to cross into
# it (see boundary-* above) -- the whole point of amendment 12: identity
# stays local to dcfs's own st_dev regardless of what an NFS export option
# asks for. Only "not listed" and "not statable" are asserted; the specific
# error an NFS client sees for a server-side EXDEV is nfsd's own errno ->
# NFS status mapping (nfserrno()), not something amendment 12 specifies.
nfs_listing=$(ls -1 "$NFS/d" 2>&1)
case "$nfs_listing" in
*mp*) fail nfs-boundary-not-listed "mp appeared in $NFS/d: $nfs_listing" ;;
*) pass nfs-boundary-not-listed ;;
esac
if stat "$NFS/d/mp" >/dev/null 2>&1; then
	fail nfs-boundary-stat-fails "unexpectedly succeeded"
else
	pass nfs-boundary-stat-fails
fi
umount /src/d/mp

# --- nfs-listing-matches: /src vs /nfs -------------------------------------

listing_matches nfs-listing-matches /src "$SRC" "$NFS" "$NFS"

# --- nfs-warm-metadata-zero: a fresh (post-drop_caches) metadata pass over
# NFS must not touch either backing device -- this also drops the NFS
# client's own attribute/dentry cache, since that's ordinary inode/dentry
# state on this same kernel, so the second pass is a genuine round trip
# through nfsd and dcfs, not served from the client's cache. -------------

drop_caches
before_vdb=$(sectors_read vdb)
find_stat_tree "$NFS" '%i %s %n' >/tmp/nfs_warm1.txt
after_vdb=$(sectors_read vdb)
if [ "$after_vdb" = "$before_vdb" ]; then
	pass nfs-warm-metadata-zero-vdb
else
	fail nfs-warm-metadata-zero-vdb "sectors_read(vdb) $before_vdb -> $after_vdb"
fi

# --- nfs-content-read: content differs from metadata -- it must hit vdb --

drop_caches
before_vdb=$(sectors_read vdb)
nfs_content=$(cat "$NFS/content.bin" | md5sum | awk '{print $1}')
after_vdb=$(sectors_read vdb)
if [ "$nfs_content" = "$src_content_md5" ]; then
	pass nfs-content-read-matches
else
	fail nfs-content-read-matches "src=$src_content_md5 nfs=$nfs_content"
fi
if [ "$after_vdb" -gt "$before_vdb" ]; then
	pass nfs-content-read-hits-disk
else
	fail nfs-content-read-hits-disk "sectors_read(vdb) $before_vdb -> $after_vdb"
fi

# --- nfs-handle-survives-restart -------------------------------------------

exec 3<"$NFS/a.txt"
FD3_OPEN=1
part1=$(dd bs=1 count=10 2>/dev/null <&3)
if [ "$part1" = "$(printf '%s' "$A_CONTENT" | dd bs=1 count=10 2>/dev/null)" ]; then
	pass nfs-handle-partial-read
else
	fail nfs-handle-partial-read "got '$part1'"
fi

if restart_daemon nfs-handle-survives-restart "$LOG2"; then
	part2=$(cat <&3 2>/tmp/part2.err)
	rc2=$?
	if [ "$rc2" -eq 0 ] && [ "$part1$part2" = "$A_CONTENT" ]; then
		pass nfs-handle-read-after-restart
	else
		fail nfs-handle-read-after-restart "rc=$rc2 got '$part1$part2' want '$A_CONTENT'"
		cat /tmp/part2.err
	fi

	listing_matches nfs-listing-matches-after-restart /src "$SRC" "$NFS" "$NFS"
else
	fail nfs-handle-read-after-restart "daemon did not come back up"
	fail nfs-listing-matches-after-restart "daemon did not come back up"
fi
exec 3<&-
FD3_OPEN=0

# --- nfs-write: write-through works over NFS too (step 4.4, merged into
# main after this script was first drafted against a pre-4.4 dcfs that
# refused every non-read-only open with EROFS) -----------------------------

WRITE_CONTENT="dcfs_nfs_write_through_test"
write_ok=0
if printf '%s' "$WRITE_CONTENT" >"$NFS/write_test.txt" 2>/tmp/nfs_write.err; then write_ok=1; fi
if [ "$write_ok" -eq 1 ]; then
	pass nfs-write-succeeds
else
	fail nfs-write-succeeds "write through NFS failed: $(cat /tmp/nfs_write.err)"
fi

backing_content=$(cat "$SRC/write_test.txt" 2>/dev/null) || true
if [ "$backing_content" = "$WRITE_CONTENT" ]; then
	pass nfs-write-lands-on-backing
else
	fail nfs-write-lands-on-backing "got '$backing_content'"
fi

nfs_reread=$(cat "$NFS/write_test.txt" 2>/dev/null) || true
if [ "$nfs_reread" = "$WRITE_CONTENT" ]; then
	pass nfs-write-reread-matches
else
	fail nfs-write-reread-matches "got '$nfs_reread'"
fi

# --- nfs-db-wipe-estale: the one case that does NOT survive -----------------

exec 3<"$NFS/a.txt"
FD3_OPEN=1

kill -TERM "$DAEMON_PID" 2>/dev/null || true
wait "$DAEMON_PID" 2>/dev/null || true
DAEMON_PID=""
if is_mounted "$MNT"; then
	umount "$MNT" 2>/dev/null || true
fi
if is_mounted "$MNT"; then
	fail db-wipe-unmount "mountpoint still mounted after kill+umount"
	MOUNTED=1
else
	pass db-wipe-unmount
	MOUNTED=0
fi

rm -f "$DB" "$DB-wal" "$DB-shm" "$DB-journal"

if start_daemon "$LOG3"; then
	pass db-wipe-mount
	# exportfs -f (see restart_daemon's comment) plus a full nfsd bounce:
	# nfsd keeps its own server-side cache of recently-opened files
	# (nfsd_file_cache), independent of both dcfs and the export table,
	# and a file already open in that cache from before the wipe can go
	# on serving reads without ever re-resolving the client's filehandle
	# against the (now cold) daemon -- which would hide exactly the
	# property this check exists to demonstrate. Stopping and restarting
	# nfsd's thread pool drops that cache along with everything else
	# nfsd owns, forcing the next request to resolve fresh.
	exportfs -f 2>/dev/null || true
	rpc.nfsd 0
	rpc.nfsd --no-nfs-version 3 --nfs-version 4 8
else
	fail db-wipe-mount "daemon did not mount within 10s (cold cache)"
fi

if [ "$MOUNTED" -eq 1 ]; then
	# drop_caches here is load-bearing, not just cache-warmth hygiene: the
	# NFS client's own attribute/dentry cache can otherwise keep serving
	# this fd's pre-wipe content without ever sending a fresh request the
	# server would have to re-resolve against the (now cold) daemon --
	# which would hide exactly the property this check exists to
	# demonstrate. Dropping it forces a real round trip.
	drop_caches
	before_stale_vdb=$(sectors_read vdb)
	stale_out=$(cat <&3 2>/tmp/stale.err)
	stale_rc=$?
	after_stale_vdb=$(sectors_read vdb)
	exec 3<&-
	FD3_OPEN=0
	if [ "$stale_rc" -ne 0 ]; then
		if [ "$after_stale_vdb" != "$before_stale_vdb" ]; then
			fail nfs-db-wipe-no-backing-io "sectors_read(vdb) $before_stale_vdb -> $after_stale_vdb (ESTALE should be caught before any backing read)"
		else
			pass nfs-db-wipe-no-backing-io
		fi
		if grep -qi stale /tmp/stale.err; then
			pass nfs-db-wipe-estale
		else
			fail nfs-db-wipe-estale "failed as expected but not with ESTALE: $(cat /tmp/stale.err)"
		fi
	else
		fail nfs-db-wipe-estale "read after db wipe unexpectedly succeeded: '$stale_out'"
	fi

	umount "$NFS" 2>/dev/null || true
	NFS_MOUNTED=0
	if timeout 10 mount -t nfs4 -o vers=4.0,nolock 127.0.0.1:/ "$NFS" 2>/tmp/nfs_remount.err; then
		NFS_MOUNTED=1
		pass nfs-remount-after-wipe
		fresh=$(cat "$NFS/a.txt" 2>/tmp/fresh.err)
		if [ "$fresh" = "$A_CONTENT" ]; then
			pass nfs-fresh-read-after-wipe
		else
			fail nfs-fresh-read-after-wipe "got '$fresh'"
			cat /tmp/fresh.err
		fi
	else
		fail nfs-remount-after-wipe "mount -t nfs4 failed after db wipe"
		cat /tmp/nfs_remount.err
	fi
else
	fail nfs-db-wipe-estale "daemon did not come back up"
	fail nfs-remount-after-wipe "daemon did not come back up"
	fail nfs-fresh-read-after-wipe "daemon did not come back up"
fi

exit "$FAILED"
