#!/bin/bash
# The pinned test tools are the same files whether they are built for the
# plain configuration or with the --config=asan flags (docs/plan/notes/
# build-speed-2026-10-07.md section 1.5; //third_party:exec_file.bzl).
# Arguments: <name> <plain file> <asan-flags file> ... in triples. A tool
# built twice (an instrumented copy per sanitizer configuration) fails the
# comparison, or at least the second file would be a second build of it.
set -euo pipefail

status=0
while [ "$#" -ge 3 ]; do
  name=$1
  plain=$2
  asan=$3
  shift 3
  for f in "$plain" "$asan"; do
    if [ ! -s "$f" ]; then
      echo "FAIL $name: $f is missing or empty" >&2
      status=1
      continue 2
    fi
  done
  h_plain=$(sha256sum "$plain" | cut -d' ' -f1)
  h_asan=$(sha256sum "$asan" | cut -d' ' -f1)
  r_plain=$(readlink -f "$plain")
  r_asan=$(readlink -f "$asan")
  if [ "$h_plain" != "$h_asan" ]; then
    echo "FAIL $name: content differs ($h_plain vs $h_asan): $r_plain vs $r_asan" >&2
    status=1
  elif [ "$r_plain" != "$r_asan" ]; then
    echo "FAIL $name: same content but built twice: $r_plain vs $r_asan" >&2
    status=1
  else
    echo "ok   $name: $h_plain $r_plain"
  fi
done
exit "$status"
