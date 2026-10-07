#!/bin/bash
# Deterministic partition of a suite's test targets across CI shards
# (step 7.4b). Reads `SIZE LABEL` lines on standard input (Bazel's `size`
# attribute and the test's label, one per line, in any order and with
# duplicates) and prints the labels of shard I of N, one per line.
#
# The lines are sorted (small, medium, large, enormous, then by label) and
# dealt out by index modulo N, so every target is in exactly one shard, the
# order is stable for the same input, and each size is spread evenly over
# the shards: the slow tests (large and enormous) are not all in one.
#
# Usage: shard.sh I N < size-label-lines     (0 <= I < N)
set -euo pipefail

if [[ $# -ne 2 ]] || ! [[ $1 =~ ^[0-9]+$ && $2 =~ ^[0-9]+$ ]] ||
  [[ $2 -lt 1 || $1 -ge $2 ]]; then
  echo "usage: $0 I N   (0 <= I < N, N >= 1)" >&2
  exit 2
fi

LC_ALL=C awk '
  BEGIN { rank["small"] = 1; rank["medium"] = 2; rank["large"] = 3; rank["enormous"] = 4 }
  {
    if (!($1 in rank)) { print "shard.sh: unknown size \"" $1 "\" for " $2 > "/dev/stderr"; bad = 1; next }
    print rank[$1] "\t" $2
  }
  END { exit bad }
' | LC_ALL=C sort -u | LC_ALL=C awk -F'\t' -v i="$1" -v n="$2" '(NR - 1) % n == i { print $2 }'
