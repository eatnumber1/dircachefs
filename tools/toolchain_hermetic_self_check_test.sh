#!/bin/bash
# Self-check of //tools:toolchain_hermetic_test (step 26.1 rule): the checker
# must pass a probe of the pinned toolchain and fail each of the host's
# leaks: a host include directory, a host C library file, a host library
# loaded by the compiler, and a configuration file of the host's. The
# fixtures are in tools/toolchain_fixtures/ (probe_*.txt).
set -euo pipefail

main() {
  local check=$1 dir=$2 bad out
  "${check}" "${dir}/probe_good.txt" ||
    { echo "FAIL: the good probe was rejected" >&2; return 1; }
  for bad in include files loaded config; do
    if out=$("${check}" "${dir}/probe_bad_${bad}.txt" 2>&1); then
      echo "FAIL: probe_bad_${bad}.txt was accepted" >&2
      return 1
    fi
    grep -q 'FAIL:' <<<"${out}" ||
      { echo "FAIL: probe_bad_${bad}.txt: no reason given" >&2; return 1; }
  done
}

main "$@"
