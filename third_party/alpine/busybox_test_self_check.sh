#!/bin/bash
# Self-check of busybox_test.sh (step 26.1): drives the real busybox_test.sh
# with a fake busybox (a script answering --list from a file). With `find`
# missing from the list the real gate must exit non-zero with its "missing
# required applets" message; with the list the real gate itself requires, it
# must get past that check (the fake has no applets, so the feature checks
# after it fail, which is not what is tested here).
#
# Usage: busybox_test_self_check.sh <busybox_test.sh>
set -euo pipefail

GATE=$(readlink -f "$1")
WORK=$(mktemp -d)
trap 'rm -rf "${WORK}"' EXIT

fail() {
  echo "FAIL: $*" >&2
  exit 1
}

# The applets the real gate requires: the lines of its required_applets="..."
# assignment, so this list cannot drift from the gate's.
sed -n '/^required_applets="$/,/^"$/p' "${GATE}" | sed '1d;$d' |
  tr -s ' ' '\n' | grep -v '^$' >"${WORK}/full"
[[ $(wc -l <"${WORK}/full") -gt 50 ]] || fail "could not read required_applets"
grep -vFx find "${WORK}/full" >"${WORK}/no_find"
[[ $(wc -l <"${WORK}/no_find") -eq $(($(wc -l <"${WORK}/full") - 1)) ]] ||
  fail "find is not a required applet any more; pick another one"

cat >"${WORK}/busybox" <<'F'
#!/bin/sh
case "${1:-}" in
--list) cat "$(dirname "$0")/applets" ;;
*) exit 1 ;;
esac
F
chmod +x "${WORK}/busybox"

# Runs the real gate over the fake with the applet list FILE; sets OUT and RC.
run() {
  cp "$1" "${WORK}/applets"
  RC=0
  OUT=$(bash "${GATE}" "${WORK}/busybox" 2>&1) || RC=$?
}

run "${WORK}/no_find"
[[ ${RC} -ne 0 ]] || fail "gate accepted a busybox without find: ${OUT}"
[[ "${OUT}" == *"FAIL: busybox --list is missing required applets: find" ]] ||
  fail "unexpected rejection message: ${OUT}"
echo "PASS: busybox without find is rejected, naming it"

run "${WORK}/full"
[[ "${OUT}" == *"PASS: all required applets present:"* ]] ||
  fail "gate rejected the full applet list: ${OUT}"
echo "PASS: the full applet list is accepted by the applet check"
