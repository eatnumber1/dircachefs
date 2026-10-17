#!/bin/bash
# SPDX-License-Identifier: MIT
# Self-check of .github/ci/pmu.sh (plan step 26.36): over a tree shaped like
# bazel-testlogs and a fake sysfs, it reports the guest's "pmu:" lines and
# whether the host has a CPU PMU, in the log and the job summary, and never
# fails, whatever it finds.
set -euo pipefail

script=$(realpath "$1")
work=$(mktemp -d)
trap 'rm -rf "${work}"' EXIT

fail() {
  echo "FAIL: $*" >&2
  exit 1
}

# run LOGS_DIR_NAME: runs the script in a checkout whose bazel-testlogs is that
# tree; output in ${work}/out, the job summary in ${work}/summary.
run() {
  rm -rf "${work}/checkout"
  mkdir "${work}/checkout"
  ln -s "${work}/$1" "${work}/checkout/bazel-testlogs"
  : >"${work}/summary"
  (cd "${work}/checkout" && GITHUB_STEP_SUMMARY="${work}/summary" \
    DCFS_PMU_SYSFS="${work}/sysfs" DCFS_PMU_PARANOID="${work}/paranoid" \
    "${script}") >"${work}/out" 2>&1 || fail "the script failed (rc $?): $(cat "${work}/out")"
}

mkdir -p "${work}/visible/test/qemu/pmu_test" "${work}/absent/test/qemu/pmu_test" \
  "${work}/empty/test/qemu/pmu_test" "${work}/none"
printf 'boot\npmu: visible: 1000010 user and 55000 kernel instructions retired by 1000000 loop iterations and 1000 getppid calls (counting)\nTEST pmu-steady PASS\n' \
  >"${work}/visible/test/qemu/pmu_test/test.log"
printf 'pmu: not visible: perf_event_open of the instructions counter: ENOENT\n' \
  >"${work}/absent/test/qemu/pmu_test/test.log"
printf 'boot only\n' >"${work}/empty/test/qemu/pmu_test/test.log"
echo 2 >"${work}/paranoid"

# A host and a guest that both have one.
mkdir -p "${work}/sysfs/cpu"
run visible
grep -q '^pmu: host: the kernel has a CPU PMU (perf_event_paranoid 2)$' "${work}/out" ||
  fail "no host line: $(cat "${work}/out")"
grep -q '^pmu: guest: pmu: visible: 1000010 user and 55000 kernel' "${work}/out" ||
  fail "no guest line: $(cat "${work}/out")"
grep -q '^- guest: pmu: visible: ' "${work}/summary" || fail "no guest line in the summary: $(cat "${work}/summary")"
grep -q '^### PMU visibility$' "${work}/summary" || fail "no heading in the summary"

# A guest without one, on a host without one.
rm -rf "${work}/sysfs"
mkdir "${work}/sysfs"
run absent
grep -q '^pmu: guest: pmu: not visible: .*ENOENT$' "${work}/out" || fail "no guest absence: $(cat "${work}/out")"
grep -q '^pmu: host: the kernel has no CPU PMU' "${work}/out" || fail "no host absence: $(cat "${work}/out")"

# A test that printed nothing, and one that did not run.
run empty
grep -q 'pmu_test ran and printed no pmu: line' "${work}/out" || fail "empty log not reported: $(cat "${work}/out")"
run none
grep -q 'pmu_test did not run' "${work}/out" || fail "missing log not reported: $(cat "${work}/out")"
echo "pmu_report_test: ok"
