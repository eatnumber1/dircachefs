#!/bin/sh
# dcfs phase 10: runs //bench:dcfs_bench (google/benchmark) in the guest and
# prints its results; the verdict is only that every benchmark ran (VM
# timing is too noisy to pass or fail on).
#
# /src is the fast backing (vdb); /slow is a filesystem on vdc seen through
# a device-mapper delay target (every read and write takes SLOW_MS), the
# "high-latency backing". bench_smoke.sh and bench_full.sh set the sizes:
# smoke runs each benchmark for one iteration on tiny trees (small tier),
# full runs the real thing (enormous tier, numbers for the phase notes).
FAILED=0
. "$(dirname "$0")/lib.sh"

BENCH=/bin/dcfs_bench
SLOW_MS=${SLOW_MS:-5}
BENCH_ARGS=${BENCH_ARGS:---smoke --entries=2000 --slow_entries=500 --big=1000 --dirty=200}

cleanup() {
	rc=$?
	if [ "$rc" -ne 0 ] || [ "$FAILED" -ne 0 ]; then
		echo "--- dcfs log ---"
		cat /tmp/dcfs-bench.log 2>/dev/null
	fi
	umount /slow 2>/dev/null || true
	umount /src 2>/dev/null || true
}
trap cleanup EXIT

echo "bench.sh: kernel $(uname -r), args: $BENCH_ARGS, slow backing delay ${SLOW_MS}ms"

mount /dev/vdb /src || {
	fail mount-fast "could not mount /dev/vdb"
	exit 1
}
mkdir -p /slow /mnt /cache
slow_dev=$("$BENCH" dm-delay slow /dev/vdc "$SLOW_MS") || {
	fail dm-delay "could not create the delay device"
	exit 1
}
pass dm-delay
mount "$slow_dev" /slow || {
	fail mount-slow "could not mount $slow_dev"
	exit 1
}

# shellcheck disable=SC2086 # BENCH_ARGS is a word list
if "$BENCH" --src=/src --slow_src=/slow $BENCH_ARGS; then
	pass benchmarks
else
	fail benchmarks "dcfs_bench exited nonzero"
fi

exit "$FAILED"
