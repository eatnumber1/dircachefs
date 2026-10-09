#!/bin/sh
# Step 15.6: what dcfs-test.service (guest/systemd_install.sh) runs in the
# systemd guest: the guest script, then the same report guest/init gives the
# other guests (the MEM line, the kernel's oopses, the verdict), then a reboot.
#
#   systemd_run.sh TEST
#
# Run as PID 1's child inside the Debian image, after multi-user.target. The
# reboot ends QEMU (the microvm has no ACPI and run-qemu.sh passes -no-reboot:
# guest/init's `reboot -f` note), and takes the dcfs instances down the way a
# real reboot does, so the next boot (run-qemu.sh --boots) sees a clean
# shutdown. guest/init's memory sampler cannot go on across the switch of root
# (it lives in the old one): this starts the same program again, from /dev,
# where guest/init left it, and counts reclaim from where guest/init began.
TEST=$1

RECLAIM_BASE=$(cat /dev/dcfs-reclaim-base 2>/dev/null)
export RECLAIM_BASE
rm -f /dev/memstat
awk -v out=/dev/memstat -f /dev/memsampler.awk &
sampler=$!

sh "/tests/$TEST"
rc=$?

# guest/init's mem_report: the sampler's last line with the reclaim counters,
# the OOM killer's lines and the kernel's failures (lib.sh has the patterns,
# which //test/qemu:run_qemu_verdict_test checks against guest/init's).
. /tests/lib.sh
# A test that ends within the sampler's first sample (it takes a moment to
# start) waits for one, as guest/init's mem_report does.
w=0
while [ ! -s /dev/memstat ] && [ "$w" -lt 20 ]; do
	sleep 0.1
	w=$((w + 1))
done
memline=$(cat /dev/memstat 2>/dev/null)
kill "$sampler" 2>/dev/null
if [ -n "$memline" ]; then
	now=$(awk '/^pgscan_(kswapd|direct) / { p += $2 } /^slabs_scanned / { s += $2 } END { print p + 0, s + 0 }' /proc/vmstat)
	echo "$memline $(awk -v now="$now" -v base="$RECLAIM_BASE" 'BEGIN {
		split(now, n, " ")
		split(base, b, " ")
		printf "reclaim_scans=%d slabs_scanned=%d", n[1] - b[1], n[2] - b[2]
	}')"
fi
dmesg_oom_lines
dmesg_kernel_failures

if [ "$rc" -eq 0 ]; then
	echo "ALL-TESTS-PASSED"
else
	echo "TEST-FAILED"
fi
# The console drains slowly (guest/init's finish); the reboot must not cut off
# the verdict.
sleep 0.5
systemctl --no-block reboot
