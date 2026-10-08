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

# check_sysctl NAME WANT: the sysctl NAME (vm.x, as /proc/sys/vm/x) reads WANT.
check_sysctl() {
	cs_got=$(cat "/proc/sys/$(echo "$1" | tr . /)" 2>&1)
	if [ "$cs_got" = "$2" ]; then
		pass "sysctl-$1"
	else
		fail "sysctl-$1" "reads '$cs_got', want '$2'"
	fi
}

nr_dirty() { awk '/^nr_dirty / { print $2 }' /proc/vmstat; }

# Writeback only on sync/fsync/dcfs's sync points: no flusher wakeup every 5 s,
# and nothing is old enough to be written for a day.
check_sysctl vm.dirty_writeback_centisecs 0
check_sysctl vm.dirty_expire_centisecs 8640000
check_sysctl vm.laptop_mode 0
check_sysctl vm.vfs_cache_pressure 1

cpus=$(grep -c '^processor' /proc/cpuinfo)
if [ "$cpus" = 1 ]; then
	pass one-vcpu
else
	fail one-vcpu "the guest has $cpus vCPUs, want 1 (qemu_test's cpus default)"
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
umount /src

exit "$FAILED"
