#!/bin/bash
# Self-check of the test selection of .github/ci/test.sh (step 17.1): the
# xfstests shards (tagged `xfstests`) have a job of their own, so the shards of
# the full, asan and ubsan jobs (no --tag) must not select them, and the
# xfstests job (--tag=xfstests-<fstype>) must select them. The script prints
# its cquery expressions with --print-queries, so no Bazel runs here.
set -euo pipefail

script=$1

fail() {
  echo "FAIL: $*" >&2
  exit 1
}

# The default selection leaves out tests tagged xfstests (and manual), in
# every size it is asked for.
default=$("${script}" --print-queries --sizes=small,large,enormous)
[[ "$(wc -l <<<"${default}")" -eq 3 ]] || fail "one expression per size expected: ${default}"
while IFS= read -r line; do
  [[ "${line}" == *"except attr(tags, 'manual|xfstests', tests(//...))" ]] ||
    fail "the default selection does not leave out xfstests: ${line}"
  [[ "${line}" != *"attr(tags, 'xfstests"*"tests(//...))) except"* ]] ||
    fail "the default selection selects by tag: ${line}"
done <<<"${default}"
[[ "${default}" == *"attr(size, '^small\$'"* && "${default}" == *"attr(size, '^enormous\$'"* ]] ||
  fail "the sizes are not in the expressions: ${default}"

# --tag selects only that tag, and still leaves out manual tests (the native
# and dev variants of the shards).
for fs in ext4 xfs btrfs; do
  tagged=$("${script}" --print-queries --tag="xfstests-${fs}" --sizes=large)
  [[ "${tagged}" == "attr(size, '^large\$', attr(tags, 'xfstests-${fs}', tests(//...))) except attr(tags, 'manual', tests(//...))" ]] ||
    fail "--tag=xfstests-${fs}: ${tagged}"
done

# The tag name is matched whole by the shell, not taken as a pattern here: the
# same script with another tag selects that one.
other=$("${script}" --print-queries --tag=nfs --sizes=large)
[[ "${other}" == *"attr(tags, 'nfs', tests(//...))"* && "${other}" != *xfstests* ]] ||
  fail "--tag=nfs: ${other}"

echo "PASS"
