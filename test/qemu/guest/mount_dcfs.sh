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
# dcfs.foreground stays in the foreground); the bind form; a file
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
. "$(dirname "$0")/fault_lib.sh"

MOUNT_DCFS=/sbin/mount.dcfs
DEV=/dev/vdb
SRC=/src
MNT=/mnt
CACHE=/cache
OUT=/tmp/out

require_commands blkid flock logread pidof setsid syslogd
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
	nl_i=0
	while [ "$nl_i" -lt 50 ] && [ -n "$(daemons)" ]; do
		sleep 0.1
		nl_i=$((nl_i + 1))
	done
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
	-o "dcfs.fstype=bind,dcfs.cache_db=$CACHE/nonroot.db" "$SRC" "$MNT" >"$OUT" 2>&1
rc=$?
if [ "$rc" -eq 1 ] && grep -q 'root' "$OUT" && grep -q 'CAP_SYS_ADMIN' "$OUT" &&
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
wrapper "$DEV" "$MNT" -n -o "dcfs.cache_db=$CACHE/auto.db"
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
if [ "$WRC" -eq 1 ] && grep -q 'dcfs.bogus' "$OUT" && nothing_left "$MNT"; then
	pass option-unknown-dcfs-refused
else
	fail option-unknown-dcfs-refused "rc=$WRC out=$(cat "$OUT") left=$(daemons)"
fi

# A native mount that fails: its error text and non-zero status, nothing
# mounted (a wrong type; a missing device).
mkdir -p /tmp/native
mount -t nosuchfs "$DEV" /tmp/native 2>/dev/null
NATIVE_RC=$?
wrapper -o "dcfs.fstype=nosuchfs,dcfs.cache_db=$CACHE/wrongtype.db" "$DEV" "$MNT"
if refused && [ "$WRC" -eq "$NATIVE_RC" ] && grep -qi 'no such device' "$OUT" && nothing_left "$MNT"; then
	pass native-failure-wrong-type
else
	fail native-failure-wrong-type "rc=$WRC out=$(cat "$OUT") left=$(daemons)"
fi
mount -t ext4 /dev/nonexistent /tmp/native 2>/dev/null
NATIVE_RC=$?
wrapper -o "dcfs.fstype=ext4,dcfs.cache_db=$CACHE/nodev.db" /dev/nonexistent "$MNT"
if refused && [ "$WRC" -eq "$NATIVE_RC" ] && grep -qi 'no such file' "$OUT" && nothing_left "$MNT"; then
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

# The backing is vdb mounted by us at /src (the bind form: the administrator
# owns it), dcfs on /mnt.
mount "$DEV" "$SRC"
wrapper -o "dcfs.fstype=bind,dcfs.cache_db=$CACHE/remount.db" "$SRC" "$MNT"
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

wrapper -o "dcfs.fstype=ext4,dcfs.stderrthreshold=0,dcfs.cache_db=$CACHE/daemon.db" "$DEV" "$MNT"
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
	if logread | grep -q "dcfs\[$DPID\].*starting" && [ ! -s "$OUT" ]; then
		pass daemon-logs-to-syslog
	else
		fail daemon-logs-to-syslog "no start line of pid $DPID in syslog, or output: $(cat "$OUT"); $(logread | tail -n 5)"
	fi
	unmount_check daemon-umount "$MNT"
	# At the default threshold (WARNING) the INFO narrative stays out of
	# syslog: the one knob, dcfs.stderrthreshold, governs it.
	wrapper -o "dcfs.fstype=ext4,dcfs.cache_db=$CACHE/quiet.db" "$DEV" "$MNT"
	only_one_daemon
	if [ -n "$DPID" ]; then
		if logread | grep -q "dcfs\[$DPID\].*starting"; then
			fail daemon-syslog-follows-threshold "INFO lines of pid $DPID reached syslog: $(logread | grep "dcfs\[$DPID\]")"
		else
			pass daemon-syslog-follows-threshold
		fi
		unmount_check daemon-quiet-umount "$MNT"
	else
		fail daemon-syslog-follows-threshold "no daemon: rc=$WRC out=$(cat "$OUT")"
	fi
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
if [ "$WRC" -eq 32 ] && grep -qi 'locked' "$OUT" && nothing_left "$MNT"; then
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
	if logread | grep -q "dcfs\[$FG\]"; then
		fail foreground-no-syslog "a foreground dcfs logged to syslog: $(logread | grep "dcfs\[$FG\]")"
	else
		pass foreground-no-syslog
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

mount "$DEV" "$SRC"

# A file mounted below SOURCE: the mount fails, naming it (stubs for
# directory submounts are step 15.4's).
mount -o bind "$SRC/file_1.txt" "$SRC/file_0.txt"
wrapper -o "dcfs.fstype=bind,dcfs.cache_db=$CACHE/nonefile.db" "$SRC" "$MNT"
if refused && grep -q "$SRC/file_0.txt" "$OUT" && nothing_left "$MNT"; then
	pass bind-file-below-refused
else
	fail bind-file-below-refused "rc=$WRC out=$(cat "$OUT") left=$(daemons)"
fi
umount "$SRC/file_0.txt"

# --- the bind form: SOURCE, a directory, opened in place (step 15.9: was none) --

wrapper -o "dcfs.fstype=bind,dcfs.cache_db=$CACHE/none.db" "$SRC" "$MNT"
only_one_daemon
set -- $(mount_type_source "$MNT")
if [ "$WRC" -eq 0 ] && [ "$(cat "$MNT/file_0.txt" 2>&1)" = "content 0" ] &&
	[ "$1" = "fuse.dcfs" ] && [ "$2" = "$SRC" ]; then
	pass bind-to-another-path
else
	fail bind-to-another-path "rc=$WRC type=$1 source=$2 out=$(cat "$OUT")"
fi
# No backing mount of its own (no clone: the real mount is SOURCE's, in place):
# $SRC has just its one mount, which dcfs keeps busy.
[ -n "$DPID" ] && [ "$(mount_count "$SRC")" -eq 1 ] && pass bind-no-extra-mount || fail bind-no-extra-mount "$(grep "$SRC" /proc/self/mountinfo)"
# (By device: busybox's umount takes /src for the FUSE mount's source, which
# is also spelled /src, and unmounts that.)
umount "$DEV" 2>"$OUT.busy"
if [ "$(mount_count "$SRC")" -eq 1 ] && grep -qi 'busy' "$OUT.busy"; then
	pass bind-keeps-the-real-mount-busy
else
	fail bind-keeps-the-real-mount-busy "$(cat "$OUT.busy") mounts=$(mount_count "$SRC")"
fi
# The administrator's mount is remounted with its own options, and dcfs sees
# the filesystem go read-only: writes through dcfs fail with EROFS.
mount -o remount,ro "$SRC" 2>"$OUT"
touch "$MNT/after_ro" 2>>"$OUT"
if grep -q 'Read-only file system' "$OUT"; then
	pass bind-remount-ro-gives-erofs
else
	fail bind-remount-ro-gives-erofs "$(cat "$OUT")"
fi
mount -o remount,rw "$SRC" 2>/dev/null
[ -n "$DPID" ] && unmount_check bind-umount "$MNT"

wrapper -o "dcfs.fstype=bind,dcfs.cache_db=$CACHE/noneover.db" "$SRC" "$SRC"
only_one_daemon
if [ "$WRC" -eq 0 ] && [ "$(mount_count "$SRC")" -eq 2 ] && [ "$(cat "$SRC/file_2.txt" 2>&1)" = "content 2" ]; then
	pass bind-over-source
else
	fail bind-over-source "rc=$WRC mounts=$(grep "$SRC" /proc/self/mountinfo) out=$(cat "$OUT")"
fi
[ -n "$DPID" ] && unmount_check bind-over-source-umount "$SRC"

# A source that is not there or not a directory is refused naming it.
wrapper -o "dcfs.fstype=bind,dcfs.cache_db=$CACHE/missing.db" /nonexistent "$MNT"
if refused && grep -q '/nonexistent' "$OUT" && nothing_left "$MNT"; then
	pass bind-missing-source-refused
else
	fail bind-missing-source-refused "rc=$WRC out=$(cat "$OUT")"
fi
umount "$SRC" 2>/dev/null

# --- relative paths: the daemon's chdir("/") must not change their meaning ---

mount "$DEV" "$SRC"
mkdir -p /tmp/rel_mnt
(cd /tmp && "$MOUNT_DCFS" -o "dcfs.fstype=bind,dcfs.cache_db=rel.db" ../src rel_mnt) >"$OUT" 2>&1
WRC=$?
only_one_daemon
if [ "$WRC" -eq 0 ] && [ "$(cat /tmp/rel_mnt/file_0.txt 2>&1)" = "content 0" ] && [ -f /tmp/rel.db ]; then
	pass relative-paths-in-daemon-mode
else
	fail relative-paths-in-daemon-mode "rc=$WRC out=$(cat "$OUT") db=$(ls /tmp/rel.db /rel.db 2>&1)"
fi
[ -n "$DPID" ] && unmount_check relative-paths-umount /tmp/rel_mnt
umount "$SRC" 2>/dev/null

# --- native options need a native mount (decision 6) ----------------------------

mount "$DEV" "$SRC"
wrapper -o "ro,dcfs.fstype=bind,dcfs.cache_db=$CACHE/noneopts.db" "$SRC" "$MNT"
if [ "$WRC" -eq 1 ] && grep -q 'ro' "$OUT" && grep -q 'dcfs.fstype=bind' "$OUT" && nothing_left "$MNT"; then
	pass bind-refuses-native-options
else
	fail bind-refuses-native-options "rc=$WRC out=$(cat "$OUT") left=$(daemons)"
fi
# The directory form was called none (step 15.9 renamed it, russ): the old
# name is an error that says so, not a native type, and nothing is mounted.
wrapper -o "dcfs.fstype=none,dcfs.cache_db=$CACHE/oldname.db" "$SRC" "$MNT"
if [ "$WRC" -eq 1 ] && grep -q 'renamed dcfs.fstype=bind' "$OUT" && nothing_left "$MNT"; then
	pass none-is-refused-it-is-bind-now
else
	fail none-is-refused-it-is-bind-now "rc=$WRC out=$(cat "$OUT") left=$(daemons)"
fi
# allow_other is always on (step 15.8): naming it, either way, is a usage
# error (exit 1) that says so, and nothing is mounted.
for allow in dcfs.allow_other dcfs.allow_other=0 allow_other; do
	wrapper -o "$allow,dcfs.fstype=bind,dcfs.cache_db=$CACHE/ao.db" "$SRC" "$MNT"
	if [ "$WRC" -eq 1 ] && grep -q 'always allows other users' "$OUT" && grep -q 'remove it' "$OUT" && nothing_left "$MNT"; then
		pass "refuses-$allow"
	else
		fail "refuses-$allow" "rc=$WRC out=$(cat "$OUT") left=$(daemons)"
	fi
done
# A mount does reach other users: a world-readable file reads as nobody.
wrapper -o "dcfs.fstype=bind,dcfs.cache_db=$CACHE/ao2.db" "$SRC" "$MNT"
only_one_daemon
if [ "$WRC" -eq 0 ] && testutil runas 65534 65534 - -- cat "$MNT/file_0.txt" >"$OUT.ao" 2>&1 && [ "$(cat "$OUT.ao")" = "content 0" ]; then
	pass other-users-reach-the-mount
else
	fail other-users-reach-the-mount "rc=$WRC: $(cat "$OUT") $(cat "$OUT.ao" 2>&1)"
fi
[ -n "$DPID" ] && unmount_check other-users-umount "$MNT"
# What libmount adds to a helper's options (rw or ro, fstab's nofail, _netdev,
# ...) is not an error for the bind form.
wrapper -o "rw,nofail,_netdev,noauto,defaults,dcfs.fstype=bind,dcfs.cache_db=$CACHE/rm2.db" "$SRC" "$MNT"
only_one_daemon
if [ "$WRC" -eq 0 ]; then
	pass bind-accepts-libmount-options
else
	fail bind-accepts-libmount-options "rc=$WRC out=$(cat "$OUT")"
fi
# A remount cannot change the underlying mount: a native ro that libmount
# merged in from fstab is ignored, with a warning naming it, and the dcfs
# mount stays as it was.
wrapper -o remount,ro,noatime,nofail "$SRC" "$MNT"
if [ "$WRC" -eq 0 ] && grep -q 'ro, noatime' "$OUT" && touch "$MNT/after_remount_ro" 2>/dev/null; then
	pass remount-ignores-native-options
else
	fail remount-ignores-native-options "rc=$WRC out=$(cat "$OUT")"
fi
[ -n "$DPID" ] && unmount_check remount-native-umount "$MNT"

umount "$SRC" 2>/dev/null

# --- the helper's other names and its usage ------------------------------------

# libmount looks for mount.<type> with the type of the mountinfo line
# (fuse.dcfs) on a remount.
if [ -x /sbin/mount.fuse.dcfs ] && /sbin/mount.fuse.dcfs -V 2>&1 | grep -q '^mount.dcfs (dcfs '; then
	pass mount-fuse-dcfs-name
else
	fail mount-fuse-dcfs-name "$(/sbin/mount.fuse.dcfs -V 2>&1)"
fi
"$MOUNT_DCFS" -x >"$OUT" 2>&1
if [ "$?" -eq 1 ] && grep -q 'usage: mount.dcfs' "$OUT"; then
	pass usage-exits-one
else
	fail usage-exits-one "$(cat "$OUT")"
fi

# --- umount.fuse.dcfs and umount.fuse (step 15.6b) ------------------------------

# The helpers umount(8) runs to unmount a dcfs mount (dcfs/mount_dcfs.h has
# which name when): each runs `umount -i` with the options it was given and,
# for a dcfs mount whose superblock this unmount ended, then waits (no timeout)
# for the daemon to exit, so that "unmounted" means "stopped". The kernel says
# whether the superblock ended in fusectl, which this guest mounts below. This
# guest's umount is busybox's, which has no -i: /bin/umount is made a script
# that drops it. A daemon is made slow to exit with a backing device whose
# writes take SLOW_WRITE_MS once the test says so: a write through dcfs leaves
# dirty pages on the backing filesystem, and the daemon's last act, the syncfs
# of the backing filesystem, then takes seconds after the unmount has returned.
UMOUNT_HELPER=/sbin/umount.fuse.dcfs
rm -f /bin/umount
cat >/bin/umount <<'EOF_UMOUNT'
#!/bin/sh
# What util-linux's `umount -i` is, for busybox's umount.
args=
for a in "$@"; do
	[ "$a" = -i ] || args="$args $a"
done
exec /bin/busybox umount $args
EOF_UMOUNT
chmod +x /bin/umount
SLOW_WRITE_MS=2000
FUSECTL=/sys/fs/fuse/connections
mount -t fusectl fusectl "$FUSECTL"

# slow_writes MS: the writes of the "slow" device take MS milliseconds from now
# on (0: none).
slow_writes() {
	sw_sectors=$(fault_sectors "$DEV")
	"$DMSETUP" suspend --nolockfs --noudevsync slow &&
		"$DMSETUP" load slow --table "0 $sw_sectors delay $DEV 0 0 $DEV 0 $1" &&
		"$DMSETUP" resume --noudevsync slow
}

# uptime_ms: the guest's uptime in milliseconds.
uptime_ms() { awk '{ printf "%d", $1 * 1000 }' /proc/uptime; }

# mount_device DIR: "major:minor" of the topmost mount at DIR.
mount_device() {
	awk -v mp="$1" '$5 == mp { d = $3 } END { print d }' /proc/self/mountinfo
}

# wait_daemon_exit DEVICE: blocks until the daemon of the mount that had
# DEVICE has exited: its lock (dcfs/umount_helper.h) is released when it
# does. For the checks that did not unmount with the helper.
wait_daemon_exit() {
	flock "/run/dcfs/$(echo "$1" | tr : _).lock" true
}

# daemon_finished DEVICE: the daemon of the mount that had DEVICE has done its
# last act, the removal of its lock file (dcfs/umount_helper.h), so what the
# helper waited for is over: the lock itself is released when the process
# closes its files a moment before it becomes a zombie, which a check of the
# process would race with.
daemon_finished() {
	[ ! -e "/run/dcfs/$(echo "$1" | tr : _).lock" ]
}

for helper in umount.fuse.dcfs umount.fuse; do
	if [ ! -x "/sbin/$helper" ]; then
		fail "$helper-installed" "/sbin/$helper is not an executable"
	else
		"/sbin/$helper" -V >"$OUT" 2>&1
		vrc=$?
		if [ "$vrc" -eq 0 ] && grep -q '^umount.fuse.dcfs (dcfs ' "$OUT"; then
			pass "$helper-name"
		else
			fail "$helper-name" "$(cat "$OUT")"
		fi
	fi
done

if ! fault_wrap slow "$DEV"; then
	fail umount-helper-fixture "cannot wrap $DEV in a dm device"
else
	SLOW=$(fault_dev slow)

	# The fixture: the daemon of a plain `umount` is still running when umount
	# returns (it is syncing, with a slow backing disk): the race the helper
	# closes.
	wrapper -o "dcfs.fstype=ext4,dcfs.cache_db=$CACHE/slow1.db" "$SLOW" "$MNT"
	only_one_daemon
	FIXTURE_PID=$DPID
	FIXTURE_DEV=$(mount_device "$MNT")
	echo dirty >"$MNT/umount_dirty"
	slow_writes "$SLOW_WRITE_MS"
	umount "$MNT" 2>"$OUT"
	if [ -n "$FIXTURE_PID" ] && [ -d "/proc/$FIXTURE_PID" ] && [ "$(mount_count "$MNT")" -eq 0 ]; then
		pass umount-helper-fixture-daemon-outlives-umount
	else
		fail umount-helper-fixture-daemon-outlives-umount "pid '$FIXTURE_PID' rc: $(cat "$OUT")"
	fi
	wait_daemon_exit "$FIXTURE_DEV"
	slow_writes 0

	# The helper waits for the daemon: it is gone when the helper returns, so
	# a mount of the same instance right after it finds the cache database
	# free.
	wrapper -o "dcfs.fstype=ext4,dcfs.cache_db=$CACHE/slow2.db" "$SLOW" "$MNT"
	only_one_daemon
	WAIT_PID=$DPID
	WAIT_DEV=$(mount_device "$MNT")
	echo dirty >"$MNT/umount_dirty"
	slow_writes "$SLOW_WRITE_MS"
	started=$(uptime_ms)
	"$UMOUNT_HELPER" "$MNT" >"$OUT" 2>&1
	hrc=$?
	elapsed=$(($(uptime_ms) - started))
	if [ "$hrc" -eq 0 ] && [ "$(mount_count "$MNT")" -eq 0 ] && [ "$elapsed" -ge 1000 ] && daemon_finished "$WAIT_DEV"; then
		pass umount-helper-waits-for-the-daemon
	else
		fail umount-helper-waits-for-the-daemon "rc=$hrc, took $elapsed ms, mounts=$(mount_count "$MNT"), daemon $WAIT_PID ($WAIT_DEV): $(cat "$OUT")"
	fi
	slow_writes 0
	wrapper -o "dcfs.fstype=ext4,dcfs.cache_db=$CACHE/slow2.db" "$SLOW" "$MNT"
	if [ "$WRC" -eq 0 ] && [ "$(cat "$MNT/umount_dirty" 2>&1)" = dirty ]; then
		pass umount-helper-then-mount-at-once
	else
		fail umount-helper-then-mount-at-once "rc=$WRC: $(cat "$OUT")"
	fi

	# Through a symbolic link to the mount point (the helper resolves it only
	# when nothing is mounted at the path as given), and by the other name.
	ln -sf "$MNT" /tmp/mnt_link
	echo dirty2 >"$MNT/umount_dirty2"
	only_one_daemon
	LINK_PID=$DPID
	LINK_DEV=$(mount_device "$MNT")
	slow_writes "$SLOW_WRITE_MS"
	/sbin/umount.fuse /tmp/mnt_link >"$OUT" 2>&1
	hrc=$?
	if [ "$hrc" -eq 0 ] && [ "$(mount_count "$MNT")" -eq 0 ] && daemon_finished "$LINK_DEV"; then
		pass umount-helper-through-a-symlink-as-umount-fuse
	else
		fail umount-helper-through-a-symlink-as-umount-fuse "rc=$hrc mounts=$(mount_count "$MNT") daemon $LINK_PID ($LINK_DEV): $(cat "$OUT")"
	fi
	slow_writes 0

	# A crashed daemon: nothing to wait for, and the dead mount unmounts.
	wrapper -o "dcfs.fstype=ext4,dcfs.cache_db=$CACHE/slow3.db" "$SLOW" "$MNT"
	only_one_daemon
	CRASH_DEV=$(mount_device "$MNT")
	kill -KILL "$DPID"
	"$UMOUNT_HELPER" "$MNT" >"$OUT" 2>&1
	hrc=$?
	wait_daemon_exit "$CRASH_DEV"
	if [ "$hrc" -eq 0 ] && [ "$(mount_count "$MNT")" -eq 0 ]; then
		pass umount-helper-daemon-already-gone
	else
		fail umount-helper-daemon-already-gone "rc=$hrc mounts=$(mount_count "$MNT"): $(cat "$OUT")"
	fi
	wrapper -o "dcfs.fstype=ext4,dcfs.cache_db=$CACHE/slow3.db" "$SLOW" "$MNT"
	if [ "$WRC" -eq 0 ]; then
		pass umount-helper-crashed-instance-mounts-again
	else
		fail umount-helper-crashed-instance-mounts-again "rc=$WRC: $(cat "$OUT")"
	fi

	# An unmount that does not end the superblock does not wait: the daemon
	# serves the mount still. A bind mount of it, then `-r` on a busy mount,
	# then a copy in another mount namespace. (A helper that waited for the
	# daemon here would wait for something the unmount did not do; the test
	# would hang until its own timeout.)
	only_one_daemon
	KEEP_PID=$DPID
	KEEP_DEV=$(mount_device "$MNT")
	echo keep >"$MNT/keep.txt"
	mkdir -p /tmp/bindcopy
	mount --bind "$MNT" /tmp/bindcopy
	"$UMOUNT_HELPER" "$MNT" >"$OUT" 2>&1
	hrc=$?
	if [ "$hrc" -eq 0 ] && [ "$(mount_count "$MNT")" -eq 0 ] && [ -d "/proc/$KEEP_PID" ] &&
		[ "$(cat /tmp/bindcopy/keep.txt 2>&1)" = keep ]; then
		pass umount-helper-bind-copy-keeps-the-daemon
	else
		fail umount-helper-bind-copy-keeps-the-daemon "rc=$hrc mounts=$(mount_count "$MNT"): $(cat "$OUT")"
	fi
	# The last copy ends it: now it waits, and the daemon is gone. The
	# connection's directory in fusectl is what tells the helper: there while
	# a copy is mounted, removed with the superblock.
	conn_before=no
	[ -d "$FUSECTL/${KEEP_DEV#*:}" ] && conn_before=yes
	slow_writes "$SLOW_WRITE_MS"
	echo more >/tmp/bindcopy/more.txt
	started=$(uptime_ms)
	"$UMOUNT_HELPER" /tmp/bindcopy >"$OUT" 2>&1
	hrc=$?
	elapsed=$(($(uptime_ms) - started))
	conn_after=yes
	[ -d "$FUSECTL/${KEEP_DEV#*:}" ] || conn_after=no
	if [ "$hrc" -eq 0 ] && daemon_finished "$KEEP_DEV" && [ "$conn_before" = yes ] && [ "$conn_after" = no ] && [ "$elapsed" -ge 1000 ]; then
		pass umount-helper-last-copy-waits
	else
		fail umount-helper-last-copy-waits "rc=$hrc daemon $KEEP_PID ($KEEP_DEV) fusectl entry before=$conn_before after=$conn_after, took $elapsed ms: $(cat "$OUT")"
	fi
	slow_writes 0

	wrapper -o "dcfs.fstype=ext4,dcfs.cache_db=$CACHE/slow6.db" "$SLOW" "$MNT"
	only_one_daemon
	NS_PID=$DPID
	NS_DEV=$(mount_device "$MNT")
	rm -f /tmp/ns.fifo /tmp/ns_in.fifo
	mkfifo /tmp/ns.fifo /tmp/ns_in.fifo
	unshare -m sh -c 'echo in >/tmp/ns_in.fifo; exec cat /tmp/ns.fifo >/dev/null' &
	read -r ns_says </tmp/ns_in.fifo
	exec 8>/tmp/ns.fifo
	"$UMOUNT_HELPER" "$MNT" >"$OUT" 2>&1
	hrc=$?
	if [ "$hrc" -eq 0 ] && [ "$(mount_count "$MNT")" -eq 0 ] && [ -d "/proc/$NS_PID" ]; then
		pass umount-helper-copy-in-another-namespace-keeps-the-daemon
	else
		fail umount-helper-copy-in-another-namespace-keeps-the-daemon "rc=$hrc mounts=$(mount_count "$MNT") daemon $NS_PID: $(cat "$OUT")"
	fi
	exec 8>&-
	wait_daemon_exit "$NS_DEV" # the namespace ends with its last process

	# A busy mount: -l detaches now and does not wait (the daemon exits when
	# the last user lets go); without -l the unmount fails with umount's own
	# status and nothing changes; -r (remount read-only if busy) is an unmount
	# that ended nothing.
	wrapper -o "dcfs.fstype=ext4,dcfs.cache_db=$CACHE/slow5.db" "$SLOW" "$MNT"
	only_one_daemon
	BUSY_PID=$DPID
	BUSY_DEV=$(mount_device "$MNT")
	rm -f /tmp/hold.fifo /tmp/in.fifo
	mkfifo /tmp/hold.fifo /tmp/in.fifo
	(cd "$MNT" && echo in >/tmp/in.fifo && exec cat /tmp/hold.fifo >/dev/null) &
	read -r busy_says </tmp/in.fifo
	exec 9>/tmp/hold.fifo
	/bin/umount "$MNT" >"$OUT.native" 2>&1
	native_rc=$?
	"$UMOUNT_HELPER" "$MNT" >"$OUT" 2>&1
	hrc=$?
	if [ "$native_rc" -ne 0 ] && [ "$hrc" -eq "$native_rc" ] && grep -qi 'busy' "$OUT" && [ "$(mount_count "$MNT")" -eq 1 ] && [ -d "/proc/$BUSY_PID" ]; then
		pass umount-helper-busy-fails-with-umounts-status
	else
		fail umount-helper-busy-fails-with-umounts-status "umount rc=$native_rc, helper rc=$hrc mounts=$(mount_count "$MNT"): $(cat "$OUT")"
	fi
	"$UMOUNT_HELPER" -r "$MNT" >"$OUT" 2>&1
	hrc=$?
	if [ "$hrc" -eq 0 ] && [ "$(mount_count "$MNT")" -eq 1 ] && [ -d "/proc/$BUSY_PID" ]; then
		pass umount-helper-r-on-a-busy-mount-returns-with-the-daemon-alive
	else
		fail umount-helper-r-on-a-busy-mount-returns-with-the-daemon-alive "rc=$hrc mounts=$(mount_count "$MNT"): $(cat "$OUT")"
	fi
	"$UMOUNT_HELPER" -l "$MNT" >"$OUT" 2>&1
	hrc=$?
	if [ "$hrc" -eq 0 ] && [ "$(mount_count "$MNT")" -eq 0 ] && [ -d "/proc/$BUSY_PID" ]; then
		pass umount-helper-lazy-detaches-and-does-not-wait
	else
		fail umount-helper-lazy-detaches-and-does-not-wait "rc=$hrc mounts=$(mount_count "$MNT") daemon=$([ -d "/proc/$BUSY_PID" ] && echo alive || echo gone): $(cat "$OUT")"
	fi
	exec 9>&-
	# The daemon exits once the last user is gone (one that never did would
	# hang here until the test's own timeout).
	wait_daemon_exit "$BUSY_DEV"
	pass umount-helper-lazy-daemon-exits-when-released

	# -f aborts the connection: an idle mount unmounts and the daemon goes.
	wrapper -o "dcfs.fstype=ext4,dcfs.cache_db=$CACHE/slow4.db" "$SLOW" "$MNT"
	"$UMOUNT_HELPER" -f "$MNT" >"$OUT" 2>&1
	hrc=$?
	if [ "$hrc" -eq 0 ] && [ "$(mount_count "$MNT")" -eq 0 ]; then
		pass umount-helper-force
	else
		fail umount-helper-force "rc=$hrc: $(cat "$OUT")"
	fi

	# Without fusectl the helper cannot tell whether the unmount ended the
	# superblock, and does not wait: the daemon is still syncing when it
	# returns.
	umount "$FUSECTL"
	wrapper -o "dcfs.fstype=ext4,dcfs.cache_db=$CACHE/slow7.db" "$SLOW" "$MNT"
	only_one_daemon
	NOCTL_PID=$DPID
	NOCTL_DEV=$(mount_device "$MNT")
	echo dirty3 >"$MNT/umount_dirty3"
	slow_writes "$SLOW_WRITE_MS"
	"$UMOUNT_HELPER" "$MNT" >"$OUT" 2>&1
	hrc=$?
	if [ "$hrc" -eq 0 ] && [ "$(mount_count "$MNT")" -eq 0 ] && [ -d "/proc/$NOCTL_PID" ]; then
		pass umount-helper-without-fusectl-does-not-wait
	else
		fail umount-helper-without-fusectl-does-not-wait "rc=$hrc daemon $NOCTL_PID: $(cat "$OUT")"
	fi
	wait_daemon_exit "$NOCTL_DEV"
	slow_writes 0
	mount -t fusectl fusectl "$FUSECTL"

	# A usage mistake is 1; a mount that is not dcfs's is simply unmounted
	# (umount -i), with umount's status; options the helper does not know go to
	# umount.
	"$UMOUNT_HELPER" >"$OUT" 2>&1
	rc_none=$?
	"$UMOUNT_HELPER" -f >"$OUT.2" 2>&1
	rc_flags=$?
	"$UMOUNT_HELPER" "$MNT" /tmp >"$OUT.3" 2>&1
	rc_two=$?
	if [ "$rc_none" -eq 1 ] && [ "$rc_flags" -eq 1 ] && [ "$rc_two" -eq 1 ] && grep -q 'usage: umount.fuse.dcfs' "$OUT"; then
		pass umount-helper-usage-errors-exit-1
	else
		fail umount-helper-usage-errors-exit-1 "none=$rc_none flags=$rc_flags two=$rc_two: $(cat "$OUT")"
	fi
	mkdir -p /tmp/tm
	mount -t tmpfs tmpfs /tmp/tm
	/sbin/umount.fuse -n /tmp/tm >"$OUT" 2>&1
	rc_plain=$?
	if [ "$rc_plain" -eq 0 ] && [ "$(mount_count /tmp/tm)" -eq 0 ]; then
		pass umount-helper-unmounts-what-is-not-dcfs
	else
		fail umount-helper-unmounts-what-is-not-dcfs "rc=$rc_plain: $(cat "$OUT")"
	fi
	"$UMOUNT_HELPER" /tmp/not-mounted >"$OUT" 2>&1
	rc_not=$?
	if [ "$rc_not" -ne 0 ]; then
		pass umount-helper-passes-umounts-failure-on
	else
		fail umount-helper-passes-umounts-failure-on "rc=0 for something that is not mounted"
	fi
	fault_unwrap slow
fi

# --- fsck.dcfs (step 15.5) -------------------------------------------------------
#
# fsck(8) and systemd-fsck run `fsck.dcfs FLAGS DEVICE` for a line with a
# passno. It finds its line's options through findmnt (util-linux: the systemd
# guest covers that), or takes them with -o, which is what this guest does: no
# findmnt here. The backing's fsck is a stand-in script of type `fakefs` that
# records what it was given and exits with the status in /tmp/fake.status.
FSCK=/sbin/fsck.dcfs
cat >/sbin/fsck.fakefs <<'EOF_FAKE'
#!/bin/sh
echo "$*" >/tmp/fake.args
exit "$(cat /tmp/fake.status)"
EOF_FAKE
chmod +x /sbin/fsck.fakefs

if [ ! -x "$FSCK" ]; then
	fail fsck-installed "$FSCK is not an executable"
else
	"$FSCK" -V >"$OUT" 2>&1
	if [ $? -eq 0 ] && grep -q '^fsck.dcfs (dcfs ' "$OUT"; then
		pass fsck-version
	else
		fail fsck-version "$(cat "$OUT")"
	fi

	# A cache that is not a database.
	garbage_cache() { head -c 8192 /dev/zero | tr '\0' x >"$1"; }

	# bind: a directory, no device to check; the cache is.
	rm -f "$CACHE/fs1.db"
	"$FSCK" -n -o "dcfs.fstype=bind,dcfs.cache_db=$CACHE/fs1.db" "$SRC" >"$OUT" 2>&1
	if [ $? -eq 0 ] && grep -q 'no device to check' "$OUT" && [ ! -e "$CACHE/fs1.db" ]; then
		pass fsck-bind-has-no-device-and-creates-nothing
	else
		fail fsck-bind-has-no-device-and-creates-nothing "rc=$? $(cat "$OUT")"
	fi

	# The backing's fsck gets the flags and the device, and its status is
	# relayed, or'ed with the cache's.
	rm -f "$CACHE/fs2.db"
	echo 0 >/tmp/fake.status
	"$FSCK" -a -f -C3 -o "dcfs.fstype=fakefs,dcfs.cache_db=$CACHE/fs2.db" /dev/fake >"$OUT" 2>&1
	rc_ok=$?
	if [ "$rc_ok" -eq 0 ] && [ "$(cat /tmp/fake.args)" = "-a -f -C3 /dev/fake" ]; then
		pass fsck-backing-gets-flags-and-device
	else
		fail fsck-backing-gets-flags-and-device "rc=$rc_ok args='$(cat /tmp/fake.args)' $(cat "$OUT")"
	fi
	all_ok=1
	for want in 1 4 8 12; do
		echo "$want" >/tmp/fake.status
		"$FSCK" -n -o "dcfs.fstype=fakefs,dcfs.cache_db=$CACHE/fs2.db" /dev/fake >"$OUT" 2>&1
		[ $? -eq "$want" ] || { all_ok=0; echo "backing $want relayed as $?" >>"$OUT.why"; }
	done
	if [ "$all_ok" -eq 1 ]; then
		pass fsck-relays-the-backings-status
	else
		fail fsck-relays-the-backings-status "$(cat "$OUT.why")"
	fi

	# The cache: -n reports (4), -y rebuilds (1), and the two statuses combine.
	echo 0 >/tmp/fake.status
	garbage_cache "$CACHE/fs3.db"
	"$FSCK" -n -o "dcfs.fstype=fakefs,dcfs.cache_db=$CACHE/fs3.db" /dev/fake >"$OUT" 2>&1
	rc_n=$?
	if [ "$rc_n" -eq 4 ] && grep -q 'is corrupt' "$OUT" && [ -e "$CACHE/fs3.db" ]; then
		pass fsck-n-reports-a-corrupt-cache
	else
		fail fsck-n-reports-a-corrupt-cache "rc=$rc_n $(cat "$OUT")"
	fi
	"$FSCK" -y -o "dcfs.fstype=fakefs,dcfs.cache_db=$CACHE/fs3.db" /dev/fake >"$OUT" 2>&1
	rc_y=$?
	if [ "$rc_y" -eq 1 ] && grep -q 'starts cold' "$OUT" && [ ! -e "$CACHE/fs3.db" ]; then
		pass fsck-y-rebuilds-a-corrupt-cache
	else
		fail fsck-y-rebuilds-a-corrupt-cache "rc=$rc_y $(cat "$OUT")"
	fi
	garbage_cache "$CACHE/fs3.db"
	echo 4 >/tmp/fake.status
	"$FSCK" -p -o "dcfs.fstype=fakefs,dcfs.cache_db=$CACHE/fs3.db" /dev/fake >"$OUT" 2>&1
	rc_c=$?
	[ "$rc_c" -eq 5 ] && pass fsck-combines-backing-4-and-cache-1 || fail fsck-combines-backing-4-and-cache-1 "rc=$rc_c $(cat "$OUT")"
	garbage_cache "$CACHE/fs3.db"
	echo 8 >/tmp/fake.status
	"$FSCK" -n -o "dcfs.fstype=fakefs,dcfs.cache_db=$CACHE/fs3.db" /dev/fake >"$OUT" 2>&1
	rc_c=$?
	[ "$rc_c" -eq 12 ] && pass fsck-combines-backing-8-and-cache-4 || fail fsck-combines-backing-8-and-cache-4 "rc=$rc_c $(cat "$OUT")"
	rm -f "$CACHE/fs3.db"

	# A database a daemon holds is reported, never waited for or touched.
	wrapper -o "dcfs.fstype=bind,dcfs.cache_db=$CACHE/fs4.db" "$SRC" "$MNT"
	only_one_daemon
	FSCK_HELD_PID=$DPID
	"$FSCK" -y -o "dcfs.fstype=bind,dcfs.cache_db=$CACHE/fs4.db" "$SRC" >"$OUT" 2>&1
	rc_h=$?
	if [ "$rc_h" -eq 8 ] && grep -q 'in use by a running dcfs' "$OUT" && [ -e "$CACHE/fs4.db" ] && [ -d "/proc/$FSCK_HELD_PID" ]; then
		pass fsck-held-cache-is-reported-not-touched
	else
		fail fsck-held-cache-is-reported-not-touched "rc=$rc_h $(cat "$OUT")"
	fi
	[ -n "$FSCK_HELD_PID" ] && unmount_check fsck-held-daemon-unmounts "$MNT"
	# And once unmounted the same database checks clean.
	"$FSCK" -n -o "dcfs.fstype=bind,dcfs.cache_db=$CACHE/fs4.db" "$SRC" >"$OUT" 2>&1
	[ $? -eq 0 ] && grep -q ': clean' "$OUT" && pass fsck-unmounted-cache-is-clean || fail fsck-unmounted-cache-is-clean "$(cat "$OUT")"

	# Usage and operational errors: 16, and 8 (no fstab line: findmnt is not here).
	"$FSCK" >"$OUT" 2>&1
	rc_u1=$?
	"$FSCK" /dev/a /dev/b >"$OUT" 2>&1
	rc_u2=$?
	"$FSCK" -o >"$OUT" 2>&1
	rc_u3=$?
	if [ "$rc_u1" -eq 16 ] && [ "$rc_u2" -eq 16 ] && [ "$rc_u3" -eq 16 ] && grep -q 'usage: fsck.dcfs' "$OUT"; then
		pass fsck-usage-errors-are-16
	else
		fail fsck-usage-errors-are-16 "$rc_u1 $rc_u2 $rc_u3: $(cat "$OUT")"
	fi
	"$FSCK" -n /dev/nowhere >"$OUT" 2>&1
	rc_o=$?
	[ "$rc_o" -eq 8 ] && grep -q 'fstab' "$OUT" && pass fsck-no-fstab-line-is-8 || fail fsck-no-fstab-line-is-8 "rc=$rc_o $(cat "$OUT")"
	"$FSCK" -n -o "dcfs.fstype=fakefs" /dev/fake >"$OUT" 2>&1
	rc_o=$?
	[ "$rc_o" -eq 8 ] && grep -q 'dcfs.cache_db' "$OUT" && pass fsck-no-cache-db-is-8 || fail fsck-no-cache-db-is-8 "rc=$rc_o $(cat "$OUT")"
	"$FSCK" -n -o "dcfs.fstype=nosuchfs,dcfs.cache_db=$CACHE/fs5.db" /dev/fake >"$OUT" 2>&1
	rc_o=$?
	[ "$rc_o" -eq 8 ] && grep -q 'fsck.nosuchfs not found' "$OUT" && pass fsck-missing-checker-is-8 || fail fsck-missing-checker-is-8 "rc=$rc_o $(cat "$OUT")"
fi

require_no_reclaim no-reclaim
exit "$FAILED"
