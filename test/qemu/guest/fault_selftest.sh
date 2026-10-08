#!/bin/sh
# Self-check of guest/fault_lib.sh (step 11.1): each mode of fault_mode does
# what it says to a filesystem on the wrapped disk. A harness that injects
# nothing would make every failure test pass for the wrong reason.
#
# Run as /tests/fault_selftest.sh by guest/init when booted with
# dcfs_test=fault_selftest.sh.
FAILED=0
. "$(dirname "$0")/lib.sh"
. "$(dirname "$0")/fault_lib.sh"

MNT=/mnt/fault
DEV=/dev/vdb
NAME=faultdisk

cleanup() {
	umount "$MNT" 2>/dev/null || umount -l "$MNT" 2>/dev/null || true
	fault_unwrap "$NAME" >/dev/null 2>&1 || true
}
trap cleanup EXIT

echo "fault_selftest.sh: kernel $(uname -r), dmsetup $("$DMSETUP" --version | head -1)"
require_commands dd sync umount
mkdir -p "$MNT"

if fault_wrap "$NAME" "$DEV"; then
	pass wrap
else
	fail wrap "dmsetup create failed"
	exit "$FAILED"
fi
FDEV=$(fault_dev "$NAME")
if [ -b "$FDEV" ]; then
	pass wrap-node
else
	fail wrap-node "$FDEV is not a block device"
	exit "$FAILED"
fi

# The filesystem run-qemu.sh made on the disk, now through the wrapper:
# healthy, a write is durable.
mount "$FDEV" "$MNT" || {
	fail mount "cannot mount $FDEV"
	exit "$FAILED"
}
echo one >"$MNT/healthy"
if sync && [ "$(cat "$MNT/healthy")" = one ]; then
	pass healthy-write
else
	fail healthy-write "sync or read failed on a healthy device"
fi

# --- error-writes: a write that reaches the device fails with EIO --------

fault_mode "$NAME" error-writes || fail mode-error-writes "fault_mode failed"
dd if=/dev/zero of="$MNT/ew" bs=4096 count=16 conv=fsync 2>/tmp/ew.err
rc=$?
if [ "$rc" -ne 0 ]; then
	pass error-writes-fsync-fails
else
	fail error-writes-fsync-fails "dd conv=fsync succeeded on an error_writes device"
fi
# Once the journal's own commit meets the error (a metadata change, then
# sync) the filesystem is aborted, whatever the mount's errors= policy: a
# creation after that fails. (sync itself reports nothing.)
echo two >"$MNT/trigger" 2>/dev/null || true
sync
sleep 1
if touch "$MNT/after" 2>/dev/null; then
	fail error-writes-fs-failed "the filesystem still accepts a creation after the journal met the error"
else
	pass error-writes-fs-failed
fi
umount "$MNT" 2>/dev/null || umount -l "$MNT" 2>/dev/null || true
fault_mode "$NAME" healthy || fail mode-healthy "fault_mode healthy failed"
mount "$FDEV" "$MNT" || fail remount-after-errors "cannot mount after the errors"
if [ "$(cat "$MNT/healthy" 2>&1)" = one ]; then
	pass healthy-again-reads-old-data
else
	fail healthy-again-reads-old-data "healthy lost: $(cat "$MNT/healthy" 2>&1)"
fi
if echo three >"$MNT/healed" && sync && [ "$(cat "$MNT/healed")" = three ]; then
	pass healthy-again-writes
else
	fail healthy-again-writes "a write after healing failed"
fi

# --- drop-writes: writes complete, and are gone after a remount ----------

fault_mode "$NAME" drop-writes || fail mode-drop-writes "fault_mode failed"
echo lost >"$MNT/dropped"
if sync; then
	pass drop-writes-sync-succeeds
else
	fail drop-writes-sync-succeeds "sync failed on a drop_writes device (it must not)"
fi
umount "$MNT" 2>/dev/null || umount -l "$MNT" 2>/dev/null || true
fault_mode "$NAME" healthy || fail mode-healthy-2 "fault_mode healthy failed"
mount "$FDEV" "$MNT" || fail remount-after-drop "cannot mount after dropped writes"
if [ ! -e "$MNT/dropped" ]; then
	pass drop-writes-lost
else
	fail drop-writes-lost "a write made while dropping survived"
fi
if [ "$(cat "$MNT/healed" 2>&1)" = three ]; then
	pass drop-writes-keeps-earlier
else
	fail drop-writes-keeps-earlier "healed: $(cat "$MNT/healed" 2>&1)"
fi

# --- error-reads: a read that reaches the device fails -------------------

drop_caches
fault_mode "$NAME" error-reads || fail mode-error-reads "fault_mode failed"
drop_caches
if cat "$MNT/healed" >/dev/null 2>&1; then
	fail error-reads-fail "a read after dropping the caches succeeded"
else
	pass error-reads-fail
fi
fault_mode "$NAME" healthy || fail mode-healthy-3 "fault_mode healthy failed"
if [ "$(cat "$MNT/healed" 2>&1)" = three ]; then
	pass error-reads-recovers
else
	fail error-reads-recovers "healed: $(cat "$MNT/healed" 2>&1)"
fi

# --- window: reads work, then fail for a while, then work again -----------

# Up for 4 s from the switch, down for the next 4, up again: a read right
# after the switch works, one in the middle of the window fails, one after it
# works. The caches are dropped before each, so that each reaches the device.
drop_caches
fault_window "$NAME" 4 4 || fail mode-window "fault_window failed"
echo 3 >/proc/sys/vm/drop_caches
if [ "$(cat "$MNT/healed" 2>&1)" = three ]; then
	pass window-works-before
else
	fail window-works-before "a read in the up interval failed: $(cat "$MNT/healed" 2>&1)"
fi
sleep_until_uptime() {
	su_n=$(awk -v t="$1" -v n="$(cut -d' ' -f1 /proc/uptime)" 'BEGIN { d = t - n; if (d < 0) d = 0; print d }')
	sleep "$su_n"
}
sleep_until_uptime "$(awk -v s="$FAULT_WINDOW_START" 'BEGIN { print s + 5 }')"
echo 3 >/proc/sys/vm/drop_caches
if cat "$MNT/healed" >/dev/null 2>&1; then
	fail window-fails-inside "a read inside the down interval succeeded"
else
	pass window-fails-inside
fi
sleep_until_uptime "$(awk -v s="$FAULT_WINDOW_START" 'BEGIN { print s + 9.5 }')"
echo 3 >/proc/sys/vm/drop_caches
if [ "$(cat "$MNT/healed" 2>&1)" = three ]; then
	pass window-works-after
else
	fail window-works-after "a read after the window failed: $(cat "$MNT/healed" 2>&1)"
fi
fault_mode "$NAME" healthy || fail mode-healthy-window "fault_mode healthy failed"

# --- dead: nothing works, and the table switches back --------------------

drop_caches
fault_mode "$NAME" dead || fail mode-dead "fault_mode failed"
drop_caches
if cat "$MNT/healed" >/dev/null 2>&1; then
	fail dead-reads-fail "a read from a dead device succeeded"
else
	pass dead-reads-fail
fi
fault_mode "$NAME" healthy || fail mode-healthy-4 "fault_mode healthy failed"
if [ "$(cat "$MNT/healed" 2>&1)" = three ]; then
	pass dead-recovers
else
	fail dead-recovers "healed: $(cat "$MNT/healed" 2>&1)"
fi

# --- error-io: both directions ------------------------------------------

drop_caches
fault_mode "$NAME" error-io || fail mode-error-io "fault_mode failed"
drop_caches
if cat "$MNT/healed" >/dev/null 2>&1; then
	fail error-io-reads-fail "a read succeeded on an error-io device"
else
	pass error-io-reads-fail
fi
fault_mode "$NAME" healthy || fail mode-healthy-5 "fault_mode healthy failed"

umount "$MNT" 2>/dev/null || true
if fault_unwrap "$NAME"; then
	pass unwrap
else
	fail unwrap "dmsetup remove failed"
fi
require_no_reclaim no-reclaim
exit "$FAILED"
