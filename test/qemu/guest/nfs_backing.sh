#!/bin/sh
# SPDX-License-Identifier: MIT
# Step 14.10's evidence: what an NFS client mount offers dcfs as a backing
# filesystem, measured on the guest's own kernel (no dcfs runs here). The
# design in docs/design.md ("Backings without a filesystem UUID") rests on
# these facts, so each is a check: a kernel that changes one fails this test
# before the design silently stops holding. Every measured value is also
# printed as a "FACT <name> <value>" line.
#
# The server is the guest's own knfsd exporting ext4 on vdb (and, for the
# identity checks, vdc), the client a loopback NFSv4.2 mount, chrooted into
# the Debian tree as nfs.sh is (nfs-utils does not fit in the busybox
# initramfs). Handles are taken and opened with testutil's handle-save and
# handle-stat (statically linked; fhtest is not, and does not run in the
# chroot). Checks:
#
#   - identity: FS_IOC_GETFSUUID on the client fails with ENOTTY; statfs
#     f_fsid is 0; the mount's source in mountinfo is the server and export
#     path as mounted; a hard mount says "hard" in its options; a second
#     mount of the same server filesystem shares the first one's superblock
#     (the same st_dev, and the first mount's source in mountinfo).
#   - handles: name_to_handle_at works for a file, a directory and the root;
#     the handle is the fileid (= the server's inode number), the file type
#     and the server's own handle, which carries the server filesystem's
#     inode number and generation; open_by_handle_at works on it, also after
#     the client dropped the inode.
#   - generations: FS_IOC_GETVERSION fails with ENOTTY on the client; the
#     birth time is the server's; a file recycled on the server (deleted,
#     and its inode number reused) leaves the old handle ESTALE, while the
#     new file's handle has the same fileid and other bytes.
#   - server restart (nfsd stopped, the exported filesystem unmounted and
#     mounted again, nfsd started): a handle taken before opens after, the
#     same file's handle is byte-identical, and a descriptor held open
#     across it keeps reading.
#   - export moved to another path with the same fsid: the old mount keeps
#     working and a new mount of the new path gives byte-identical handles.
#   - export's fsid changed: every old handle is ESTALE, the old mount's
#     root included.
#   - two filesystems exported under the same fsid number have
#     byte-identical root handles; under the default (UUID) fsid they differ.
#
# Run as /tests/nfs_backing.sh (chrooted; see guest/init) when booted with
# dcfs_test=nfs_backing.sh; prints one "TEST ... PASS/FAIL" line per check.
FAILED=0
. "$(dirname "$0")/lib.sh"
require_commands awk cat cut dd grep ls md5sum mkdir mount rm stat sync \
	umount uname rpcbind rpc.idmapd rpc.nfsd rpc.mountd exportfs pkill

TESTUTIL=/usr/local/bin/testutil

EXP=/exp
EXP2=/exp2
OTHER=/other
NFS=/nfs
NFS2=/nfs2
NFS3=/nfs3
NFS4=/nfs4
H=/tmp/handles
OPTS=rw,sync,no_subtree_check,no_root_squash
# A freshly started nfsd refuses OPEN for its grace period (default 90 s);
# see nfs.sh.
NFSD_ARGS="--grace-time 10 --lease-time 30 --no-nfs-version 3 --nfs-version 4 8"
FD3_OPEN=0

cleanup() {
	if [ "$FD3_OPEN" -eq 1 ]; then
		exec 3<&- 2>/dev/null || true
	fi
	for m in "$NFS4" "$NFS3" "$NFS2" "$NFS"; do
		if is_mounted "$m"; then umount -f "$m" 2>/dev/null || true; fi
	done
	exportfs -ua 2>/dev/null || true
	rpc.nfsd 0 2>/dev/null || true
	pkill -TERM -x rpc.mountd 2>/dev/null || true
	pkill -TERM -x rpc.idmapd 2>/dev/null || true
	for m in "$EXP" "$EXP2" "$OTHER"; do
		if is_mounted "$m"; then umount "$m" 2>/dev/null || true; fi
	done
}
trap cleanup EXIT

echo "nfs_backing.sh: kernel $(uname -r)"

fact() { echo "FACT $1 $2"; }

# expect NAME WANT GOT: passes NAME if GOT is WANT.
expect() {
	if [ "$3" = "$2" ]; then
		pass "$1"
	else
		fail "$1" "want '$2', got '$3'"
	fi
}

# save PATH NAME: PATH's file handle into $H/NAME ("<type> <hex>"); prints
# the line, or testutil's "ERR <errno>".
save() {
	"$TESTUTIL" handle-save "$1" "$H/$2" && cat "$H/$2"
}

# hex_of NAME: the handle bytes saved as NAME.
hex_of() { awk '{ print $2 }' "$H/$1"; }

# reopen MOUNT NAME: open_by_handle_at of the handle saved as NAME against
# MOUNT; prints "OK <size> <inode>" or "ERR <errno>".
reopen() {
	"$TESTUTIL" handle-stat "$1" "$H/$2" |
		awk '{ s = $1; for (i = 2; i <= 3 && i <= NF; i++) s = s " " $i; print s }'
}

# le32 HEX8: the 32-bit little-endian word HEX8 (8 hex digits) in decimal.
le32() {
	h=$1
	printf '%d' "0x$(echo "$h" | cut -c7-8)$(echo "$h" | cut -c5-6)$(echo "$h" | cut -c3-4)$(echo "$h" | cut -c1-2)"
}

# source_of MOUNTPOINT: the mount's source in mountinfo.
source_of() {
	awk -v m="$1" '$5 == m { for (i = 1; i <= NF; i++) if ($i == "-") { print $(i + 2); exit } }' /proc/self/mountinfo
}

# mount_nfs EXPORT MOUNTPOINT: a loopback NFSv4.2 hard mount.
mount_nfs() {
	mkdir -p "$2"
	mount -t nfs4 -o vers=4.2,hard 127.0.0.1:"$1" "$2" 2>/tmp/mount.err
}

# --- the server -------------------------------------------------------------

mkdir -p "$EXP" "$EXP2" "$OTHER" "$H"
mount /dev/vdb "$EXP"
echo "dcfs_nfs_backing_a" >"$EXP/a.txt"
mkdir "$EXP/d"
dd if=/dev/urandom of="$EXP/content.bin" bs=1M count=4 2>/dev/null
mount /dev/vdc "$OTHER"
echo "other filesystem" >"$OTHER/a.txt"
sync

mkdir -p /proc/fs/nfsd /var/lib/nfs/rpc_pipefs
mount -t nfsd nfsd /proc/fs/nfsd
mount -t rpc_pipefs sunrpc /var/lib/nfs/rpc_pipefs
rpcbind
# Without -f / --foreground both daemonize once they serve, so nothing
# here needs to wait for them.
rpc.idmapd
# shellcheck disable=SC2086 # NFSD_ARGS is a list of flags
rpc.nfsd $NFSD_ARGS
exportfs -o "$OPTS,fsid=1" 127.0.0.1:"$EXP"
rpc.mountd --no-nfs-version 3 --nfs-version 4
if mount_nfs "$EXP" "$NFS"; then
	pass nfs-mount
else
	fail nfs-mount "$(cat /tmp/mount.err)"
	exit "$FAILED"
fi

# --- identity ---------------------------------------------------------------

fact server-fsuuid "$("$TESTUTIL" fsuuid "$EXP")"
got=$("$TESTUTIL" fsuuid "$NFS")
fact client-fsuuid "$got"
expect client-fsuuid-enotty "ERR ENOTTY" "$got"

got=$(stat -f -c %i "$NFS")
fact client-statfs-fsid "$got"
expect client-statfs-fsid-zero 0 "$got"
fact client-statfs-type "$(stat -f -c %t "$NFS")"

line=$(awk -v m="$NFS" '$5 == m' /proc/self/mountinfo)
fact client-mountinfo "$line"
expect client-mountinfo-source "127.0.0.1:$EXP" "$(source_of "$NFS")"
case "$line" in
*,hard,* | *,hard) pass client-mountinfo-says-hard ;;
*) fail client-mountinfo-says-hard "no ',hard' in: $line" ;;
esac

# --- handles ----------------------------------------------------------------

out=$(save "$NFS/a.txt" a)
fact client-handle-a "$out"
ha=$(hex_of a)
htype=$(awk '{ print $1 }' "$H/a")
expect client-handle-type-is-length-in-words "$((${#ha} / 8))" "$htype"
ino=$(stat -c %i "$NFS/a.txt")
expect client-ino-is-server-ino "$(stat -c %i "$EXP/a.txt")" "$ino"
hi=$(le32 "$(echo "$ha" | cut -c1-8)")
lo=$(le32 "$(echo "$ha" | cut -c9-16)")
expect client-handle-starts-with-fileid "$ino" "$((hi * 4294967296 + lo))"
expect client-handle-has-file-type "$((0100000))" "$(le32 "$(echo "$ha" | cut -c17-24)")"
# The server's own handle of a.txt on ext4 (FILEID_INO32_GEN: inode number,
# generation) is inside the client's: the server's generation travels in the
# handle, opaque to the client.
fact server-handle-a "$(save "$EXP/a.txt" server_a)"
fact server-generation-a "$("$TESTUTIL" getversion "$EXP/a.txt")"
server_hex=$(hex_of server_a)
case "$ha" in
"" | *"$server_hex"*"$server_hex"*) fail client-handle-embeds-server-ino-and-generation "no handle, or the server's twice: $ha" ;;
*"$server_hex"*) pass client-handle-embeds-server-ino-and-generation ;;
*) fail client-handle-embeds-server-ino-and-generation "$server_hex not in $ha" ;;
esac
fact client-handle-dir "$(save "$NFS/d" d)"
fact client-handle-root-fsid1 "$(save "$NFS" root1)"
root1=$(hex_of root1)
if [ -n "$root1" ] && [ -n "$(hex_of d)" ]; then
	pass client-handles-for-dir-and-root
else
	fail client-handles-for-dir-and-root "no handle for $NFS or $NFS/d"
fi

expect open-by-handle "OK 19 $ino" "$(reopen "$NFS" a)"
drop_caches
expect open-by-handle-after-drop-caches "OK 19 $ino" "$(reopen "$NFS" a)"

# --- generations ------------------------------------------------------------

got=$("$TESTUTIL" getversion "$NFS/a.txt")
fact client-getversion "$got"
expect client-getversion-enotty "ERR ENOTTY" "$got"
got=$(stat -c %W "$NFS/a.txt")
fact client-btime "$got"
expect client-btime-is-servers "$(stat -c %W "$EXP/a.txt")" "$got"

# A file deleted on the server and its inode number reused (ext4 hands the
# lowest free number in the group out again; a few creates find it).
echo recycled >"$EXP/r.txt"
sync
r_ino=$(stat -c %i "$NFS/r.txt")
fact recycled-old-handle "$(save "$NFS/r.txt" r)"
rm "$EXP/r.txt"
sync
reused=""
for i in 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16; do
	echo "new $i" >"$EXP/n$i.txt"
	if [ "$(stat -c %i "$EXP/n$i.txt")" = "$r_ino" ]; then
		reused="n$i.txt"
		break
	fi
done
sync
drop_caches
if [ -z "$reused" ]; then
	fail recycled-inode-number "no new file got inode $r_ino"
else
	pass recycled-inode-number
	got=$(reopen "$NFS" r)
	fact recycled-old-handle-reopen "$got"
	expect recycled-old-handle-estale "ERR ESTALE" "$got"
	fact recycled-new-handle "$(save "$NFS/$reused" n)"
	r_hex=$(hex_of r)
	n_hex=$(hex_of n)
	expect recycled-new-handle-same-fileid "$(echo "$r_hex" | cut -c1-16)" "$(echo "$n_hex" | cut -c1-16)"
	if [ -n "$n_hex" ] && [ "$n_hex" != "$r_hex" ]; then
		pass recycled-new-handle-differs
	else
		fail recycled-new-handle-differs "'$n_hex' and '$r_hex'"
	fi
fi

# --- server restart ---------------------------------------------------------

exec 3<"$NFS/content.bin"
FD3_OPEN=1
dd bs=64k count=1 of=/tmp/part1.bin 2>/dev/null <&3
rpc.nfsd 0
exportfs -ua
if umount "$EXP"; then
	pass server-restart-unmounts-export
else
	fail server-restart-unmounts-export "umount $EXP failed"
fi
mount /dev/vdb "$EXP"
exportfs -o "$OPTS,fsid=1" 127.0.0.1:"$EXP"
# shellcheck disable=SC2086 # NFSD_ARGS is a list of flags
rpc.nfsd $NFSD_ARGS
drop_caches
expect restart-old-handle-opens "OK 19 $ino" "$(reopen "$NFS" a)"
save "$NFS/a.txt" a_restart >/dev/null
expect restart-handle-identical "$ha" "$(hex_of a_restart)"
cat <&3 >/tmp/part2.bin
exec 3<&-
FD3_OPEN=0
expect restart-held-descriptor-reads \
	"$(md5sum <"$EXP/content.bin" | awk '{ print $1 }')" \
	"$(cat /tmp/part1.bin /tmp/part2.bin | md5sum | awk '{ print $1 }')"

# --- export moved to another path, same fsid ----------------------------

exportfs -ua
umount "$EXP"
mount /dev/vdb "$EXP2"
exportfs -o "$OPTS,fsid=1" 127.0.0.1:"$EXP2"
drop_caches
got=$(reopen "$NFS" a)
fact moved-old-mount-old-handle "$got"
expect moved-old-mount-keeps-working "OK 19 $ino" "$got"
if mount_nfs "$EXP2" "$NFS2"; then
	pass moved-mount
	save "$NFS2/a.txt" a_moved >/dev/null
	save "$NFS2" root_moved >/dev/null
	expect moved-handle-identical "$ha" "$(hex_of a_moved)"
	expect moved-root-handle-identical "$root1" "$(hex_of root_moved)"
	expect moved-new-mount-opens-old-handle "OK 19 $ino" "$(reopen "$NFS2" a)"
	# The new mount is of the same server filesystem (fsid=1), so the
	# client shares the old mount's superblock: one st_dev, and mountinfo
	# names the first mount's source for both.
	dev1=$(stat -c %d "$NFS")
	dev2=$(stat -c %d "$NFS2")
	fact st-dev-mounts "$dev1 $dev2"
	expect moved-mount-shares-st-dev "$dev1" "$dev2"
	got=$(source_of "$NFS2")
	fact moved-mountinfo-source "$got"
	expect moved-mountinfo-source-is-first-mounts "127.0.0.1:$EXP" "$got"
else
	fail moved-mount "$(cat /tmp/mount.err)"
fi

# --- the export's fsid changed ----------------------------------------------

exportfs -ua
exportfs -o "$OPTS,fsid=2" 127.0.0.1:"$EXP2"
drop_caches
got=$(reopen "$NFS2" a)
fact fsid-changed-old-handle "$got"
expect fsid-changed-old-handle-estale "ERR ESTALE" "$got"
got=$(ls "$NFS2" 2>&1 >/dev/null)
fact fsid-changed-old-mount-root "$got"
case "$got" in
*"Stale file handle"*) pass fsid-changed-old-mount-root-stale ;;
*) fail fsid-changed-old-mount-root-stale "ls $NFS2: '$got'" ;;
esac

# --- one fsid number, two filesystems ---------------------------------------

exportfs -ua
exportfs -o "$OPTS,fsid=1" 127.0.0.1:"$OTHER"
if mount_nfs "$OTHER" "$NFS3"; then
	pass other-mount-fsid1
	fact client-handle-root-other-fsid1 "$(save "$NFS3" root_other1)"
	expect same-fsid-number-same-root-handle "$root1" "$(hex_of root_other1)"
	umount "$NFS3"
else
	fail other-mount-fsid1 "$(cat /tmp/mount.err)"
fi
exportfs -ua
exportfs -o "$OPTS" 127.0.0.1:"$EXP2"
exportfs -o "$OPTS" 127.0.0.1:"$OTHER"
if mount_nfs "$EXP2" "$NFS3" && mount_nfs "$OTHER" "$NFS4"; then
	pass mounts-uuid-fsid
	fact client-handle-root-uuid-exp "$(save "$NFS3" root_uuid)"
	fact client-handle-root-uuid-other "$(save "$NFS4" root_uuid_other)"
	fact st-dev-uuid-mounts "$(stat -c %d "$NFS3") $(stat -c %d "$NFS4")"
	u_exp=$(hex_of root_uuid)
	u_other=$(hex_of root_uuid_other)
	if [ -n "$u_exp" ] && [ "$u_exp" != "$u_other" ]; then
		pass uuid-fsid-root-handles-differ
	else
		fail uuid-fsid-root-handles-differ "'$u_exp' and '$u_other'"
	fi
	fact client-handle-a-uuid "$(save "$NFS3/a.txt" a_uuid)"
	case "$(hex_of a_uuid)" in
	*"$server_hex"*) pass uuid-fsid-handle-embeds-server-ino-and-generation ;;
	*) fail uuid-fsid-handle-embeds-server-ino-and-generation "$server_hex not in $(hex_of a_uuid)" ;;
	esac
else
	fail mounts-uuid-fsid "$(cat /tmp/mount.err)"
fi

exit "$FAILED"
