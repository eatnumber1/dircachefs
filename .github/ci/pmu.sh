#!/bin/bash
# SPDX-License-Identifier: MIT
# Says whether this runner, and the guests of its tests, see a hardware
# performance counter (a PMU), which decides whether the CPU budgets can count
# instructions instead of CPU time (plan step 26.36; test/qemu/README.md,
# "Instructions instead of CPU time"). Reports, never fails: a runner without
# one is a result. The guest's side is //test/qemu:pmu_test, whose log has the
# "pmu:" lines of `testutil pmu` (guest/pmu.sh); the host's is the kernel's
# event source for the CPU. Both go to the log and to the job summary.
set -uo pipefail
summary=${GITHUB_STEP_SUMMARY:-/dev/null}
sysfs=${DCFS_PMU_SYSFS:-/sys/bus/event_source/devices}
paranoid=${DCFS_PMU_PARANOID:-/proc/sys/kernel/perf_event_paranoid}

if [ -d "$sysfs/cpu" ]; then
	host="host: the kernel has a CPU PMU (perf_event_paranoid $(cat "$paranoid" 2>/dev/null || echo unknown))"
else
	host="host: the kernel has no CPU PMU: a virtual machine without a virtual PMU, or perf events off"
fi

logs=$(readlink -f bazel-testlogs 2>/dev/null)
log="$logs/test/qemu/pmu_test/test.log"
if [ -f "$log" ]; then
	guest=$(grep -a -h '^pmu: ' "$log" | sed 's/^/guest: /')
	[ -n "$guest" ] || guest="guest: pmu_test ran and printed no pmu: line"
else
	guest="guest: //test/qemu:pmu_test did not run (no log)"
fi

echo "pmu: $host"
echo "$guest" | sed 's/^/pmu: /'
{
	echo "### PMU visibility"
	echo
	echo "- $host"
	echo "$guest" | sed 's/^/- /'
} >>"$summary"
