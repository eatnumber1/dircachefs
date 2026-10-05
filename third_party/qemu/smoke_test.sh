#!/bin/sh
# Host-side smoke test for the pinned, minimal QEMU build (see BUILD.bazel
# and README.md). Needs neither root nor kernel control (AGENTS.md allows
# such checks on the host): it never boots a guest, just probes the
# built qemu-system-x86_64 binary's own `--version`, `-device help` and
# `-machine help` output.
#
# Usage: smoke_test.sh <qemu-system-x86_64> <README.md>
set -eu

QEMU=$1
README=$2

WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

fail() {
	echo "FAIL: $*" >&2
	exit 1
}

# --- --version --------------------------------------------------------
out=$("$QEMU" --version 2>&1) || fail "--version exited nonzero: $out"
case "$out" in
*"QEMU emulator version"*) ;;
*) fail "--version didn't print a version string: $out" ;;
esac
echo "PASS: --version: $(echo "$out" | head -1)"

# --- README.md's documented device/machine lists -----------------------
# README.md brackets the authoritative lists with HTML comments so this
# test and the prose can't drift apart silently; a device or machine
# that appears in QEMU's own output but not between the markers fails
# the test until README.md is updated to list (and justify) it.
extract() {
	marker=$1
	sed -n "/<!-- ${marker}-start -->/,/<!-- ${marker}-end -->/p" "$README" |
		sed -n 's/^- `\([^`]*\)`.*/\1/p' | sort
}

documented_devices=$(extract devices)
documented_machines=$(extract machines)

[ -n "$documented_devices" ] || fail "README.md has no documented device list (devices-start/devices-end markers)"
[ -n "$documented_machines" ] || fail "README.md has no documented machine list (machines-start/machines-end markers)"

# --- -device help --------------------------------------------------------
# CPU models are also listed under `-device help` (QEMU treats every CPU
# type as a qdev-instantiable "device"); there are ~200 of them and
# listing each one in README.md would defeat the point of an auditable
# list, so they're excluded by name pattern (every one of them ends in
# "-cpu") rather than individually enumerated.
actual_devices=$("$QEMU" -device help 2>&1 |
	sed -n 's/^name "\([^"]*\)".*/\1/p' |
	grep -v -- '-cpu$' | sort)

[ -n "$actual_devices" ] || fail "-device help produced no devices at all (something is badly wrong)"

echo "$actual_devices" >"$WORK/actual_devices"
echo "$documented_devices" >"$WORK/documented_devices"
extra_devices=$(comm -23 "$WORK/actual_devices" "$WORK/documented_devices")
if [ -n "$extra_devices" ]; then
	fail "-device help lists devices README.md does not document:
$extra_devices"
fi
echo "PASS: -device help lists only documented devices:
$actual_devices"

# --- -machine help --------------------------------------------------------
actual_machines=$("$QEMU" -machine help 2>&1 |
	sed -n 's/^\([a-zA-Z0-9_-]*\) .*/\1/p' |
	grep -v '^Supported$' | sort)

[ -n "$actual_machines" ] || fail "-machine help produced no machines at all"

echo "$actual_machines" >"$WORK/actual_machines"
echo "$documented_machines" >"$WORK/documented_machines"
extra_machines=$(comm -23 "$WORK/actual_machines" "$WORK/documented_machines")
if [ -n "$extra_machines" ]; then
	fail "-machine help lists machines README.md does not document:
$extra_machines"
fi
echo "PASS: -machine help lists only documented machines:
$actual_machines"

echo "PASS: all checks passed"
