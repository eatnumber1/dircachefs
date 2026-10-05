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

# The built binary must be the pinned release (review nit): README.md's
# qemu-version marker must equal MODULE.bazel's pin, and `--version` the
# marker.
pinned=$(sed -n 's/.*<!-- qemu-version: \([0-9][0-9.]*\) -->.*/\1/p' "$README" | head -1)
[ -n "$pinned" ] || fail "README.md has no '<!-- qemu-version: X.Y.Z -->' marker"
case "$out" in
*"QEMU emulator version $pinned"*) ;;
*) fail "--version is not the pinned $pinned: $out" ;;
esac
echo "PASS: --version matches the pinned $pinned"

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

# --- hermetic linking --------------------------------------------------
# The whole point of building QEMU with Bazel is that the result doesn't
# depend on what's installed on the host (or on this specific Bazel
# invocation's own output_base staying around). Two ways that can go
# wrong, both silent until something actually breaks: a NEEDED entry
# resolves to the *host's* copy of a library instead of the Bazel-built
# one (glib/zlib/pcre2 are all fetched and built hermetically -- see
# README.md -- so none of their shared objects should be linked at all),
# or a NEEDED/RUNPATH entry bakes in an absolute path into this
# particular build's Bazel output_base, which breaks in any other
# checkout, in CI, from a shared cache hit, or the moment this
# output_base is cleaned. `readelf -d` reads the ELF dynamic section
# directly (no need to run the binary, unlike `ldd`).
dynsection=$(readelf -d "$QEMU" 2>&1) || fail "readelf -d failed: $dynsection"

needed=$(echo "$dynsection" | sed -n 's/.*(NEEDED).*\[\(.*\)\]/\1/p')
[ -n "$needed" ] || fail "readelf -d found no NEEDED entries at all (something is badly wrong): $dynsection"

bad_needed=$(echo "$needed" | grep -vE '^(libc\.so\.6|libm\.so\.6)$' || true)
if [ -n "$bad_needed" ]; then
	fail "qemu-system-x86_64 is not hermetically linked -- NEEDED entries beyond libc/libm:
$bad_needed"
fi
echo "PASS: only libc.so.6/libm.so.6 in NEEDED:
$needed"

# Under --config=asan/ubsan QEMU must not be instrumented (review M2): the
# NEEDED check above already rejects a shared libasan/libubsan; this also
# names them explicitly and catches a statically linked runtime.
if echo "$dynsection" | grep -qE 'lib(ubsan|asan)'; then
	fail "qemu-system-x86_64 links a sanitizer runtime: $(echo "$dynsection" | grep -E 'lib(ubsan|asan)')"
fi
if readelf --syms "$QEMU" 2>/dev/null | grep -qE '__(ubsan|asan)_'; then
	fail "qemu-system-x86_64 contains sanitizer runtime/instrumentation symbols (__ubsan_*/__asan_*)"
fi
echo "PASS: no sanitizer runtime or instrumentation"

bad_path=$(echo "$dynsection" | grep -E 'RUNPATH|RPATH' || true)
if [ -n "$bad_path" ]; then
	fail "qemu-system-x86_64 has a RUNPATH/RPATH baked in (leaks a build-time path):
$bad_path"
fi
echo "PASS: no RUNPATH/RPATH"

echo "PASS: all checks passed"
