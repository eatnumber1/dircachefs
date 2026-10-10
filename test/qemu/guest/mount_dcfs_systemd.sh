#!/bin/sh
# dcfs step 15.6 acceptance test: mount.dcfs through util-linux's mount(8),
# fstab and systemd (phase 15, decision 14).
#
# The busybox guest of mount_dcfs.sh runs the wrapper directly (its mount
# runs no mount.<type> helpers). This one is a released Debian cloud image
# with systemd as PID 1 (README.md, "The systemd guest"): util-linux's
# mount(8) and umount(8), libmount's option passing, the mount units systemd
# generates from /etc/fstab, journald. The daemons live in their mount units'
# cgroups here, as they do on a real machine.
#
# Two boots over the same disks (run-qemu.sh --boots 2). Boot 1 checks the
# command line (mount -t dcfs, remount, umount, exit statuses), fstab
# (mount -a, then the units systemd generates from it: start, status,
# restart, stop, failure) and what the daemons log to the journal; it leaves
# the fstab mounts up and reboots (guest/systemd_run.sh). Boot 2 checks what a
# reboot leaves: the mounts were made by systemd at boot, in order, with the
# cache from boot 1 (a warm tree reads nothing from the backing disk), the
# previous boot's daemons shut down cleanly, and a nofail mount of a device
# that is missing did not hold the boot.
#
# Run as /tests/mount_dcfs_systemd.sh by guest/systemd_run.sh, from the
# oneshot unit dcfs-test.service; one "TEST ... PASS/FAIL/SKIP" line per
# check.
FAILED=0
. "$(dirname "$0")/lib.sh"

BOOT=$(sed -n 's/.*\bdcfs_boot=\([^ ]*\).*/\1/p' /proc/cmdline)
BOOT=${BOOT:-1}
# The backing disks start at vda: a microvm guest sees at most four virtio
# disks (a fifth is not probed, measured 2026-10-09), and the image is the
# fourth. The kernel mounts the xfs and the btrfs: the image has no mkfs
# tools, run-qemu.sh made them.
DEV=/dev/vda # ext4
DEV_XFS=/dev/vdb
DEV_BTRFS=/dev/vdc
CACHE=/var/cache/dcfs
OUT=/tmp/out
SRC=/srv/live # the none form's directory
RAW=/srv/raw  # the bind form's
MNT=/mnt/m    # for the command-line checks
MISSING_UUID=0e0e0e0e-dead-4bee-8f00-000000000001

require_commands blkid findmnt journalctl pgrep python3 systemctl systemd-escape umount mount awk

# --- helpers -------------------------------------------------------------

# The pids of the running dcfs daemons: the processes started as mount.dcfs
# (libmount runs /sbin/mount.dcfs; a sanitizer build runs dcfs.real through
# its loader, guest/init). One pgrep, not a walk of /proc in shell: the checks
# poll this on a slow host.
daemons() { pgrep -f '(^|/)(mount\.dcfs|dcfs\.real) ' 2>/dev/null; }

# The pid of the daemon when there is just the one, else nothing.
only_daemon() {
	set -- $(daemons)
	if [ "$#" -eq 1 ]; then echo "$1"; fi
}

# fs_of MOUNTPOINT: "FSTYPE SOURCE" of the topmost mount there.
fs_of() { findmnt -n -o FSTYPE,SOURCE -M "$1" 2>/dev/null | tail -n 1; }

# mount_count MOUNTPOINT: how many mounts there are exactly at it.
mount_count() { findmnt -rn -M "$1" -o TARGET 2>/dev/null | wc -l; }

# wait_exit PID...: blocks until each process has exited, on its pidfd
# (python3 is in the image): an event, not a poll, and no timeout of its own;
# a hung daemon is bounded by the test's timeout (Bazel's). For what umount.
# fuse.dcfs cannot wait for: a daemon that failed to start, or was killed.
wait_exit() {
	[ "$#" -eq 0 ] && return 0
	python3 -I -c 'import os, select, sys
for pid in sys.argv[1:]:
    try:
        fd = os.pidfd_open(int(pid))
    except ProcessLookupError:
        continue
    select.select([fd], [], [])' "$@"
}

# wait_no_daemons: until no dcfs daemon is left (for a test that has none
# running otherwise).
wait_no_daemons() { wait_exit $(daemons); }

# journal_has PATTERN JOURNALCTL-ARGS...: the journal has a line matching the
# grep pattern. journald takes the daemon's datagram asynchronously:
# `journalctl --sync` returns when everything it has received is written.
journal_has() {
	j_pat=$1
	shift
	journalctl --sync 2>/dev/null
	journalctl --no-pager -o cat "$@" 2>/dev/null | grep -q -e "$j_pat"
}

# mnt ARGS...: mount(8), output in $OUT, status in MRC (the test's own
# timeout bounds a hung mount).
mnt() {
	mount "$@" >"$OUT" 2>&1
	MRC=$?
}

# log_helper NAME: makes /sbin/NAME a script that records its arguments in
# /tmp/helper.log and runs the real one, to see which helper util-linux ran
# (restore_helpers puts the installed state back).
log_helper() {
	rm -f "/sbin/$1"
	cat >"/sbin/$1" <<EOF_LOG
#!/bin/sh
echo "$1 \$*" >>/tmp/helper.log
exec /usr/local/bin/$1 "\$@"
EOF_LOG
	chmod +x "/sbin/$1"
}

# restore_helpers: the state boot 1's checks run in: umount.fuse.dcfs, the
# link guest/init installs, and umount.fuse, the opt-in (README.md, "Unmount")
# that makes a plain `umount PATH` wait for the daemon too: libmount looks for
# umount.fuse alone on that path (see the helper checks below), and the
# checks that unmount and then mount the same instance at once need it.
restore_helpers() {
	rm -f /sbin/umount.fuse /sbin/umount.fuse.dcfs
	ln -s /usr/local/bin/umount.fuse.dcfs /sbin/umount.fuse.dcfs
	ln -s /usr/local/bin/umount.fuse /sbin/umount.fuse
}

# cursor: the journal's cursor now; journal_failures_since CURSOR prints the
# lines of dcfs and of the mount units since then that say a start or a stop
# went wrong (the flock "in use" of the restart race in particular).
cursor() {
	journalctl --sync 2>/dev/null
	journalctl --no-pager -n 0 --show-cursor 2>/dev/null | sed -n 's/^-- cursor: //p'
}
journal_failures_since() {
	journalctl --sync 2>/dev/null
	journalctl --no-pager -o cat --after-cursor "$1" 2>/dev/null |
		grep -i -e 'in use' -e 'Failed to mount' -e 'Failed with result' -e 'ERROR' || true
}

# cleanup_mount MOUNTPOINT: unmounts whatever a failed check left there.
cleanup_mount() {
	umount "$1" 2>/dev/null || umount -l "$1" 2>/dev/null || true
}

dump_state() {
	echo "--- state ---"
	findmnt -t fuse.dcfs 2>&1
	systemctl --no-pager --failed 2>&1 | head -n 20
	journalctl --no-pager -b -t dcfs 2>&1 | tail -n 30
}

# --- the guest ------------------------------------------------------------

echo "systemd guest: boot $BOOT, kernel $(uname -r), $(mount --version | head -n 1)"
# What the boot looked like when this script began (boot 2's nofail check
# reads them: the checks before it take a while).
START_UP=$(cut -d. -f1 /proc/uptime)
START_JOBS=$(systemctl list-jobs --no-pager 2>/dev/null)
START_MISSING=$(systemctl is-active mnt-missing.mount)
echo "up $START_UP s at the start of the script"

pid1=$(readlink /proc/1/exe)
case "$pid1" in
*/systemd) pass systemd-is-pid1 ;;
*) fail systemd-is-pid1 "pid 1 is $pid1" ;;
esac
mount --version | grep -q 'util-linux' && pass util-linux-mount || fail util-linux-mount "$(mount --version)"

# Nobody can log in on the console: the nocloud image logs root in on the
# serial console without a password; the getty templates are masked
# (guest/systemd_install.sh) and root's password is locked.
getty_state=$(systemctl is-enabled serial-getty@ttyS0.service 2>&1)
gettys=""
for g_c in /proc/[0-9]*/comm; do
	case "$(cat "$g_c" 2>/dev/null)" in agetty | login | getty) gettys="$gettys ${g_c#/proc/}" ;; esac
done
root_pw=$(getent shadow root | cut -d: -f2)
case "$root_pw" in
'!'* | '*'*) pw_locked=1 ;;
*) pw_locked=0 ;;
esac
if [ "$getty_state" = masked ] && [ -z "$gettys" ] && [ "$pw_locked" -eq 1 ]; then
	pass console-login-disabled
else
	fail console-login-disabled "serial-getty is $getty_state, login processes:$gettys, root password locked: $pw_locked"
fi

# The quiet kernel guest/init sets (step 26.14) is still set under systemd:
# nothing in the image (systemd-sysctl, tmpfiles, udev) changes those.
quiet_ok=1
for kv in dirty_writeback_centisecs=0 dirty_expire_centisecs=8640000 laptop_mode=0 vfs_cache_pressure=100; do
	[ "$(cat "/proc/sys/vm/${kv%%=*}")" = "${kv#*=}" ] || quiet_ok=0
done
if [ "$quiet_ok" -eq 1 ]; then
	pass quiet-kernel-sysctls-survive-systemd
else
	fail quiet-kernel-sysctls-survive-systemd "$(cd /proc/sys/vm && for f in dirty_writeback_centisecs dirty_expire_centisecs laptop_mode vfs_cache_pressure; do echo "$f=$(cat $f)"; done | tr '\n' ' ')"
fi

[ -x /sbin/mount.dcfs ] && [ -x /sbin/mount.fuse.dcfs ] ||
	fail wrapper-installed "/sbin/mount.dcfs and /sbin/mount.fuse.dcfs are not both executable"

mkdir -p -m 0700 "$CACHE"
mkdir -p "$SRC" "$RAW" "$MNT" /data /mnt/live /mnt/missing /mnt/fail /mnt/xfs /mnt/btrfs /tmp/other

# restart_check MODE: systemctl restart of rp.mount, three times. MODE both
# names rp-c.mount too, so systemctl waits for both jobs and its status is
# theirs; MODE parent names only rp.mount (the child's restart is propagated by
# systemd: Requires=), and then `systemctl start rp-c.mount` waits for that
# job. That start could start a child whose restart failed and so hide the
# failure, so the journal since a cursor must show no failure line (the unit
# the start would have started again leaves one: the helper's "in use", mount's
# "Failed to mount"), and both daemons must have changed. Prints why not.
restart_check() {
	rc_mode=$1
	rc_n=0
	while [ "$rc_n" -lt 3 ]; do
		rc_n=$((rc_n + 1))
		rc_before=$(daemons | sort | tr '\n' ' ')
		rc_cursor=$(cursor)
		if [ "$rc_mode" = both ]; then
			systemctl restart rp.mount rp-c.mount 2>/tmp/restart.err
		else
			systemctl restart rp.mount 2>/tmp/restart.err && systemctl start rp-c.mount 2>>/tmp/restart.err
		fi
		rc_status=$?
		rc_failures=$(journal_failures_since "$rc_cursor")
		if [ "$rc_status" -ne 0 ]; then
			echo "restart ($rc_mode) $rc_n failed: $(cat /tmp/restart.err); rp.mount is $(systemctl is-active rp.mount), rp-c.mount $(systemctl is-active rp-c.mount); $rc_failures"
			return 1
		fi
		rc_after=$(daemons | sort | tr '\n' ' ')
		rc_new=0
		for rc_pid in $rc_after; do
			case " $rc_before " in *" $rc_pid "*) ;; *) rc_new=$((rc_new + 1)) ;; esac
		done
		if [ "$(systemctl is-active rp.mount)" != active ] || [ "$(systemctl is-active rp-c.mount)" != active ] ||
			[ "$(cat /rp/c/rc.txt 2>&1)" != rc ] || [ "$rc_new" -ne 2 ] || [ -n "$rc_failures" ]; then
			echo "restart ($rc_mode) $rc_n left rp.mount $(systemctl is-active rp.mount), rp-c.mount $(systemctl is-active rp-c.mount), rp/c: $(cat /rp/c/rc.txt 2>&1); daemons $rc_before -> $rc_after ($rc_new new); journal: $rc_failures"
			return 1
		fi
	done
}

# reboot_clean_check: the previous boot's daemons all logged a clean
# shutdown and this boot's starts all found it clean. Prints why not.
reboot_clean_check() {
	rcc_before=$(journalctl --no-pager -b -1 -t dcfs -o cat 2>/dev/null | grep -c 'shutdown: clean')
	rcc_now=$(journalctl --no-pager -b 0 -t dcfs -o cat 2>/dev/null | grep -c 'recovery: the last run ended cleanly')
	rcc_unclean=$(journalctl --no-pager -b 0 -t dcfs -o cat 2>/dev/null | grep -c 'did not shut down cleanly')
	[ "$rcc_before" -ge 5 ] && [ "$rcc_now" -ge 5 ] && [ "$rcc_unclean" -eq 0 ] && return 0
	echo "previous boot: $rcc_before clean shutdowns; this boot: $rcc_now clean starts, $rcc_unclean unclean"
	return 1
}

# --- boot 1 ---------------------------------------------------------------

boot1() {
	# The fixtures: an ext4 on vda (a virtio disk, as a real machine's
	# data disk) with a directory for the nested mount, a directory for the
	# none form, one for the bind form.
	restore_helpers
	mount "$DEV" /mnt/m || fail fixture-mount "cannot mount $DEV"
	i=0
	while [ "$i" -lt 5 ]; do
		echo "content $i" >"/mnt/m/file_$i.txt"
		i=$((i + 1))
	done
	mkdir -p /mnt/m/d1/d2 /mnt/m/sub
	echo deep >/mnt/m/d1/d2/deep.txt
	echo "under the mount point" >/mnt/m/sub/covered.txt
	umount /mnt/m
	for fixture in "xfs:$DEV_XFS" "btrfs:$DEV_BTRFS"; do
		fname=${fixture%%:*}
		fdev=${fixture#*:}
		mount "$fdev" /mnt/m || fail "fixture-mount-$fname" "cannot mount $fdev"
		echo "$fname content" >"/mnt/m/$fname.txt"
		umount /mnt/m
	done
	XFS_UUID=$(blkid -s UUID -o value "$DEV_XFS")
	BTRFS_UUID=$(blkid -s UUID -o value "$DEV_BTRFS")
	UUID=$(blkid -s UUID -o value "$DEV")
	echo live >"$SRC/live.txt"
	echo raw >"$RAW/raw.txt"
	if [ -z "$UUID" ] || [ -z "$XFS_UUID" ] || [ -z "$BTRFS_UUID" ]; then
		fail fixture-uuid "blkid prints no UUID for $DEV ($UUID), $DEV_XFS ($XFS_UUID) or $DEV_BTRFS ($BTRFS_UUID)"
		return
	fi

	# --- mount -t dcfs through mount(8) (the helper dispatch) ---
	mnt -t dcfs -o "dcfs.fstype=none,dcfs.cache_db=$CACHE/m1.db" "$SRC" "$MNT"
	pid=$(only_daemon)
	set -- $(fs_of "$MNT")
	if [ "$MRC" -eq 0 ] && [ -n "$pid" ] && [ "$1" = fuse.dcfs ] && [ "$2" = "$SRC" ] &&
		[ "$(cat "$MNT/live.txt" 2>&1)" = live ]; then
		pass mount-t-dcfs
	else
		fail mount-t-dcfs "rc=$MRC type=$1 source=$2 daemons=$(daemons) out=$(cat "$OUT")"
	fi
	# What libmount adds to a helper's options (rw, fstab's nofail and _netdev,
	# defaults, noauto) is the helper's business, not an error.
	umount "$MNT" 2>/dev/null
	mnt -t dcfs -o "rw,nofail,_netdev,defaults,dcfs.fstype=none,dcfs.cache_db=$CACHE/m2.db" "$SRC" "$MNT"
	pid=$(only_daemon)
	if [ "$MRC" -eq 0 ] && [ -n "$pid" ] && [ "$(cat "$MNT/live.txt" 2>&1)" = live ]; then
		pass mount-passes-libmount-options
	else
		fail mount-passes-libmount-options "rc=$MRC out=$(cat "$OUT")"
	fi

	# --- remount through mount(8): mount.fuse.dcfs, only the dcfs mount ---
	mnt -o remount,dcfs.ro "$MNT"
	rc_ro=$MRC
	out_ro=$(cat "$OUT")
	touch "$MNT/after_ro" 2>"$OUT"
	if [ "$rc_ro" -eq 0 ] && grep -q 'Read-only file system' "$OUT" &&
		findmnt -n -o OPTIONS -M "$MNT" | grep -q -w ro; then
		pass remount-dcfs-ro
	else
		fail remount-dcfs-ro "rc=$rc_ro out=$out_ro touch: $(cat "$OUT") options: $(findmnt -n -o OPTIONS -M "$MNT")"
	fi
	if [ "$rc_ro" -eq 0 ] && findmnt -n -o OPTIONS -M "$MNT" | grep -q -w ro && touch "$SRC/backing_still_rw" 2>/dev/null; then
		pass remount-leaves-backing-rw
	else
		fail remount-leaves-backing-rw "rc=$rc_ro, the dcfs mount is $(findmnt -n -o OPTIONS -M "$MNT"), or the backing is read-only"
	fi
	mnt -o remount,rw "$MNT"
	if [ "$MRC" -eq 0 ] && touch "$MNT/after_rw" 2>"$OUT"; then
		pass remount-back-to-rw
	else
		fail remount-back-to-rw "rc=$MRC out=$(cat "$OUT") touch: $(cat "$OUT")"
	fi
	# A native option in a remount is ignored, with a WARNING mount(8) shows
	# on its stderr: the underlying mount is not reachable from here.
	mnt -o remount,dcfs.ro,noatime "$MNT"
	if [ "$MRC" -eq 0 ] && grep -q '^W[0-9]' "$OUT" && grep -q 'noatime' "$OUT" &&
		findmnt -n -o OPTIONS -M "$MNT" | grep -q -w ro; then
		pass remount-warns-about-native-options
	else
		fail remount-warns-about-native-options "rc=$MRC out=$(cat "$OUT")"
	fi
	mnt -o remount,rw "$MNT"

	# --- umount ---
	umount "$MNT" 2>"$OUT"
	urc=$?
	# At once: umount.fuse.dcfs returned when the daemon had exited.
	if [ "$urc" -eq 0 ] && [ -z "$(daemons)" ] && [ "$(mount_count "$MNT")" -eq 0 ]; then
		pass umount-stops-dcfs
	else
		fail umount-stops-dcfs "umount rc=$urc daemon $pid: $(cat "$OUT") $(fs_of "$MNT")"
		kill -KILL "$pid" 2>/dev/null
		cleanup_mount "$MNT"
	fi

	# --- exit statuses, as mount(8) reports them ---
	# A usage error: the wrapper exits 1, with its one ERROR line on the
	# stderr mount(8) passes through.
	mnt -t dcfs -o "dcfs.bogus,dcfs.fstype=none,dcfs.cache_db=$CACHE/bogus.db" "$SRC" "$MNT"
	# A start that failed after the fork leaves its daemon exiting for a moment.
	wait_no_daemons
	errors=$(grep -c '^E[0-9]' "$OUT")
	if [ "$MRC" -eq 1 ] && [ "$errors" -eq 1 ] && grep -q 'dcfs.bogus' "$OUT" && [ -z "$(daemons)" ] &&
		[ "$(mount_count "$MNT")" -eq 0 ]; then
		pass exit-status-usage-error
	else
		fail exit-status-usage-error "rc=$MRC, $errors ERROR lines, out=$(cat "$OUT")"
	fi
	# A failed start after the fork (the cache database's writer lock is
	# held): 32, mount failure, and dcfs's message.
	: >"$CACHE/locked.db"
	# The locker says READY on a fifo once it holds the lock: reading it is the
	# wait.
	mkfifo /tmp/lock.fifo
	testutil sqlite-lock "$CACHE/locked.db" 30 >/tmp/lock.fifo 2>&1 &
	locker=$!
	read -r locker_says </tmp/lock.fifo
	mnt -t dcfs -o "dcfs.fstype=none,dcfs.cache_db=$CACHE/locked.db" "$SRC" "$MNT"
	# A start that failed after the fork leaves its daemon exiting for a moment.
	wait_no_daemons
	errors=$(grep -c '^E[0-9]' "$OUT")
	if [ "$MRC" -eq 32 ] && [ "$errors" -eq 1 ] && grep -qi 'locked' "$OUT" && [ -z "$(daemons)" ] &&
		[ "$(mount_count "$MNT")" -eq 0 ]; then
		pass exit-status-failed-start
	else
		fail exit-status-failed-start "rc=$MRC, $errors ERROR lines, out=$(cat "$OUT") daemons=$(daemons)"
	fi
	kill "$locker" 2>/dev/null
	wait "$locker" 2>/dev/null
	# A native mount that fails: its error text and its own exit status,
	# nothing mounted (a wrong type; a device that is not there).
	mount -t bogusfs "$DEV" /tmp/other >/dev/null 2>&1
	native_rc=$?
	mnt -t dcfs -o "dcfs.fstype=bogusfs,dcfs.cache_db=$CACHE/wrongtype.db" "$DEV" "$MNT"
	# A start that failed after the fork leaves its daemon exiting for a moment.
	wait_no_daemons
	if [ "$native_rc" -ne 0 ] && [ "$MRC" -eq "$native_rc" ] && grep -q "unknown filesystem type 'bogusfs'" "$OUT" &&
		[ -z "$(daemons)" ] && [ "$(mount_count "$MNT")" -eq 0 ]; then
		pass exit-status-native-wrong-type
	else
		fail exit-status-native-wrong-type "native rc=$native_rc, rc=$MRC, out=$(cat "$OUT")"
	fi
	mount -t ext4 /dev/nonexistent /tmp/other >/dev/null 2>&1
	native_rc=$?
	mnt -t dcfs -o "dcfs.fstype=ext4,dcfs.cache_db=$CACHE/nodev.db" /dev/nonexistent "$MNT"
	# A start that failed after the fork leaves its daemon exiting for a moment.
	wait_no_daemons
	if [ "$native_rc" -ne 0 ] && [ "$MRC" -eq "$native_rc" ] && grep -qi 'nonexistent' "$OUT" &&
		[ -z "$(daemons)" ] && [ "$(mount_count "$MNT")" -eq 0 ]; then
		pass exit-status-native-missing-device
	else
		fail exit-status-native-missing-device "native rc=$native_rc, rc=$MRC, out=$(cat "$OUT")"
	fi
	# A native option the none form cannot honor (ro) is a usage error.
	mnt -t dcfs -o "ro,dcfs.fstype=none,dcfs.cache_db=$CACHE/noneopts.db" "$SRC" "$MNT"
	# A start that failed after the fork leaves its daemon exiting for a moment.
	wait_no_daemons
	if [ "$MRC" -eq 1 ] && grep -q 'dcfs.fstype=none' "$OUT" && [ -z "$(daemons)" ]; then
		pass exit-status-none-refuses-ro
	else
		fail exit-status-none-refuses-ro "rc=$MRC out=$(cat "$OUT")"
	fi

	# --- fstab, mount -a ---
	# One line of each kind the guest can host: a native ext4 by UUID (the type
	# autodetected) with a native option, a bind nested under it, the none form
	# with _netdev, an xfs (the type named) and a btrfs (autodetected) by UUID
	# (the kernel mounts them: the image has no xfsprogs or btrfs-progs), a
	# nofail mount of a device that is not there, and (noauto) one that fails to
	# start.
	cat >/etc/fstab <<EOF
UUID=$UUID /data dcfs noatime,dcfs.cache_db=$CACHE/data.db,dcfs.stderrthreshold=0 0 0
$RAW /data/sub dcfs dcfs.fstype=bind,dcfs.ro,dcfs.cache_db=$CACHE/sub.db,dcfs.stderrthreshold=0 0 0
$SRC /mnt/live dcfs dcfs.fstype=none,_netdev,x-systemd.requires-mounts-for=$CACHE,dcfs.cache_db=$CACHE/live.db,dcfs.stderrthreshold=0 0 0
UUID=$XFS_UUID /mnt/xfs dcfs dcfs.fstype=xfs,dcfs.cache_db=$CACHE/xfs.db,dcfs.stderrthreshold=0 0 0
UUID=$BTRFS_UUID /mnt/btrfs dcfs dcfs.cache_db=$CACHE/btrfs.db,dcfs.stderrthreshold=0 0 0
UUID=$MISSING_UUID /mnt/missing dcfs nofail,x-systemd.device-timeout=10min,dcfs.fstype=ext4,dcfs.cache_db=$CACHE/missing.db 0 0
$SRC /mnt/fail dcfs noauto,dcfs.fstype=none,dcfs.bogus,dcfs.cache_db=$CACHE/fail.db 0 0
/srv/rp /rp dcfs noauto,nofail,dcfs.fstype=bind,dcfs.cache_db=$CACHE/rp.db 0 0
/srv/rc /rp/c dcfs noauto,nofail,dcfs.fstype=bind,dcfs.cache_db=$CACHE/rc.db 0 0
EOF
	systemctl daemon-reload
	mnt -a -t dcfs
	mount_a_rc=$MRC
	if [ "$MRC" -eq 0 ] && [ "$(findmnt -rn -t fuse.dcfs | wc -l)" -eq 5 ]; then
		pass mount-a
	else
		fail mount-a "rc=$MRC, $(findmnt -rn -t fuse.dcfs | wc -l) dcfs mounts, out=$(cat "$OUT")"
	fi
	set -- $(fs_of /data)
	if [ "$1" = fuse.dcfs ] && [ "$2" = "$DEV" ] && [ "$(cat /data/file_3.txt 2>&1)" = "content 3" ]; then
		pass fstab-native-uuid-served
	else
		fail fstab-native-uuid-served "type=$1 source=$2"
	fi
	set -- $(fs_of /data/sub)
	if [ "$1" = fuse.dcfs ] && [ "$2" = "$RAW" ] && [ "$(cat /data/sub/raw.txt 2>&1)" = raw ]; then
		pass fstab-bind-nested-served
	else
		fail fstab-bind-nested-served "type=$1 source=$2 $(ls -l /data/sub 2>&1)"
	fi
	touch /data/sub/x 2>"$OUT"
	if grep -q 'Read-only file system' "$OUT"; then
		pass fstab-bind-dcfs-ro
	else
		fail fstab-bind-dcfs-ro "touch said: $(cat "$OUT")"
	fi
	set -- $(fs_of /mnt/live)
	if [ "$1" = fuse.dcfs ] && [ "$2" = "$SRC" ] && [ "$(cat /mnt/live/live.txt 2>&1)" = live ]; then
		pass fstab-none-served
	else
		fail fstab-none-served "type=$1 source=$2"
	fi
	set -- $(fs_of /mnt/xfs)
	if [ "$1" = fuse.dcfs ] && [ "$2" = "$DEV_XFS" ] && [ "$(cat /mnt/xfs/xfs.txt 2>&1)" = "xfs content" ]; then
		pass fstab-xfs-served
	else
		fail fstab-xfs-served "type=$1 source=$2 $(ls /mnt/xfs 2>&1)"
	fi
	set -- $(fs_of /mnt/btrfs)
	if [ "$1" = fuse.dcfs ] && [ "$2" = "$DEV_BTRFS" ] && [ "$(cat /mnt/btrfs/btrfs.txt 2>&1)" = "btrfs content" ]; then
		pass fstab-btrfs-served
	else
		fail fstab-btrfs-served "type=$1 source=$2 $(ls /mnt/btrfs 2>&1)"
	fi
	n=$(daemons | wc -l)
	dbs=$(ls "$CACHE"/data.db "$CACHE"/sub.db "$CACHE"/live.db "$CACHE"/xfs.db "$CACHE"/btrfs.db 2>/dev/null | wc -l)
	if [ "$n" -eq 5 ] && [ "$dbs" -eq 5 ]; then
		pass fstab-one-instance-each
	else
		fail fstab-one-instance-each "$n daemons, $dbs cache databases: $(daemons)"
	fi
	# Not mounted: the missing device (nofail) -- it was not an error.
	if [ "$mount_a_rc" -eq 0 ] && [ "$(mount_count /data)" -eq 1 ] && [ "$(mount_count /mnt/missing)" -eq 0 ]; then
		pass fstab-nofail-missing-skipped
	else
		fail fstab-nofail-missing-skipped "mount -a rc=$mount_a_rc, /data mounted ($(mount_count /data)), /mnt/missing mounted ($(mount_count /mnt/missing))"
	fi
	umount /data/sub /mnt/live /mnt/xfs /mnt/btrfs /data 2>"$OUT"
	urc=$?
	if [ "$urc" -eq 0 ] && [ -z "$(daemons)" ] && [ "$(findmnt -rn -t fuse.dcfs | wc -l)" -eq 0 ]; then
		pass fstab-umount-leaves-nothing
	else
		fail fstab-umount-leaves-nothing "rc=$urc $(cat "$OUT") $(findmnt -rn -t fuse.dcfs) $(daemons)"
		cleanup_mount /data/sub
		cleanup_mount /mnt/live
		cleanup_mount /mnt/xfs
		cleanup_mount /mnt/btrfs
		cleanup_mount /data
	fi

	# --- the README's recipes, with util-linux ---
	# `mount /data` takes the options of its fstab line (a native one
	# among them: the daemon was started with all of them), and a remount of
	# an fstab mount has libmount merge the line's options into the helper's:
	# the native noatime is ignored with the wrapper's WARNING naming it.
	mnt /data
	pid=$(only_daemon)
	set -- $(fs_of /data)
	if [ "$MRC" -eq 0 ] && [ -n "$pid" ] && [ "$1" = fuse.dcfs ] && [ "$2" = "$DEV" ] &&
		tr '\0' ' ' <"/proc/$pid/cmdline" | grep -q "noatime,dcfs.cache_db=$CACHE/data.db"; then
		pass mount-mountpoint-takes-the-fstab-options
	else
		fail mount-mountpoint-takes-the-fstab-options "rc=$MRC type=$1 source=$2 out=$(cat "$OUT") cmdline=$(tr '\0' ' ' <"/proc/$pid/cmdline" 2>&1)"
	fi
	mnt -o remount,dcfs.ro /data
	if [ "$MRC" -eq 0 ] && grep -q '^W[0-9]' "$OUT" && grep -q 'noatime' "$OUT" &&
		findmnt -n -o OPTIONS -M /data | grep -q -w ro; then
		pass remount-merges-the-fstab-options
	else
		fail remount-merges-the-fstab-options "rc=$MRC out=$(cat "$OUT")"
	fi
	mnt /data/sub
	umount -R /data 2>"$OUT"
	urc=$?
	if [ "$urc" -eq 0 ] && [ "$(findmnt -rn -t fuse.dcfs | wc -l)" -eq 0 ] && [ -z "$(daemons)" ]; then
		pass umount-r-unmounts-the-tree
	else
		fail umount-r-unmounts-the-tree "rc=$urc $(cat "$OUT") $(findmnt -rn -t fuse.dcfs)"
		cleanup_mount /data/sub
		cleanup_mount /data
	fi
	# Restarting one instance without systemd: umount, then mount, at once.
	# umount(8) runs umount.fuse.dcfs, which returns when the daemon has
	# exited, so the new daemon finds the cache database free (step 15.6b).
	mnt -t dcfs -o "dcfs.fstype=none,dcfs.cache_db=$CACHE/again.db" "$SRC" "$MNT"
	umount "$MNT"
	mnt -t dcfs -o "dcfs.fstype=none,dcfs.cache_db=$CACHE/again.db" "$SRC" "$MNT"
	if [ "$MRC" -eq 0 ] && [ "$(cat "$MNT/live.txt" 2>&1)" = live ]; then
		pass umount-then-mount-at-once
	else
		fail umount-then-mount-at-once "rc=$MRC out=$(cat "$OUT")"
	fi
	umount "$MNT"
	# After a SIGKILL the mount is dead (ENOTCONN) until umount -l, and the
	# instance then mounts again.
	mnt -t dcfs -o "dcfs.fstype=none,dcfs.cache_db=$CACHE/kill.db" "$SRC" "$MNT"
	pid=$(only_daemon)
	kill -KILL "$pid"
	wait_exit "$pid"
	dead=$(ls "$MNT" 2>&1)
	umount -l "$MNT" 2>/dev/null
	mnt -t dcfs -o "dcfs.fstype=none,dcfs.cache_db=$CACHE/kill.db" "$SRC" "$MNT"
	if echo "$dead" | grep -q 'Transport endpoint is not connected' && [ "$MRC" -eq 0 ] &&
		[ "$(cat "$MNT/live.txt" 2>&1)" = live ] && [ "$(mount_count "$MNT")" -eq 1 ]; then
		pass sigkill-umount-l-then-mount
	else
		fail sigkill-umount-l-then-mount "dead mount said '$dead'; remount rc=$MRC out=$(cat "$OUT") mounts=$(mount_count "$MNT")"
	fi
	umount "$MNT"

	# --- which helper util-linux runs (dcfs/mount_dcfs.h) ---
	# libmount runs umount.<type>. `umount -c` (what systemd runs) takes the
	# type from mountinfo, fuse.dcfs, and runs umount.fuse.dcfs; a plain
	# `umount PATH` takes it from statfs, which has no subtype, and looks for
	# umount.fuse alone (README.md, "Unmount"). The daemon is gone when the
	# helper returns. With umount.fuse.dcfs not installed, umount.fuse is what
	# runs for either; and `umount -N` (a mount namespace) is passed to the
	# helper, which does not wait: the unmount happens, nothing hangs.
	rm -f /sbin/umount.fuse
	: >/tmp/helper.log
	log_helper umount.fuse.dcfs
	mnt -t dcfs -o "dcfs.fstype=none,dcfs.cache_db=$CACHE/h0.db" "$SRC" "$MNT"
	LIBMOUNT_DEBUG=all umount "$MNT" >/tmp/lm_plain.txt 2>&1
	plain_ran=$(cat /tmp/helper.log)
	plain_looked=$(grep -c '/sbin/umount.fuse  *\.\.\. not found' /tmp/lm_plain.txt)
	wait_no_daemons
	if [ -z "$plain_ran" ] && [ "$plain_looked" -ge 1 ]; then
		pass plain-umount-looks-for-umount-fuse-only
	else
		fail plain-umount-looks-for-umount-fuse-only "helper log: $plain_ran; $(grep -i 'helper\|umount\.' /tmp/lm_plain.txt | cut -c20- | head -n 10)"
	fi
	mnt -t dcfs -o "dcfs.fstype=none,dcfs.cache_db=$CACHE/h1.db" "$SRC" "$MNT"
	umount -c "$MNT" 2>"$OUT"
	urc=$?
	if [ "$urc" -eq 0 ] && [ -z "$(daemons)" ] && grep -q "^umount.fuse.dcfs .*$MNT" /tmp/helper.log; then
		pass umount-c-runs-umount-fuse-dcfs
	else
		fail umount-c-runs-umount-fuse-dcfs "rc=$urc daemons=$(daemons) helper log: $(cat /tmp/helper.log) $(cat "$OUT")"
	fi
	rm -f /sbin/umount.fuse.dcfs
	log_helper umount.fuse
	: >/tmp/helper.log
	mnt -t dcfs -o "dcfs.fstype=none,dcfs.cache_db=$CACHE/h2.db" "$SRC" "$MNT"
	umount "$MNT" 2>"$OUT"
	urc=$?
	if [ "$urc" -eq 0 ] && [ -z "$(daemons)" ] && grep -q "^umount.fuse .*$MNT" /tmp/helper.log; then
		pass umount-fuse-is-the-opt-in-fallback
	else
		fail umount-fuse-is-the-opt-in-fallback "rc=$urc daemons=$(daemons) helper log: $(cat /tmp/helper.log) $(cat "$OUT")"
	fi
	mnt -t dcfs -o "dcfs.fstype=none,dcfs.cache_db=$CACHE/h3.db" "$SRC" "$MNT"
	pid=$(only_daemon)
	: >/tmp/helper.log
	umount -N 1 "$MNT" 2>"$OUT"
	urc=$?
	wait_exit "$pid"
	if [ "$urc" -eq 0 ] && [ "$(mount_count "$MNT")" -eq 0 ]; then
		pass umount-N-is-passed-on-and-unmounts
	else
		fail umount-N-is-passed-on-and-unmounts "rc=$urc mounts=$(mount_count "$MNT") helper log: $(cat /tmp/helper.log) $(cat "$OUT")"
	fi
	restore_helpers

	# --- systemd's mount units, generated from the same fstab ---
	systemctl start data.mount data-sub.mount mnt-live.mount mnt-xfs.mount mnt-btrfs.mount 2>"$OUT"
	src=$?
	units_active=1
	for u in data.mount data-sub.mount mnt-live.mount mnt-xfs.mount mnt-btrfs.mount; do
		[ "$(systemctl is-active $u)" = active ] || units_active=0
	done
	status=$(systemctl status data.mount --no-pager 2>&1)
	if [ "$src" -eq 0 ] && [ "$units_active" -eq 1 ] && echo "$status" | grep -q 'active (mounted)' &&
		echo "$status" | grep -q "What: $DEV" && echo "$status" | grep -q 'Where: /data'; then
		pass systemd-mount-units-start
	else
		fail systemd-mount-units-start "rc=$src $(cat "$OUT") $status"
	fi
	set -- $(fs_of /data)
	if [ "$1" = fuse.dcfs ] && [ "$2" = "$DEV" ] && [ "$(cat /data/file_4.txt 2>&1)" = "content 4" ]; then
		pass systemd-findmnt-spec-as-written
	else
		fail systemd-findmnt-spec-as-written "type=$1 source=$2"
	fi
	# x-systemd.requires-mounts-for= is recorded (the guest has no mount of
	# its own under the cache directory to order after).
	if systemctl show -p RequiresMountsFor mnt-live.mount | grep -q "$CACHE" &&
		[ "$(systemctl is-active mnt-live.mount)" = active ]; then
		pass systemd-requires-mounts-for-accepted
	else
		fail systemd-requires-mounts-for-accepted "$(systemctl show -p RequiresMountsFor mnt-live.mount)"
	fi
	# Mount units under a path require and order after the one above.
	if systemctl show -p Requires -p After data-sub.mount | grep -q 'data.mount' &&
		[ "$(systemctl is-active data-sub.mount)" = active ]; then
		pass systemd-child-requires-parent
	else
		fail systemd-child-requires-parent "$(systemctl show -p Requires -p After data-sub.mount)"
	fi
	# The daemon's log is in the journal, attributed to its mount unit, at the
	# threshold the options set (dcfs.stderrthreshold=0: INFO).
	if journal_has "starting: source=$DEV " -t dcfs _SYSTEMD_UNIT=data.mount &&
		journal_has "starting: source=$RAW" -t dcfs _SYSTEMD_UNIT=data-sub.mount; then
		pass journal-daemon-log-by-unit
	else
		fail journal-daemon-log-by-unit "$(journalctl --no-pager -b -t dcfs -o short-full _SYSTEMD_UNIT=data.mount 2>&1 | tail -n 5; journalctl --no-pager -b -t dcfs 2>&1 | tail -n 8)"
	fi
	if journalctl --no-pager -b -u data.mount -o cat 2>/dev/null | grep -q "starting: source=$DEV "; then
		pass journal-u-unit-shows-daemon
	else
		fail journal-u-unit-shows-daemon "$(journalctl --no-pager -b -u data.mount 2>&1 | tail -n 10)"
	fi
	# Stopping the child leaves the parent; stopping the parent stops what is
	# under it.
	# systemd stops a mount with `umount WHERE -c`, which runs umount.fuse.dcfs.
	: >/tmp/helper.log
	log_helper umount.fuse.dcfs
	systemctl stop data-sub.mount
	if [ "$(systemctl is-active data-sub.mount)" != active ] && [ "$(systemctl is-active data.mount)" = active ] &&
		[ "$(cat /data/file_1.txt 2>&1)" = "content 1" ]; then
		pass systemd-stop-child-leaves-parent
	else
		fail systemd-stop-child-leaves-parent "$(systemctl is-active data-sub.mount data.mount)"
	fi
	if grep -q '^umount.fuse.dcfs /data/sub' /tmp/helper.log; then
		pass systemd-stop-runs-umount-fuse-dcfs
	else
		fail systemd-stop-runs-umount-fuse-dcfs "helper log: $(cat /tmp/helper.log)"
	fi
	restore_helpers
	systemctl start data-sub.mount
	both_up=0
	[ "$(systemctl is-active data.mount)" = active ] && [ "$(systemctl is-active data-sub.mount)" = active ] && both_up=1
	systemctl stop data.mount
	if [ "$both_up" -eq 1 ] && [ "$(systemctl is-active data.mount)" != active ] && [ "$(systemctl is-active data-sub.mount)" != active ] &&
		[ "$(mount_count /data)" -eq 0 ] && [ "$(mount_count /data/sub)" -eq 0 ]; then
		pass systemd-stop-parent-stops-child
	else
		fail systemd-stop-parent-stops-child "$(systemctl is-active data.mount data-sub.mount) $(findmnt -t fuse.dcfs)"
	fi
	systemctl start data.mount data-sub.mount
	# systemctl restart of a parent restarts its child (decision 4: restart
	# one instance with systemctl restart): first on a pair of units of its
	# own, nofail and noauto (nothing requires them, so a restart that failed
	# would not take the machine to emergency.target), then on /data, which
	# local-fs.target requires.
	mkdir -p /srv/rp/c /srv/rc /rp
	echo rc >/srv/rc/rc.txt
	systemctl start rp-c.mount
	if restart_why=$(restart_check parent); then
		pass systemd-restart-parent-restarts-child
	else
		fail systemd-restart-parent-restarts-child "$restart_why"
	fi
	if restart_why=$(restart_check both); then
		pass systemd-restart-two-named-units
	else
		fail systemd-restart-two-named-units "$restart_why"
	fi
	systemctl stop rp-c.mount rp.mount 2>/dev/null
	systemctl restart data.mount data-sub.mount 2>"$OUT"
	rrc=$?
	if [ "$rrc" -eq 0 ] && [ "$(systemctl is-active data.mount)" = active ] &&
		[ "$(systemctl is-active data-sub.mount)" = active ] && [ "$(cat /data/sub/raw.txt 2>&1)" = raw ]; then
		pass systemd-restart-required-mount
	else
		fail systemd-restart-required-mount "rc=$rrc $(systemctl is-active data.mount data-sub.mount) $(cat "$OUT")"
	fi
	# A start that fails marks the unit failed, with dcfs's message in the
	# journal under the unit.
	systemctl start mnt-fail.mount 2>"$OUT"
	frc=$?
	if [ "$frc" -ne 0 ] && [ "$(systemctl is-active mnt-fail.mount)" = failed ] && journal_has 'dcfs.bogus' -u mnt-fail.mount; then
		pass systemd-failed-start-marks-unit-failed
	else
		fail systemd-failed-start-marks-unit-failed "rc=$frc state=$(systemctl is-active mnt-fail.mount) $(journalctl --no-pager -b -u mnt-fail.mount 2>&1 | tail -n 8)"
	fi
	# At the default threshold (WARNING) the INFO narrative stays out of the
	# journal (the fstab lines above set dcfs.stderrthreshold=0), and with the
	# threshold set it is there.
	mnt -t dcfs -o "dcfs.fstype=none,dcfs.cache_db=$CACHE/quiet.db" "$SRC" "$MNT"
	quiet_rc=$MRC
	umount "$MNT"
	mnt -t dcfs -o "dcfs.fstype=none,dcfs.cache_db=$CACHE/loud.db,dcfs.stderrthreshold=0" "$SRC" "$MNT"
	loud_rc=$MRC
	umount "$MNT"
	if [ "$quiet_rc" -eq 0 ] && [ "$loud_rc" -eq 0 ] && journal_has "cache_db=$CACHE/loud.db" -t dcfs &&
		! journalctl --no-pager -b -t dcfs -o cat | grep -q "cache_db=$CACHE/quiet.db"; then
		pass journal-follows-threshold
	else
		fail journal-follows-threshold "mounts rc=$quiet_rc/$loud_rc; $(journalctl --no-pager -b -t dcfs -o cat | grep -c "cache_db=$CACHE/quiet.db") INFO lines of the default-threshold daemon, $(journalctl --no-pager -b -t dcfs -o cat | grep -c "cache_db=$CACHE/loud.db") of the other"
	fi

	# The fstab mounts stay up: the reboot at the end of this boot stops them
	# through systemd, and boot 2 finds them mounted at boot. Walk the trees so
	# the cache is warm for boot 2.
	find /data /mnt/live -exec stat -c %n {} + >/dev/null
	sync
	mkdir -p /var/lib/dcfs-test
	echo "boot1 done" >/var/lib/dcfs-test/boot1
}

# only_daemon_of MOUNTPOINT: the pids of the dcfs daemons in the cgroup of
# the mount unit of MOUNTPOINT.
only_daemon_of() {
	od_unit=$(systemd-escape -p --suffix=mount "$1")
	od_cg=$(systemctl show -p ControlGroup --value "$od_unit")
	[ -n "$od_cg" ] || return 0
	od_all=" $(daemons | tr '\n' ' ') "
	for od_pid in $(cat "/sys/fs/cgroup$od_cg/cgroup.procs" 2>/dev/null); do
		case "$od_all" in *" $od_pid "*) echo "$od_pid" ;; esac
	done
}

# --- boot 2 ---------------------------------------------------------------

boot2() {
	[ -f /var/lib/dcfs-test/boot1 ] || fail boot1-state "boot 1 left no state: the disks were not kept"
	# The fstab mounts were made by systemd at boot, not by this script.
	ok=1
	for pair in "data.mount /data" "data-sub.mount /data/sub" "mnt-live.mount /mnt/live" "mnt-xfs.mount /mnt/xfs" "mnt-btrfs.mount /mnt/btrfs"; do
		set -- $pair
		[ "$(systemctl is-active "$1")" = active ] || ok=0
		[ "$(fs_of "$2" | cut -d' ' -f1)" = fuse.dcfs ] || ok=0
	done
	if [ "$ok" -eq 1 ]; then
		pass reboot-fstab-mounts-came-back
	else
		fail reboot-fstab-mounts-came-back "$(systemctl is-active data.mount data-sub.mount mnt-live.mount mnt-xfs.mount mnt-btrfs.mount) $(findmnt -t fuse.dcfs)"
	fi
	set -- $(fs_of /data)
	if [ "$2" = "$DEV" ] && [ "$(cat /data/file_2.txt 2>&1)" = "content 2" ]; then
		pass reboot-spec-as-written
	else
		fail reboot-spec-as-written "type=$1 source=$2"
	fi
	# In order: the parent became active before the child.
	parent_at=$(systemctl show -p ActiveEnterTimestampMonotonic --value data.mount)
	child_at=$(systemctl show -p ActiveEnterTimestampMonotonic --value data-sub.mount)
	if [ "${parent_at:-0}" -gt 0 ] && [ "${child_at:-0}" -gt "${parent_at:-0}" ]; then
		pass reboot-parent-mounted-before-child
	else
		fail reboot-parent-mounted-before-child "data.mount at $parent_at, data-sub.mount at $child_at"
	fi
	# The reboot stopped every daemon of the previous boot cleanly: each
	# logged "shutdown: clean" and this boot's starts all found it clean. The
	# unit stops through umount.fuse.dcfs, which returns when the daemon has
	# exited, so systemd's last SIGTERM finds nothing left to kill (step 15.6b).
	if rebooted_why=$(reboot_clean_check); then
		pass reboot-every-daemon-shuts-down-cleanly
	else
		fail reboot-every-daemon-shuts-down-cleanly "$rebooted_why"
	fi
	# The cache survived: a warm tree is answered without a read of the
	# backing disk.
	# No drop_caches: a rebooted guest has nothing cached, so the lookups that
	# follow reach dcfs, and what dcfs answers from its database it answers
	# without the disk.
	before=$(sectors_read "${DEV#/dev/}")
	find /data -exec stat -c %n {} + >/dev/null 2>&1
	after=$(sectors_read "${DEV#/dev/}")
	if [ "$after" -eq "$before" ]; then
		pass reboot-cache-survives
	else
		fail reboot-cache-survives "the backing disk was read: $((after - before)) sectors"
	fi
	# nofail on a missing backing did not block the boot: this script, which
	# starts after multi-user.target, began while the mount's start job was
	# still waiting for the device (its timeout is ten minutes, so a slow host
	# does not end the wait first).
	up=$START_UP
	state=$START_MISSING
	job=$(echo "$START_JOBS" | grep 'mnt-missing.mount')
	if [ -n "$job" ] && [ "$state" != active ] && [ "$(mount_count /mnt/missing)" -eq 0 ]; then
		pass nofail-missing-backing-does-not-block-boot
	else
		fail nofail-missing-backing-does-not-block-boot "up $up s, mnt-missing.mount is $state, jobs: $START_JOBS"
	fi
	echo "nofail: up $up s, mnt-missing.mount $state, job: $job"
}

if [ "$BOOT" -eq 1 ]; then
	boot1
else
	boot2
fi
[ "$FAILED" -ne 0 ] && dump_state
exit "$FAILED"
