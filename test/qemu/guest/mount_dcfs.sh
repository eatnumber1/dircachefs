#!/bin/sh
# dcfs step 15.1 acceptance test: the mount.dcfs wrapper (phase 15).
#
# mount(8) runs `mount.dcfs SOURCE MOUNTPOINT [-sfnv] [-N ns] -o OPTIONS` for
# a `dcfs` mount. This test runs the wrapper as mount(8) does, on the ext4
# image vdb, and checks: the capture of the backing filesystem (dcfs.fstype
# absent or a native type: the tree is served, the FUSE mount's source is
# the spec as written, no backing mount is left in the caller's namespace,
# umount stops dcfs, a SIGKILLed dcfs releases the superblock); the option
# split (ro is the underlying mount's, dcfs.ro dcfs's, remount changes only
# the dcfs mount, a bogus dcfs. option or a failing native mount mounts
# nothing and says why); daemonization (the wrapper returns when dcfs
# answers FUSE_INIT, a failure after the fork is the wrapper's exit status
# and message, a daemonized dcfs has no stdio and logs to syslog,
# dcfs.foreground stays in the foreground); the bind and none forms; a file
# mounted below the source refused; a non-root caller refused with the
# reason.
#
# Not here (later steps of phase 15): the instance identity and cache path
# (15.3), stubs for submounts (15.4), fsck.dcfs and exports (15.5), systemd
# and fstab with util-linux's mount (15.6: this guest's busybox mount does
# not do everything mount(8) does), xfs, btrfs and NFS fixtures.
#
# Run as /tests/mount_dcfs.sh by guest/init; one "TEST ... PASS/FAIL/SKIP"
# line per check.
FAILED=0
. "$(dirname "$0")/lib.sh"

MOUNT_DCFS=/sbin/mount.dcfs
DEV=/dev/vdb
SRC=/src
MNT=/mnt
CACHE=/cache
OUT=/tmp/out

require_commands blkid logread pidof setsid syslogd
[ -x "$MOUNT_DCFS" ] || fail wrapper-installed "$MOUNT_DCFS is not an executable"

cleanup() {
	rc=$?
	if [ "$rc" -ne 0 ] || [ "$FAILED" -ne 0 ]; then
		echo "--- last wrapper output ---"
		cat "$OUT" 2>/dev/null
		echo "--- syslog ---"
		logread 2>/dev/null | tail -n 40
	fi
	for pid in $(pidof mount.dcfs 2>/dev/null); do
		kill -KILL "$pid" 2>/dev/null || true
	done
	umount -l "$MNT" 2>/dev/null || true
	umount -l "$SRC" 2>/dev/null || true
	umount -l "$SRC/file_0.txt" 2>/dev/null || true
}
trap cleanup EXIT

# --- helpers ---------------------------------------------------------------

# The number of mountinfo lines whose mount point is exactly $1.
mount_count() {
	awk -v mp="$1" '$5 == mp { n++ } END { print n + 0 }' /proc/self/mountinfo
}

# The filesystem type (field after "-") and the mount source of the topmost
# mount at $1, as "TYPE SOURCE".
mount_type_source() {
	awk -v mp="$1" '$5 == mp { for (i = 6; i <= NF; i++) if ($i == "-") { t = $(i + 1); s = $(i + 2) } }
		END { print t, s }' /proc/self/mountinfo
}

# The per-mount options (field 6) of the topmost mount at $1.
mount_options_of() {
	awk -v mp="$1" '$5 == mp { o = $6 } END { print o }' /proc/self/mountinfo
}

# How many mountinfo lines name $1 as their mount source.
source_count() {
	awk -v dev="$1" '{ for (i = 6; i <= NF; i++) if ($i == "-") { if ($(i + 2) == dev) n++ } }
		END { print n + 0 }' /proc/self/mountinfo
}

# wrapper ARGS...: runs mount.dcfs (the way mount(8) does: SOURCE MOUNTPOINT
# -o OPTIONS), output in $OUT, status in WRC; bounded, since a wrapper that
# hangs would hold the guest until its timeout.
wrapper() {
	timeout 60 "$MOUNT_DCFS" "$@" >"$OUT" 2>&1
	WRC=$?
}

# Whether the last wrapper run was a refusal: it ran (127 is the shell's not
# found, which would make every refusal check pass without a wrapper) and
# failed.
refused() { [ "$WRC" -ne 0 ] && [ "$WRC" -ne 127 ]; }

# The pids of the running dcfs daemons.
daemons() { pidof mount.dcfs 2>/dev/null; }

# wait_gone PID: up to 10 s for the process to exit.
wait_gone() {
	i=0
	while [ "$i" -lt 100 ] && [ -d "/proc/$1" ]; do
		sleep 0.1
		i=$((i + 1))
	done
	[ ! -d "/proc/$1" ]
}

# unmount_check NAME DIR: umount DIR, then the daemon (the pid in DPID) is gone.
unmount_check() {
	umount "$2" 2>"$OUT"
	urc=$?
	if [ "$urc" -eq 0 ] && wait_gone "$DPID"; then
		pass "$1"
	else
		fail "$1" "umount rc=$urc, dcfs $DPID still running: $(cat "$OUT")"
		kill -KILL "$DPID" 2>/dev/null || true
		umount -l "$2" 2>/dev/null || true
	fi
}

# only_one_daemon: sets DPID to the pid of the single running daemon, or "".
only_one_daemon() {
	set -- $(daemons)
	if [ "$#" -eq 1 ]; then DPID=$1; else DPID=""; fi
}

# Nothing of dcfs left: no mount at $1, no daemon.
nothing_left() {
	[ "$(mount_count "$1")" -eq 0 ] && [ -z "$(daemons)" ]
}

# --- the tree on vdb -------------------------------------------------------

mkdir -p "$CACHE" "$MNT" "$SRC" /tmp/other
mount "$DEV" "$SRC"
mkdir -p "$SRC/d1/d2"
i=0
while [ "$i" -lt 5 ]; do
	echo "content $i" >"$SRC/file_$i.txt"
	i=$((i + 1))
done
echo deep >"$SRC/d1/d2/deep.txt"
umount "$SRC"

syslogd -C256
sleep 0.5 # the socket /dev/log appears

# --- a non-root caller is refused with the reason (decision 12) ---------------

testutil runas 1000 1000 - -- "$MOUNT_DCFS" \
	-o "dcfs.fstype=none,dcfs.cache_db=$CACHE/nonroot.db" "$SRC" "$MNT" >"$OUT" 2>&1
rc=$?
if [ "$rc" -ne 0 ] && [ "$rc" -ne 127 ] && grep -q 'root' "$OUT" && grep -q 'CAP_SYS_ADMIN' "$OUT" &&
	nothing_left "$MNT"; then
	pass non-root-refused
else
	fail non-root-refused "rc=$rc out=$(cat "$OUT")"
fi

# --- capture: dcfs.fstype=ext4 -----------------------------------------------

BEFORE_SRC=$(source_count "$DEV")
wrapper -o "dcfs.fstype=ext4,dcfs.cache_db=$CACHE/ext4.db" "$DEV" "$MNT"
only_one_daemon
if [ "$WRC" -eq 0 ] && [ -n "$DPID" ]; then
	pass capture-ext4-mount
else
	fail capture-ext4-mount "rc=$WRC daemons=$(daemons) out=$(cat "$OUT")"
fi
# The wrapper returned when dcfs was serving: this stat does not wait.
if [ "$(cat "$MNT/file_3.txt" 2>&1)" = "content 3" ] &&
	[ "$(cat "$MNT/d1/d2/deep.txt" 2>&1)" = "deep" ]; then
	pass capture-ext4-served
else
	fail capture-ext4-served "$(ls -l "$MNT" 2>&1)"
fi
set -- $(mount_type_source "$MNT")
if [ "$1" = "fuse.dcfs" ] && [ "$2" = "$DEV" ]; then
	pass capture-ext4-mountinfo-source
else
	fail capture-ext4-mountinfo-source "type=$1 source=$2, want fuse.dcfs $DEV"
fi
# The only mount that names the device is the FUSE mount (as its source):
# the backing filesystem was mounted in dcfs's own namespace, nowhere else.
if [ "$(source_count "$DEV")" -eq $((BEFORE_SRC + 1)) ] && [ "$(mount_count "$SRC")" -eq 0 ] &&
	! grep -q ' ext4 ' /proc/self/mountinfo; then
	pass capture-ext4-no-backing-mount
else
	fail capture-ext4-no-backing-mount "$(grep -e "$DEV" -e ' ext4 ' /proc/self/mountinfo)"
fi
unmount_check capture-ext4-umount-stops-dcfs "$MNT"
if nothing_left "$MNT"; then
	pass capture-ext4-umount-leaves-nothing
else
	fail capture-ext4-umount-leaves-nothing "$(grep "$MNT" /proc/self/mountinfo) $(daemons)"
fi

# The same with dcfs.fstype absent: mount(8) finds the type.
wrapper -o "dcfs.cache_db=$CACHE/auto.db" "$DEV" "$MNT"
only_one_daemon
if [ "$WRC" -eq 0 ] && [ "$(cat "$MNT/file_1.txt" 2>&1)" = "content 1" ]; then
	pass capture-autodetect
else
	fail capture-autodetect "rc=$WRC out=$(cat "$OUT")"
fi
if [ -n "$DPID" ]; then
	unmount_check capture-autodetect-umount "$MNT"
fi

# UUID= sources are resolved by the native mount (busybox's, here).
UUID=$(blkid "$DEV" 2>/dev/null | sed -n 's/.*[ :]UUID="\([^"]*\)".*/\1/p')
if [ -z "$UUID" ]; then
	skip capture-uuid-source "blkid prints no UUID for $DEV"
else
	wrapper -o "dcfs.cache_db=$CACHE/uuid.db" "UUID=$UUID" "$MNT"
	only_one_daemon
	set -- $(mount_type_source "$MNT")
	if [ "$WRC" -eq 0 ] && [ "$(cat "$MNT/file_2.txt" 2>&1)" = "content 2" ] &&
		[ "$2" = "UUID=$UUID" ]; then
		pass capture-uuid-source
	else
		fail capture-uuid-source "rc=$WRC source=$2 out=$(cat "$OUT")"
	fi
	[ -n "$DPID" ] && unmount_check capture-uuid-umount "$MNT"
fi

# A SIGKILLed dcfs: the lazy unmount finds the FUSE connection gone, and the
# backing superblock goes with dcfs's descriptors (the kernel's ext4 sysfs
# directory for it disappears).
wrapper -o "dcfs.fstype=ext4,dcfs.cache_db=$CACHE/kill.db" "$DEV" "$MNT"
only_one_daemon
if [ -n "$DPID" ] && [ -d /sys/fs/ext4 ]; then
	[ -d /sys/fs/ext4/vdb ] || fail capture-sigkill-superblock-present "no /sys/fs/ext4/vdb while mounted"
	kill -KILL "$DPID"
	wait_gone "$DPID"
	umount -l "$MNT" 2>/dev/null
	i=0
	while [ "$i" -lt 100 ] && [ -d /sys/fs/ext4/vdb ]; do
		sleep 0.1
		i=$((i + 1))
	done
	if [ ! -d /sys/fs/ext4/vdb ] && [ "$(mount_count "$MNT")" -eq 0 ]; then
		pass capture-sigkill-releases-superblock
	else
		fail capture-sigkill-releases-superblock "ext4 vdb still held: $(grep "$DEV" /proc/self/mountinfo)"
	fi
else
	fail capture-sigkill-releases-superblock "rc=$WRC no daemon or no /sys/fs/ext4: $(cat "$OUT")"
fi

# --- options -----------------------------------------------------------------

# ro is the underlying mount's: writes through dcfs get EROFS, the dcfs mount
# itself is read-write.
wrapper -o "ro,dcfs.fstype=ext4,dcfs.cache_db=$CACHE/ro.db" "$DEV" "$MNT"
only_one_daemon
if [ "$WRC" -eq 0 ]; then
	touch "$MNT/new_file" 2>"$OUT"
	if grep -q 'Read-only file system' "$OUT"; then
		pass option-ro-backing-erofs
	else
		fail option-ro-backing-erofs "touch said: $(cat "$OUT")"
	fi
	case ",$(mount_options_of "$MNT")," in
	*,rw,*) pass option-ro-dcfs-mount-rw ;;
	*) fail option-ro-dcfs-mount-rw "mount options $(mount_options_of "$MNT")" ;;
	esac
	[ "$(cat "$MNT/file_0.txt")" = "content 0" ] && pass option-ro-reads || fail option-ro-reads "read failed"
	[ -n "$DPID" ] && unmount_check option-ro-umount "$MNT"
else
	fail option-ro-backing-erofs "rc=$WRC out=$(cat "$OUT")"
fi

# dcfs.ro is dcfs's: the dcfs mount is read-only, the backing is not.
wrapper -o "dcfs.ro,dcfs.fstype=ext4,dcfs.cache_db=$CACHE/dro.db" "$DEV" "$MNT"
only_one_daemon
if [ "$WRC" -eq 0 ]; then
	touch "$MNT/new_file" 2>"$OUT"
	if grep -q 'Read-only file system' "$OUT"; then
		pass option-dcfs-ro-erofs
	else
		fail option-dcfs-ro-erofs "touch said: $(cat "$OUT")"
	fi
	case ",$(mount_options_of "$MNT")," in
	*,ro,*) pass option-dcfs-ro-mount-ro ;;
	*) fail option-dcfs-ro-mount-ro "mount options $(mount_options_of "$MNT")" ;;
	esac
	[ -n "$DPID" ] && unmount_check option-dcfs-ro-umount "$MNT"
else
	fail option-dcfs-ro-erofs "rc=$WRC out=$(cat "$OUT")"
fi

# A dcfs. option dcfs does not know: refused, named, nothing mounted.
wrapper -o "dcfs.bogus,dcfs.fstype=ext4,dcfs.cache_db=$CACHE/bogus.db" "$DEV" "$MNT"
if refused && grep -q 'dcfs.bogus' "$OUT" && nothing_left "$MNT"; then
	pass option-unknown-dcfs-refused
else
	fail option-unknown-dcfs-refused "rc=$WRC out=$(cat "$OUT") left=$(daemons)"
fi

# A native mount that fails: its error text and non-zero status, nothing
# mounted (a wrong type; a missing device).
wrapper -o "dcfs.fstype=nosuchfs,dcfs.cache_db=$CACHE/wrongtype.db" "$DEV" "$MNT"
if refused && grep -qi 'no such device' "$OUT" && nothing_left "$MNT"; then
	pass native-failure-wrong-type
else
	fail native-failure-wrong-type "rc=$WRC out=$(cat "$OUT") left=$(daemons)"
fi
wrapper -o "dcfs.fstype=ext4,dcfs.cache_db=$CACHE/nodev.db" /dev/nonexistent "$MNT"
if refused && grep -qi 'no such file' "$OUT" && nothing_left "$MNT"; then
	pass native-failure-missing-device
else
	fail native-failure-missing-device "rc=$WRC out=$(cat "$OUT") left=$(daemons)"
fi

# -f mounts nothing.
wrapper -f -o "dcfs.fstype=ext4,dcfs.cache_db=$CACHE/fake.db" "$DEV" "$MNT"
if [ "$WRC" -eq 0 ] && nothing_left "$MNT"; then
	pass fake-mounts-nothing
else
	fail fake-mounts-nothing "rc=$WRC out=$(cat "$OUT") left=$(daemons)"
fi

# A mount point that is a file is refused, naming it.
echo x >/tmp/regular-file
wrapper -o "dcfs.fstype=ext4,dcfs.cache_db=$CACHE/fileMP.db" "$DEV" /tmp/regular-file
if refused && grep -q '/tmp/regular-file' "$OUT" && nothing_left /tmp/regular-file; then
	pass mountpoint-file-refused
else
	fail mountpoint-file-refused "rc=$WRC out=$(cat "$OUT") left=$(daemons)"
fi

# --- mount -t dcfs: through mount(8) -------------------------------------------

# This guest's busybox mount runs no mount.<type> helpers (it asks the kernel
# for a filesystem named dcfs and gets ENODEV); util-linux's does, and step
# 15.6 runs this through it in the Debian guest.
mount -t dcfs -o "dcfs.fstype=ext4,dcfs.cache_db=$CACHE/mount_t.db" "$DEV" "$MNT" 2>"$OUT"
rc=$?
only_one_daemon
if [ "$rc" -eq 0 ] && [ -n "$DPID" ] && [ "$(cat "$MNT/file_4.txt" 2>&1)" = "content 4" ]; then
	pass mount-t-dcfs
	unmount_check mount-t-dcfs-umount "$MNT"
elif [ "$rc" -ne 0 ] && grep -q 'No such device' "$OUT" && nothing_left "$MNT"; then
	skip mount-t-dcfs "busybox mount has no mount.<type> helper support (step 15.6 uses util-linux's)"
else
	fail mount-t-dcfs "rc=$rc out=$(cat "$OUT") mounts=$(grep "$MNT" /proc/self/mountinfo)"
fi

# --- remount changes only the dcfs mount (decision 10) -------------------------

# The backing is vdb mounted by us at /src (the none form: the administrator
# owns it), dcfs on /mnt.
mount "$DEV" "$SRC"
wrapper -o "dcfs.fstype=none,dcfs.cache_db=$CACHE/remount.db" "$SRC" "$MNT"
only_one_daemon
if [ "$WRC" -eq 0 ]; then
	wrapper -o remount,dcfs.ro "$SRC" "$MNT"
	rc_ro=$WRC
	touch "$MNT/rm_file" 2>"$OUT"
	grep -q 'Read-only file system' "$OUT" && dcfs_ro=1 || dcfs_ro=0
	if touch "$SRC/rm_backing" 2>/dev/null; then backing_rw=1; else backing_rw=0; fi
	if [ "$rc_ro" -eq 0 ] && [ "$dcfs_ro" -eq 1 ] && [ "$backing_rw" -eq 1 ]; then
		pass remount-dcfs-ro-only-dcfs
	else
		fail remount-dcfs-ro-only-dcfs "rc=$rc_ro dcfs_ro=$dcfs_ro backing_rw=$backing_rw"
	fi
	wrapper -o remount "$SRC" "$MNT"
	if [ "$WRC" -eq 0 ] && touch "$MNT/rm_file" 2>"$OUT"; then
		pass remount-back-to-rw
	else
		fail remount-back-to-rw "rc=$WRC touch: $(cat "$OUT")"
	fi
	# A remount of something that is not a dcfs mount is refused, and leaves
	# that mount alone.
	wrapper -o remount,dcfs.ro "$DEV" "$SRC"
	if refused && touch "$SRC/rm_backing2" 2>/dev/null; then
		pass remount-refuses-non-dcfs
	else
		fail remount-refuses-non-dcfs "rc=$WRC out=$(cat "$OUT")"
	fi
	[ -n "$DPID" ] && unmount_check remount-umount "$MNT"
else
	fail remount-dcfs-ro-only-dcfs "mount failed rc=$WRC out=$(cat "$OUT")"
fi
umount "$SRC" 2>/dev/null

# --- daemonization -------------------------------------------------------------

wrapper -o "dcfs.fstype=ext4,dcfs.cache_db=$CACHE/daemon.db" "$DEV" "$MNT"
only_one_daemon
if [ -n "$DPID" ]; then
	fds=$(ls -l "/proc/$DPID/fd/0" "/proc/$DPID/fd/1" "/proc/$DPID/fd/2" 2>&1 | awk '{ print $NF }' | tr '\n' ' ')
	if [ "$fds" = "/dev/null /dev/null /dev/null " ]; then
		pass daemon-stdio-is-dev-null
	else
		fail daemon-stdio-is-dev-null "fds: $fds"
	fi
	[ "$(readlink "/proc/$DPID/cwd")" = "/" ] && pass daemon-cwd-is-root || fail daemon-cwd-is-root "cwd $(readlink "/proc/$DPID/cwd")"
	sid=$(awk '{ print $6 }' "/proc/$DPID/stat")
	[ "$sid" = "$DPID" ] && pass daemon-leads-its-session || fail daemon-leads-its-session "session $sid, pid $DPID"
	# Its log went to syslog, none of it to the (closed) stderr.
	if logread | grep -q 'starting'; then
		pass daemon-logs-to-syslog
	else
		fail daemon-logs-to-syslog "no start line in: $(logread | tail -n 5)"
	fi
	unmount_check daemon-umount "$MNT"
else
	fail daemon-stdio-is-dev-null "no daemon: rc=$WRC out=$(cat "$OUT")"
fi

# A failure after the fork (the cache database's writer lock is held): the
# wrapper reports it with a non-zero status and message, nothing mounted.
: >"$CACHE/locked.db"
testutil sqlite-lock "$CACHE/locked.db" 25 >/tmp/lock.out 2>&1 &
LOCKER=$!
i=0
while [ "$i" -lt 50 ] && ! grep -q READY /tmp/lock.out; do
	sleep 0.1
	i=$((i + 1))
done
wrapper -o "dcfs.fstype=ext4,dcfs.cache_db=$CACHE/locked.db" "$DEV" "$MNT"
if refused && [ -s "$OUT" ] && nothing_left "$MNT"; then
	pass daemon-late-failure-reported
else
	fail daemon-late-failure-reported "rc=$WRC out=$(cat "$OUT") left=$(daemons)"
fi
kill "$LOCKER" 2>/dev/null
wait "$LOCKER" 2>/dev/null

# dcfs.foreground: no fork, the process stays, the log reaches stderr as well
# as syslog.
"$MOUNT_DCFS" -o "dcfs.fstype=ext4,dcfs.foreground,dcfs.stderrthreshold=0,dcfs.cache_db=$CACHE/fg.db" \
	"$DEV" "$MNT" >"$OUT" 2>&1 &
FG=$!
i=0
while [ "$i" -lt 100 ] && [ "$(mount_count "$MNT")" -eq 0 ] && kill -0 "$FG" 2>/dev/null; do
	sleep 0.1
	i=$((i + 1))
done
if [ "$(mount_count "$MNT")" -eq 1 ] && kill -0 "$FG" 2>/dev/null; then
	pass foreground-stays
	if grep -q 'starting' "$OUT"; then
		pass foreground-logs-to-stderr
	else
		fail foreground-logs-to-stderr "stderr: $(cat "$OUT")"
	fi
	kill -TERM "$FG"
	wait "$FG"
	rc=$?
	if [ "$rc" -eq 0 ] && [ "$(mount_count "$MNT")" -eq 0 ]; then
		pass foreground-sigterm-clean
	else
		fail foreground-sigterm-clean "rc=$rc mounts=$(grep "$MNT" /proc/self/mountinfo)"
	fi
else
	fail foreground-stays "no mount or exited: $(cat "$OUT")"
	kill -KILL "$FG" 2>/dev/null
fi

# --- the bind form: SOURCE captured with a non-recursive bind ------------------

mount "$DEV" "$SRC"
wrapper -o "dcfs.fstype=bind,dcfs.cache_db=$CACHE/bind.db" "$SRC" "$MNT"
only_one_daemon
set -- $(mount_type_source "$MNT")
if [ "$WRC" -eq 0 ] && [ "$(cat "$MNT/file_0.txt" 2>&1)" = "content 0" ] &&
	[ "$1" = "fuse.dcfs" ] && [ "$2" = "$SRC" ]; then
	pass bind-to-another-path
else
	fail bind-to-another-path "rc=$WRC type=$1 source=$2 out=$(cat "$OUT")"
fi
# No backing mount of its own: $SRC has just its one mount.
[ -n "$DPID" ] && [ "$(mount_count "$SRC")" -eq 1 ] && pass bind-no-extra-mount || fail bind-no-extra-mount "$(grep "$SRC" /proc/self/mountinfo)"
[ -n "$DPID" ] && unmount_check bind-umount "$MNT"

# Over the same path (over-mount): the original is under dcfs, and comes back.
wrapper -o "dcfs.fstype=bind,dcfs.cache_db=$CACHE/bindover.db" "$SRC" "$SRC"
only_one_daemon
if [ "$WRC" -eq 0 ] && [ "$(mount_count "$SRC")" -eq 2 ] &&
	[ "$(cat "$SRC/file_0.txt" 2>&1)" = "content 0" ]; then
	pass bind-over-mount
else
	fail bind-over-mount "rc=$WRC mounts=$(grep "$SRC" /proc/self/mountinfo) out=$(cat "$OUT")"
fi
if [ -n "$DPID" ]; then
	unmount_check bind-over-mount-umount "$SRC"
	[ "$(mount_count "$SRC")" -eq 1 ] && pass bind-over-mount-original-back || fail bind-over-mount-original-back "$(grep "$SRC" /proc/self/mountinfo)"
fi

# A file mounted below SOURCE: the mount fails, naming it (stubs for
# directory submounts are step 15.4's).
mount -o bind "$SRC/file_1.txt" "$SRC/file_0.txt"
wrapper -o "dcfs.fstype=bind,dcfs.cache_db=$CACHE/bindfile.db" "$SRC" "$MNT"
if refused && grep -q "$SRC/file_0.txt" "$OUT" && nothing_left "$MNT"; then
	pass bind-file-below-refused
else
	fail bind-file-below-refused "rc=$WRC out=$(cat "$OUT") left=$(daemons)"
fi
wrapper -o "dcfs.fstype=none,dcfs.cache_db=$CACHE/nonefile.db" "$SRC" "$MNT"
if refused && grep -q "$SRC/file_0.txt" "$OUT" && nothing_left "$MNT"; then
	pass none-file-below-refused
else
	fail none-file-below-refused "rc=$WRC out=$(cat "$OUT") left=$(daemons)"
fi
umount "$SRC/file_0.txt"

# --- the none form: the live tree, no capture ----------------------------------

wrapper -o "dcfs.fstype=none,dcfs.cache_db=$CACHE/none.db" "$SRC" "$MNT"
only_one_daemon
set -- $(mount_type_source "$MNT")
if [ "$WRC" -eq 0 ] && [ "$(cat "$MNT/file_0.txt" 2>&1)" = "content 0" ] &&
	[ "$1" = "fuse.dcfs" ] && [ "$2" = "$SRC" ]; then
	pass none-to-another-path
else
	fail none-to-another-path "rc=$WRC type=$1 source=$2 out=$(cat "$OUT")"
fi
# The administrator's mount is remounted with its own options, and dcfs sees
# the filesystem go read-only: writes through dcfs fail with EROFS.
mount -o remount,ro "$SRC" 2>"$OUT"
touch "$MNT/after_ro" 2>>"$OUT"
if grep -q 'Read-only file system' "$OUT"; then
	pass none-remount-ro-gives-erofs
else
	fail none-remount-ro-gives-erofs "$(cat "$OUT")"
fi
mount -o remount,rw "$SRC" 2>/dev/null
[ -n "$DPID" ] && unmount_check none-umount "$MNT"

wrapper -o "dcfs.fstype=none,dcfs.cache_db=$CACHE/noneover.db" "$SRC" "$SRC"
only_one_daemon
if [ "$WRC" -eq 0 ] && [ "$(mount_count "$SRC")" -eq 2 ] && [ "$(cat "$SRC/file_2.txt" 2>&1)" = "content 2" ]; then
	pass none-over-source
else
	fail none-over-source "rc=$WRC mounts=$(grep "$SRC" /proc/self/mountinfo) out=$(cat "$OUT")"
fi
[ -n "$DPID" ] && unmount_check none-over-source-umount "$SRC"

# A source that is not there or not a directory is refused naming it.
wrapper -o "dcfs.fstype=none,dcfs.cache_db=$CACHE/missing.db" /nonexistent "$MNT"
if refused && grep -q '/nonexistent' "$OUT" && nothing_left "$MNT"; then
	pass none-missing-source-refused
else
	fail none-missing-source-refused "rc=$WRC out=$(cat "$OUT")"
fi
umount "$SRC" 2>/dev/null

require_no_reclaim no-reclaim
exit "$FAILED"
