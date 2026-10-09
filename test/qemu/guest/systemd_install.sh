#!/bin/sh
# Step 15.6: sets up the released Debian cloud image for the systemd guest.
# guest/init runs it (in the busybox initramfs) with the image's root
# partition mounted at ROOT, just before switch_root; it is not a test.
#
#   systemd_install.sh ROOT TEST
#
# dcfs, the test binaries and /tests are already in ROOT (guest/init's
# install_dcfs_into). This adds what makes the image a test guest:
#
#   - dcfs-test.service, a oneshot unit that runs guest/systemd_run.sh (and
#     through it TEST) once the system is up, with its output on the serial
#     console, where run-qemu.sh reads the verdict;
#   - no way to log in: the getty templates are masked (the nocloud image
#     logs root in on the serial console without a password), root's password
#     stays locked;
#   - nothing that waits for a network or a clock that this guest lacks, or
#     that would run on its own schedule (networkd, resolved, timesyncd, apt's
#     timers), and a status-quiet console;
#   - (first boot only) a machine-id, so the image's first-boot handling (which
#     would prompt on the console) is not entered, and the same id across a
#     reboot, and an empty /etc/fstab: the test writes its own lines.
set -eu

ROOT=$1
TEST=$2
UNITS=$ROOT/etc/systemd/system

mkdir -p "$UNITS/multi-user.target.wants" "$ROOT/etc/systemd/system.conf.d" \
	"$ROOT/etc/systemd/journald.conf.d"

cat >"$UNITS/dcfs-test.service" <<EOF
[Unit]
Description=dcfs systemd guest test ($TEST)
After=multi-user.target

[Service]
Type=oneshot
ExecStart=/bin/sh /tests/systemd_run.sh $TEST
# The output is the serial console, a tty: no pager, no colors.
Environment=SYSTEMD_PAGER=cat SYSTEMD_COLORS=0
StandardInput=null
StandardOutput=tty
StandardError=tty
TTYPath=/dev/console
TimeoutStartSec=infinity

[Install]
WantedBy=multi-user.target
EOF
ln -sf ../dcfs-test.service "$UNITS/multi-user.target.wants/dcfs-test.service"

# A service that hangs while stopping must not hold a reboot for the 90 s
# default: dcfs's stop is its unmount.
cat >"$ROOT/etc/systemd/system.conf.d/dcfs-test.conf" <<EOF
[Manager]
DefaultTimeoutStopSec=15s
ShowStatus=no
EOF
# The journal is kept across the reboot of a --boots run (the image already has
# /var/log/journal), and is rate-unlimited: a test that logs a burst of dcfs
# lines must not lose them.
cat >"$ROOT/etc/systemd/journald.conf.d/dcfs-test.conf" <<EOF
[Journal]
Storage=persistent
RateLimitIntervalSec=0
EOF
mkdir -p "$ROOT/var/log/journal"

for unit in serial-getty@.service getty@.service console-getty.service container-getty@.service \
	systemd-networkd.service systemd-networkd-wait-online.service systemd-networkd.socket \
	systemd-resolved.service systemd-timesyncd.service \
	apt-daily.timer apt-daily-upgrade.timer apt-daily.service apt-daily-upgrade.service \
	e2scrub_all.timer e2scrub_reap.service fstrim.timer man-db.timer dpkg-db-backup.timer \
	systemd-firstboot.service unattended-upgrades.service apt-listchanges.timer apt-listchanges.service; do
	ln -sf /dev/null "$UNITS/$unit"
done
# The wants the image enabled for getty@tty1 would only fail on the mask.
rm -f "$UNITS/getty.target.wants/getty@tty1.service"

# The rest is for the image's first boot only: a reboot (run-qemu.sh --boots)
# keeps what the test left in /etc/fstab, and the machine-id.
if [ ! -e "$ROOT/etc/dcfs-test-installed" ]; then
	# Not the image's "uninitialized" (first boot).
	echo 5e1f0a6f4a7c4b0d9f2b6f9d3c1a8e07 >"$ROOT/etc/machine-id"
	: >"$ROOT/etc/fstab"
	: >"$ROOT/etc/dcfs-test-installed"
fi
