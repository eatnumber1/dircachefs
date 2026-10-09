#!/bin/sh
# Step 26.14: the guest's kernel is quiet. guest/init sets the sysctls that
# take away the kernel's spontaneous activity (periodic writeback, reclaim of
# cached dentries and inodes, laptop-mode writeback) before any test runs and
# qemu_test gives a non-stress test one vCPU, so a test's behavior and its
# coverage depend on nothing but the test. This script reads those settings
# back and then shows what they buy: a dirty page is not written back by the
# kernel on its own (nr_dirty in /proc/vmstat stays up for 10 s) and is
# written back by `sync`, the explicit trigger tests use instead.
#
# Run as /tests/quiet_kernel.sh by guest/init (qemu_test quiet_kernel_test, a
# one-vCPU test: the default); the ext4 disk vdb is only a place for a dirty
# page (tmpfs pages are not counted in nr_dirty).
FAILED=0
. "$(dirname "$0")/lib.sh"
. "$(dirname "$0")/kernel_mode_lib.sh"

nr_dirty() { awk '/^nr_dirty / { print $2 }' /proc/vmstat; }

# The quiet mode (guest/kernel_mode_lib.sh): writeback only on sync/fsync/dcfs's
# sync points (no flusher wakeup every 5 s, nothing old enough to be written for
# a day), laptop mode off, vfs_cache_pressure not lowered (drop_caches reaches
# dentries and inodes through the same slab shrinkers, whose object counts the
# setting scales (vfs_pressure_ratio), so a low value makes `echo 3
# >drop_caches` drop almost nothing: checked below), and one vCPU. Under
# DCFS_NOISY=1 (step 26.14e) this fails, by design: the noisy run excludes
# this target (tag quiet-only) and noisy_kernel_test is its inverse.
mode_problems=$(kernel_mode_problems quiet)
if [ -z "$mode_problems" ]; then
	pass kernel-mode-quiet
else
	fail kernel-mode-quiet "$mode_problems"
fi

if ! mount -t ext4 /dev/vdb /src; then
	fail vdb-mount "mount failed"
	exit 1
fi
sync
base=$(nr_dirty)
dd if=/dev/zero of=/src/dirty bs=4096 count=256 2>/dev/null
dirtied=$(nr_dirty)
if [ "$dirtied" -ge $((base + 200)) ]; then
	pass page-dirtied
else
	fail page-dirtied "nr_dirty was $base and is $dirtied after writing 256 pages: the counter does not show the dirty page, so the next check proves nothing"
fi

sleep 10
after=$(nr_dirty)
if [ "$after" -ge $((dirtied - 20)) ]; then
	pass no-writeback-without-sync
else
	fail no-writeback-without-sync "nr_dirty fell from $dirtied to $after in 10 s: the kernel wrote the page back on its own"
fi

sync
synced=$(nr_dirty)
if [ "$synced" -le $((base + 20)) ]; then
	pass sync-writes-back
else
	fail sync-writes-back "nr_dirty is $synced after sync (was $base before the write): sync did not write the page back"
fi

# The explicit trigger for reclaiming dentries and inodes: drop_caches must
# still drop them (the kernel's unused dentries, field 2 of dentry-state).
mkdir /src/many
i=0
while [ "$i" -lt 300 ]; do
	: >"/src/many/f$i"
	i=$((i + 1))
done
ls -l /src/many >/dev/null
unused_before=$(awk '{ print $2 }' /proc/sys/fs/dentry-state)
sync
echo 3 >/proc/sys/vm/drop_caches
unused_after=$(awk '{ print $2 }' /proc/sys/fs/dentry-state)
if [ "$unused_after" -lt $((unused_before / 2)) ]; then
	pass drop-caches-drops-dentries
else
	fail drop-caches-drops-dentries "unused dentries: $unused_before before, $unused_after after drop_caches (want under half)"
fi
umount /src

exit "$FAILED"
