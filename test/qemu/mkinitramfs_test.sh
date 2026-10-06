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
# Usage: mkinitramfs_test.sh <mkinitramfs.sh> <busybox>
set -eu

MKINITRAMFS=$(readlink -f "$1")
BUSYBOX=$(readlink -f "$2")
SH=$(command -v sh)
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

fail() {
	echo "FAIL: $*" >&2
	exit 1
}

# A PATH with the tools mkinitramfs.sh uses, and no cpio.
mkdir "$WORK/tools"
for t in mktemp rm mkdir cp ln chmod find gzip ldd awk dirname basename cat sed; do
	p=$(command -v "$t") || fail "host has no $t"
	ln -s "$p" "$WORK/tools/$t"
done
if PATH="$WORK/tools" command -v cpio >/dev/null 2>&1; then
	fail "cpio is still on the restricted PATH"
fi

# Inputs: a non-ELF "binary" (ldd finds nothing to copy), an init, a guest script.
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
echo "PASS: all checks passed"
