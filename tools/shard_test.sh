#!/bin/bash
# Self-check of .github/ci/shard.sh (step 7.4b): over a canned target list,
# every target is in exactly one shard, the partition is stable (the same for
# any input order and with duplicates), and each size is spread over the
# shards.
set -euo pipefail

script=$1

input() {
  cat <<'LIST'
large //t:large_d
small //a:small_1
medium //m:medium_1
small //a:small_2
enormous //e:enormous_a
small //a:small_3
large //t:large_a
small //a:small_4
medium //m:medium_2
enormous //e:enormous_b
large //t:large_b
small //a:small_5
medium //m:medium_3
large //t:large_c
enormous //e:enormous_c
LIST
}

fail() {
  echo "FAIL: $*" >&2
  exit 1
}

n=4
all=""
for ((i = 0; i < n; i++)); do
  shard=$(input | "${script}" "$i" "$n")
  [[ -n "${shard}" ]] || fail "shard $i of $n is empty"
  all+="${shard}"$'\n'
  # Each shard has one of the four enormous/large tests at most one more than
  # another shard's: the sizes are dealt out in turn.
  large=$(grep -c ':large_' <<<"${shard}" || true)
  [[ "${large}" -eq 1 ]] || fail "shard $i has ${large} large tests of 4"
done

# Every target exactly once.
expected=$(input | awk '{ print $2 }' | LC_ALL=C sort -u)
got=$(LC_ALL=C sort <<<"${all%$'\n'}")
[[ "${got}" == "${expected}" ]] || fail "the shards are not a partition:
$(diff <(echo "${expected}") <(echo "${got}") || true)"

# Stable: reversed and duplicated input gives the same shards.
for ((i = 0; i < n; i++)); do
  a=$(input | "${script}" "$i" "$n")
  b=$( (input | tac; input) | "${script}" "$i" "$n")
  [[ "${a}" == "${b}" ]] || fail "shard $i differs with the input order"
done

# One shard is everything, in size order.
one=$(input | "${script}" 0 1)
[[ "$(head -n 1 <<<"${one}")" == "//a:small_1" &&
  "$(tail -n 1 <<<"${one}")" == "//e:enormous_c" ]] ||
  fail "a single shard is not size-then-label order: ${one}"

# Misuse is refused, not silently a different partition.
for args in "4 4" "x 3" "0 0" "-1 3" "1"; do
  # shellcheck disable=SC2086 # the words are the point
  if input | "${script}" ${args} >/dev/null 2>&1; then
    fail "accepted the arguments: ${args}"
  fi
done
if printf 'huge //x:y\n' | "${script}" 0 1 >/dev/null 2>&1; then
  fail "accepted an unknown size"
fi
echo "PASS"
