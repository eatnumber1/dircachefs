#!/bin/sh
# SPDX-License-Identifier: MIT
# Does this guest see a hardware instruction counter (a virtual PMU)?
# `testutil pmu` opens perf_event_open's instructions counter, runs a
# fixed amount of work and prints what it read; this script says so in the log
# (the "pmu:" lines, which .github/ci/pmu.sh collects into the CI job summary)
# and checks the premise of counting instructions instead of CPU time: the
# same work reads the same count twice, to within 1%. A guest without a PMU
# (the host or its hypervisor shows none, TCG) skips the checks; it is not a
# failure, the probe reports.
FAILED=0
. "$(dirname "$0")/lib.sh"

TESTUTIL=/bin/testutil

first=$("$TESTUTIL" pmu 2>&1)
second=$("$TESTUTIL" pmu 2>&1)
echo "$first"
echo "$second"

# user_count LINE: the user-mode instructions of a "pmu: visible:" line.
user_count() {
  echo "$1" | awk '{ print $3 }'
}

case "$first" in
  "pmu: visible: "*"(counting)")
    pass pmu-counts-instructions
    case "$second" in
      "pmu: visible: "*"(counting)")
        u1=$(user_count "$first")
        u2=$(user_count "$second")
        if [ "$((u1 > u2 ? u1 - u2 : u2 - u1))" -le "$((u1 / 100))" ]; then
          pass pmu-steady
        else
          fail pmu-steady "the same work read $u1 and $u2 user instructions"
        fi
        ;;
      *)
        fail pmu-steady "second probe was not visible: $second"
        ;;
    esac
    ;;
  "pmu: not visible: "*)
    skip pmu-counts-instructions "no hardware counter in this guest"
    skip pmu-steady "no hardware counter in this guest"
    ;;
  *)
    fail pmu-counts-instructions "the probe says: $first"
    ;;
esac

exit "$FAILED"
