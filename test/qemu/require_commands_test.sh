#!/bin/bash
# Self-check of guest/lib.sh's require_commands gate (step 26.1): sources the
# real lib.sh and calls the real require_commands with one name that is not a
# command, then with names that all are. It must report the missing name in
# the exact `TEST guest-commands FAIL` line and set FAILED=1 (the guest's
# exit status).
#
# Usage: require_commands_test.sh <lib.sh>
set -euo pipefail

LIB=$(readlink -f "$1")
WORK=$(mktemp -d)
trap 'rm -rf "${WORK}"' EXIT

fail() {
  echo "FAIL: $*" >&2
  exit 1
}

mkdir "${WORK}/bin"
printf '#!/bin/sh\nexit 0\n' >"${WORK}/bin/present-a"
cp "${WORK}/bin/present-a" "${WORK}/bin/present-b"
chmod +x "${WORK}/bin/"*

# Runs require_commands NAME... from the real lib.sh in a fresh sh whose PATH
# is only ${WORK}/bin; prints its output and "FAILED=<n>".
run() {
  PATH="${WORK}/bin" /bin/sh -c '
    FAILED=0
    . "$1" >/dev/null 2>&1 || true
    FAILED=0
    shift
    require_commands "$@"
    echo "FAILED=${FAILED}"
  ' sh "${LIB}" "$@"
}

got=$(run present-a no-such-one present-b no-such-two)
want="TEST guest-commands FAIL (not in this guest: no-such-one no-such-two)
FAILED=1"
[[ "${got}" == "${want}" ]] || fail "missing names: got '${got}', want '${want}'"
echo "PASS: missing commands are named and fail the run"

got=$(run present-a present-b)
[[ "${got}" == "FAILED=0" ]] || fail "all present: got '${got}', want FAILED=0"
echo "PASS: present commands are accepted"
