#!/bin/sh
# Readdir benchmark only (Phase 6.2): the READDIR_ENTRIES-entry directory
# (default 10000) listed through the backing filesystem, dcfs and dcfs0,
# with the cache dropped before every listing. Prints the numbers. Run it
# in the default fastbuild and with the optimized sources (the
# --per_file_copt command in test/qemu/README.md's "Benchmarks"; plain
# `-c opt` also rebuilds the kernel and QEMU). See bench.sh.
N=${READDIR_ENTRIES:-10000}
BENCH_ARGS="--entries=$N --slow_entries=500 --big=$N --dirty=200 --benchmark_filter=Readdir/(backing|dcfs|dcfs0)/"
. "$(dirname "$0")/bench.sh"
