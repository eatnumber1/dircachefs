#!/bin/bash
# Self-check of the crash states guest/sqlite_durability.sh replays (step
# 12.14): the real guest/sqlite_durability_lib.sh epoch_states over a
# synthetic crash_states log-list, so that a write left out of the states
# (as the first ordinary write of every epoch once was: no tears, the WAL
# writeback never torn) fails here, on the host, not silently in the guest.
#
# Usage: sqlite_durability_lib_test.sh <sqlite_durability_lib.sh>
set -euo pipefail

LIB=$(readlink -f "$1")
WORK=$(mktemp -d)
trap 'rm -rf "${WORK}"' EXIT

fail() {
  echo "FAIL: $*" >&2
  exit 1
}

# shellcheck source=/dev/null
. "${LIB}"

# An epoch [10, 16): the FLUSH|FUA journal commit (8 sectors), a mark, a
# 24-sector ordinary write (the WAL writeback: three 4 KiB blocks), an
# 8-sector ordinary write, a FUA write, a 16-sector ordinary write; then
# the next FLUSH at 16 and a write after it.
cat >"${WORK}/entries" <<'ENTRIES'
9 100 8 NONE
10 200 8 FLUSH|FUA
11 0 0 MARK op3
12 300 24 NONE
13 400 8 NONE
14 500 8 FUA
15 600 16 NONE
16 700 8 FLUSH|FUA
17 800 8 NONE
ENTRIES

epoch_states "${WORK}/entries" 10 16 100 1 64 8 >"${WORK}/states"
read -r cov fua ord total taken mode torn <"${WORK}/states"
[[ "${cov} ${fua} ${ord} ${total} ${mode}" == "COV 2 3 6 exhaustive" ]] ||
  fail "coverage line: $(head -n 1 "${WORK}/states")"
echo "PASS: two FUA writes, three ordinary, six proper subsets, all taken"

sed 1d "${WORK}/states" >"${WORK}/lines"
[[ "$(wc -l <"${WORK}/lines")" == "${taken}" ]] ||
  fail "COV says ${taken} states, there are $(wc -l <"${WORK}/lines")"
if grep -q -E ' (9|11|16|17)( |:|$)' "${WORK}/lines"; then
  fail "a state holds a write outside the epoch or the mark: $(grep -E ' (9|11|16|17)( |:|$)' "${WORK}/lines" | head -n 1)"
fi
echo "PASS: every state holds only the epoch's writes"

for cut in 12:0-8 12:0-16 12:16-24 12:8-24 15:0-8 15:8-16; do
  grep -q " ${cut}\$" "${WORK}/lines" ||
    fail "no torn state for ${cut}: the first ordinary write too must be torn"
done
[[ "${torn}" == "$(grep -c '^tear' "${WORK}/lines")" ]] ||
  fail "COV says ${torn} torn states, there are $(grep -c '^tear' "${WORK}/lines")"
if grep -E '^tear' "${WORK}/lines" | grep -q -E ' 13:'; then
  fail "a one-block write was torn"
fi
echo "PASS: every ordinary write of several blocks is torn, the first included"

for want in "fua 10" "fua 10 14" "subset 10 14 12" "subset 10 14 13 15"; do
  grep -q -x "${want}" "${WORK}/lines" || fail "no state '${want}'"
done
echo "PASS: FUA prefixes and subsets with all FUA writes"

epoch_states "${WORK}/entries" 10 16 4 1 2 8 >"${WORK}/sampled"
read -r _ _ _ _ s_taken s_mode s_torn <"${WORK}/sampled"
[[ "${s_mode}" == "sampled" ]] || fail "a share of 4 did not sample: ${s_mode}"
[[ "${s_taken}" -le 4 ]] ||
  fail "a share of 4 took ${s_taken} states (${s_torn} torn)"
grep -q ' 12:' "${WORK}/sampled" ||
  fail "the sampled epoch tore nothing of its first ordinary write"
echo "PASS: a sampled epoch stays within its share and still tears the first write"

# A log with 4096-byte sectors: a 3-sector write is three blocks, torn at
# whole sectors.
sed -e 's/ 24 NONE/ 3 NONE/' -e 's/ 16 NONE/ 2 NONE/' -e 's/ 8 / 1 /' \
  "${WORK}/entries" >"${WORK}/entries4k"
epoch_states "${WORK}/entries4k" 10 16 100 1 64 1 >"${WORK}/states4k"
for cut in 12:0-1 12:2-3 12:1-3 15:0-1 15:1-2; do
  grep -q " ${cut}\$" "${WORK}/states4k" ||
    fail "4 KiB sectors: no torn state for ${cut}"
done
echo "PASS: tears follow the log's sector size"
