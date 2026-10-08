#!/bin/sh
# Host-side test of scripts/mkinitramfs.sh (no root, no kernel). Phase 5.2's
# act run found it printing "cpio: not found", exiting 0 and leaving an EMPTY
# initramfs behind (which Bazel then cached). Two checks:
#
#   - the initramfs is built with the pinned busybox's own cpio applet: with
#     every host tool but cpio on PATH, both modes succeed and the archive
#     lists the files that belong in it;
#   - a failing cpio fails the script: with a fake busybox whose cpio exits 1
#     the script exits nonzero and leaves no output file.
#
#   - a dynamically linked binary gets its interpreter and libraries from the
#     toolchain's sysroot (step 7.1b), not from the host: the archive's libc is
#     the sysroot's file, byte for byte.
#
# Usage: mkinitramfs_test.sh <mkinitramfs.sh> <busybox> <llvm-readelf> \
#            <sysroot> <dynamically linked binary>
set -eu

MKINITRAMFS=$(readlink -f "$1")
BUSYBOX=$(readlink -f "$2")
DCFS_READELF=$(readlink -f "$3")
DCFS_SYSROOT=$(readlink -f "$4")
DYNAMIC=$(readlink -f "$5")
export DCFS_READELF DCFS_SYSROOT
SH=$(command -v sh)
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

fail() {
	echo "FAIL: $*" >&2
	exit 1
}

# A PATH with the tools mkinitramfs.sh uses, and no cpio.
mkdir "$WORK/tools"
for t in mktemp rm mkdir cp ln chmod find gzip dirname basename cat sed dd grep; do
	p=$(command -v "$t") || fail "host has no $t"
	ln -s "$p" "$WORK/tools/$t"
done
if PATH="$WORK/tools" command -v cpio >/dev/null 2>&1; then
	fail "cpio is still on the restricted PATH"
fi

# Inputs: a non-ELF "binary" (nothing to copy), an init, a guest script.
: >"$WORK/init"
printf '#!/bin/sh\n' >"$WORK/testbin"
printf '#!/bin/sh\n' >"$WORK/names.sh"

# Fake busybox: its cpio fails.
cat >"$WORK/fake-busybox" <<'E'
#!/bin/sh
case "${1:-}" in
cpio) echo "fake cpio: boom" >&2; exit 1 ;;
esac
exit 0
E
chmod +x "$WORK/fake-busybox"

listing() { # <archive>: the archive's file names
	gzip -dc "$1" | "$BUSYBOX" cpio -t 2>/dev/null
}

want() { # <archive> <name>
	listing "$1" | grep -qx "$2" || fail "$1 does not list $2: $(listing "$1" | tr '\n' ' ')"
}

# Bazel passes the busybox and the output as paths relative to the working
# directory (and the script cds into its work tree): run from $WORK with a
# relative busybox and output path.
run_unit() { # <busybox> <out>
	rm -f "$WORK/busybox"
	cp "$1" "$WORK/busybox" || return 1
	(cd "$WORK" && rm -f rel.cpio.gz && PATH="$WORK/tools" "$SH" "$MKINITRAMFS" --unit \
		rel.cpio.gz ./busybox "$WORK/init" "$WORK/testbin" - "" && mv rel.cpio.gz "$2")
}

run_e2e() { # <busybox> <out>
	rm -f "$WORK/busybox"
	cp "$1" "$WORK/busybox" || return 1
	(cd "$WORK" && rm -f rel.cpio.gz && PATH="$WORK/tools" "$SH" "$MKINITRAMFS" \
		rel.cpio.gz ./busybox "$WORK/testbin" "$WORK/testbin" \
		"$WORK/testbin" "$WORK/init" "$WORK/names.sh" && mv rel.cpio.gz "$2")
}

# --- no host cpio needed ----------------------------------------------------
run_unit "$BUSYBOX" "$WORK/unit.cpio.gz" >"$WORK/unit.out" 2>&1 ||
	fail "--unit with the real busybox and no host cpio: $(cat "$WORK/unit.out")"
for n in init bin/busybox test/run test/args; do want "$WORK/unit.cpio.gz" "$n"; done
echo "PASS: --unit initramfs built without a host cpio"

run_e2e "$BUSYBOX" "$WORK/e2e.cpio.gz" >"$WORK/e2e.out" 2>&1 ||
	fail "e2e with the real busybox and no host cpio: $(cat "$WORK/e2e.out")"
for n in init bin/busybox bin/dcfs bin/testutil tests/names.sh; do want "$WORK/e2e.cpio.gz" "$n"; done
echo "PASS: e2e initramfs built without a host cpio"

# --- a failing cpio fails the script -----------------------------------------
for mode in unit e2e; do
	rm -f "$WORK/bad.cpio.gz"
	rc=0
	"run_$mode" "$WORK/fake-busybox" "$WORK/bad.cpio.gz" >"$WORK/bad.out" 2>&1 || rc=$?
	[ "$rc" -ne 0 ] || fail "$mode: exit 0 although cpio failed: $(cat "$WORK/bad.out")"
	[ ! -s "$WORK/bad.cpio.gz" ] || fail "$mode: an initramfs was left behind after cpio failed"
done
echo "PASS: a failing cpio fails the script and leaves no initramfs"

# --- a dynamic binary's libraries come from the sysroot ----------------------
rm -f "$WORK/dyn.cpio.gz"
(cd "$WORK" && cp "$BUSYBOX" busybox && PATH="$WORK/tools" "$SH" "$MKINITRAMFS" --unit \
	dyn.cpio.gz ./busybox "$WORK/init" "$DYNAMIC" - "") >"$WORK/dyn.out" 2>&1 ||
	fail "--unit with a dynamic binary: $(cat "$WORK/dyn.out")"
want "$WORK/dyn.cpio.gz" lib/x86_64-linux-gnu/libc.so.6
want "$WORK/dyn.cpio.gz" lib64/ld-linux-x86-64.so.2
mkdir "$WORK/dyn"
(cd "$WORK/dyn" && gzip -dc "$WORK/dyn.cpio.gz" | "$BUSYBOX" cpio -id 2>/dev/null)
cmp "$WORK/dyn/lib/x86_64-linux-gnu/libc.so.6" "$DCFS_SYSROOT/lib/x86_64-linux-gnu/libc.so.6" ||
	fail "the archive's libc.so.6 is not the sysroot's"
cmp "$WORK/dyn/lib64/ld-linux-x86-64.so.2" "$DCFS_SYSROOT/lib64/ld-linux-x86-64.so.2" ||
	fail "the archive's interpreter is not the sysroot's"
echo "PASS: a dynamic binary's libc and interpreter are the sysroot's"

# --- a failing readelf is an error, not a static binary -----------------------
printf '#!/bin/sh\necho "fake readelf: boom" >&2\nexit 3\n' >"$WORK/bad-readelf"
chmod +x "$WORK/bad-readelf"
rm -f "$WORK/bre.cpio.gz"
rc=0
(cd "$WORK" && cp "$BUSYBOX" busybox && DCFS_READELF="$WORK/bad-readelf" \
	PATH="$WORK/tools" "$SH" "$MKINITRAMFS" --unit bre.cpio.gz ./busybox \
	"$WORK/init" "$DYNAMIC" - "") >"$WORK/bre.out" 2>&1 || rc=$?
[ "$rc" -ne 0 ] || fail "a failing readelf passed: $(cat "$WORK/bre.out")"
grep -q "readelf" "$WORK/bre.out" || fail "the failure does not name readelf: $(cat "$WORK/bre.out")"
[ ! -s "$WORK/bre.cpio.gz" ] || fail "an initramfs was left behind after readelf failed"
echo "PASS: a failing readelf fails the script"

# --- a library missing from the sysroot is an error ---------------------------
mkdir -p "$WORK/thin/lib/x86_64-linux-gnu" "$WORK/thin/lib64"
ln -s "$DCFS_SYSROOT/lib/x86_64-linux-gnu/libc.so.6" "$WORK/thin/lib/x86_64-linux-gnu/"
ln -s "$DCFS_SYSROOT/lib/x86_64-linux-gnu/ld-linux-x86-64.so.2" \
	"$WORK/thin/lib/x86_64-linux-gnu/"
ln -s ../lib/x86_64-linux-gnu/ld-linux-x86-64.so.2 "$WORK/thin/lib64/"
rm -f "$WORK/thin.cpio.gz"
rc=0
(cd "$WORK" && cp "$BUSYBOX" busybox && DCFS_SYSROOT="$WORK/thin" \
	PATH="$WORK/tools" "$SH" "$MKINITRAMFS" --unit thin.cpio.gz ./busybox \
	"$WORK/init" "$DYNAMIC" - "") >"$WORK/thin.out" 2>&1 || rc=$?
[ "$rc" -ne 0 ] || fail "a missing library passed: $(cat "$WORK/thin.out")"
grep -q "needs libm.so.6, not in the sysroot" "$WORK/thin.out" ||
	fail "the failure does not say what is missing: $(cat "$WORK/thin.out")"
[ ! -s "$WORK/thin.cpio.gz" ] || fail "an initramfs was left behind after a library was missing"
echo "PASS: a library missing from the sysroot fails the script"
echo "PASS: all checks passed"
