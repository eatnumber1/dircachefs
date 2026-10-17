#!/bin/sh
# SPDX-License-Identifier: MIT
# Write and SetidWrite benchmarks only (step 17.2c): a MiB written in 64 KiB
# writes to the backing filesystem, through dcfs and through dcfs0, with the
# daemons' request log on (--v=2), then the requests the daemons served,
# counted by operation, and those of them that reached the backing
# filesystem. With FUSE passthrough engaged, the requests a write causes are
# the ones the kernel makes around it. See bench.sh and
# docs/plan/notes/passthrough-write-cost-17.2c.md.
BENCH_ARGS="--entries=100 --slow_entries=50 --big=50 --dirty=10 --dcfs_flags=--v=2,--stderrthreshold=0 --benchmark_min_time=100x --benchmark_filter=^(Setid)?Write/(backing|dcfs|dcfs0)$"
after_bench() {
	echo "--- requests served, by operation (the log's reply lines) ---"
	sed -n 's/^.*fuse_ops.cc:[0-9]*\] \([A-Za-z]*\)(.*) -> .*$/\1/p' /tmp/dcfs-bench.log | sort | uniq -c
	echo "--- of those, requests that reached the backing filesystem ---"
	sed -n 's/^.*fuse_ops.cc:[0-9]*\] \([A-Za-z]*\)(.*) reached the backing.*$/\1/p' /tmp/dcfs-bench.log | sort | uniq -c
}
. "$(dirname "$0")/bench.sh"
