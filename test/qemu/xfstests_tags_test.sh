#!/bin/bash
# Self-check of the xfstests tag (step 17.1): .github/ci/test.sh leaves out of
# the full, asan and ubsan shards every test tagged `xfstests`, so a target
# that runs xfstests and lacks the tag would run in those jobs too (and
# twice as much CI as intended). The tags come from the xfstests_test macro
# (xfstests.bzl); this checks that BUILD.bazel starts no xfstests guest script
# any other way (a qemu_test, or a macro of its own), and that the macro is
# used for the shards at all.
#
# Usage: xfstests_tags_test.sh <BUILD.bazel>
set -euo pipefail

build=$1

fail() {
  echo "FAIL: $*" >&2
  exit 1
}

# The rule or macro call each guest_script line belongs to: the last line that
# starts a call at column 0 or in a list comprehension ("[name(").
bad=$(awk '
  /^\[?[a-z_]+\(/ { call = $0; sub(/\(.*/, "", call); sub(/^\[/, "", call) }
  /guest_script *= *"guest\/xfstests/ && call != "xfstests_test" { print NR ": " $0 " (in " call ")" }
' "${build}")
[[ -z "${bad}" ]] || fail "xfstests guest scripts started without xfstests_test:
${bad}"

n=$(awk '/guest_script *= *"guest\/xfstests/ && call == "xfstests_test" { n++ } /^\[?[a-z_]+\(/ { call = $0; sub(/\(.*/, "", call); sub(/^\[/, "", call) } END { print n + 0 }' "${build}")
[[ "${n}" -ge 4 ]] || fail "only ${n} xfstests_test calls with a guest script (shards, slow shards, native shards, dev)"
echo "PASS: ${n} xfstests_test calls, none bypassed"
