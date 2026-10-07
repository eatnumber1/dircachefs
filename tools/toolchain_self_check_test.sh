#!/bin/sh
# Self-check of //tools:toolchain_test (step 26.1 rule): the checker must pass
# the pinned clang's info file and reject a host gcc and another clang version.
set -eu

check=$1
dir=$2

"$check" "$dir/llvm_22.txt" || {
	echo "FAIL: the pinned clang's info file was rejected" >&2
	exit 1
}
for bad in gcc llvm_23; do
	if "$check" "$dir/$bad.txt" 2>/dev/null; then
		echo "FAIL: $bad.txt was accepted" >&2
		exit 1
	fi
done
