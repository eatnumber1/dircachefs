#!/bin/sh
# Step 7.1: the C++ toolchain is the pinned clang from toolchains_llvm.
# $1 is tools/cc_toolchain_info.txt: the toolchain's $(CC) and its
# `--version` output.
set -eu

info=$1
expected_version="23.1.2"

if ! grep -q "^CC=.*llvm_toolchain" "$info"; then
	echo "FAIL: the compiler is not from @llvm_toolchain:" >&2
	cat "$info" >&2
	exit 1
fi
if ! grep -q "clang version $expected_version" "$info"; then
	echo "FAIL: expected clang version $expected_version:" >&2
	cat "$info" >&2
	exit 1
fi
