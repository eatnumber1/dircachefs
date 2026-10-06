#!/bin/sh
# Full benchmark run (large trees, real iteration counts); see bench.sh.
BENCH_ARGS="--entries=100000 --slow_entries=3000 --big=10000 --dirty=10000"
. "$(dirname "$0")/bench.sh"
