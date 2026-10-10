#!/bin/bash
# Self-check of .github/ci/collect-logs.sh (plan step 26.22): over a tree
# shaped like bazel-testlogs, where Bazel leaves the test outputs read-only
# (files 0444 in directories 0555), every log, result and console file is
# copied, not only the first of each directory. The CI coverage job lost 157
# files of its artifact to `cp --parents` refusing the second file of a
# read-only directory.
set -euo pipefail

script=$(realpath "$1")

work=$(mktemp -d)
cleanup() {
  chmod -R u+w "${work}"
  rm -rf "${work}"
}
trap cleanup EXIT

logs=${work}/real-testlogs
outputs=${logs}/dcfs/a_test/test.outputs
mkdir -p "${outputs}"
echo log >"${logs}/dcfs/a_test/test.log"
echo xml >"${logs}/dcfs/a_test/test.xml"
echo serial >"${outputs}/serial.log"
echo coverage >"${outputs}/qemu-__dcfs_a_test.dat"
echo other >"${outputs}/qemu-__dcfs_a_test-boot2.dat"
chmod 0444 "${logs}/dcfs/a_test/test.log" "${logs}/dcfs/a_test/test.xml" "${outputs}"/*
chmod 0555 "${outputs}"

mkdir "${work}/checkout"
ln -s "${logs}" "${work}/checkout/bazel-testlogs"
cd "${work}/checkout"
"${script}" label >/dev/null

for file in test.log test.xml test.outputs/serial.log \
  test.outputs/qemu-__dcfs_a_test.dat test.outputs/qemu-__dcfs_a_test-boot2.dat; do
  if [ ! -f "ci-test-logs/label/dcfs/a_test/${file}" ]; then
    echo "FAIL: ci-test-logs/label/dcfs/a_test/${file} was not copied" >&2
    exit 1
  fi
done
echo "collect_logs_test: ok"
