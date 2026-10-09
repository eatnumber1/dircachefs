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
# The sampler announces its first sample on stdout: a fifo opened for reading
# and writing here (so that neither end waits for the other), which is read
# below, when the test is done, instead of polling the file.
rm -f /dev/memsampler.first
mkfifo /dev/memsampler.first
exec 3<>/dev/memsampler.first
awk -v out=/dev/memstat -v announce=1 -f /dev/memsampler.awk >&3 &
sampler=$!

sh "/tests/$TEST"
rc=$?

# guest/init's mem_report: the sampler's last line with the reclaim counters,
# the OOM killer's lines and the kernel's failures (lib.sh has the patterns,
# which //test/qemu:run_qemu_verdict_test checks against guest/init's).
. /tests/lib.sh
# A test that ends within the sampler's first sample (it takes a moment to
# start) waits for one, as guest/init's mem_report does: for the line the
# sampler announces.
read -r first_sample <&3
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
# The reboot stops every unit, the dcfs mounts included (each unmount waits for
# its daemon), before the machine restarts: that is long after the console has
# drained the verdict, which guest/init's `reboot -f` has to sleep for.
systemctl --no-block reboot
