#!/bin/sh
# SPDX-License-Identifier: MIT
# Write and SetidWrite benchmarks with the daemons' request log on, then the
# requests served by operation (step 17.2c). See bench.sh.
BENCH_ARGS="--entries=100 --slow_entries=50 --big=50 --dirty=10"
BENCH_ARGS="$BENCH_ARGS --dcfs_flags=--v=2,--stderrthreshold=0"
BENCH_ARGS="$BENCH_ARGS --benchmark_min_time=100x"
BENCH_ARGS="$BENCH_ARGS --benchmark_filter=^(Setid)?Write/(backing|dcfs|dcfs0)$"
LOG=/tmp/dcfs-bench.log
REPLIED='s/^.*fuse_ops.cc:[0-9]*\] \([A-Za-z]*\)(.*) -> .*$/\1/p'
REACHED='s/^.*fuse_ops.cc:[0-9]*\] \([A-Za-z]*\)(.*) reached the .*$/\1/p'
after_bench() {
	echo "--- requests served, by operation ---"
	sed -n "$REPLIED" "$LOG" | sort | uniq -c
	echo "--- of those, requests that reached the backing filesystem ---"
	sed -n "$REACHED" "$LOG" | sort | uniq -c
}
. "$(dirname "$0")/bench.sh"
