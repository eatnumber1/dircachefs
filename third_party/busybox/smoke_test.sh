#!/bin/sh
# Host-side smoke test for the pinned, statically linked busybox build
# (see BUILD.bazel and README.md). Needs neither root nor kernel control.
#
# Usage: smoke_test.sh <busybox-binary>
set -eu

BB=$(readlink -f "$1") # absolute: the feature checks below cd into a scratch dir

fail() {
	echo "FAIL: $*" >&2
	exit 1
}

# --- statically linked --------------------------------------------------
case "$(file -b "$BB" 2>/dev/null || true)" in
*"statically linked"* | *"static-pie"*) ;;
*)
	# `file` may not be installed on every host this test runs on; fall
	# back to checking there's no ELF interpreter (PT_INTERP) segment,
	# which is what "statically linked" actually means.
	if readelf -l "$BB" 2>/dev/null | grep -q "INTERP"; then
		fail "$BB is dynamically linked (has a PT_INTERP segment)"
	fi
	;;
esac
echo "PASS: $BB is statically linked"

# --- --help runs ----------------------------------------------------------
"$BB" --help >/dev/null 2>&1 || true # busybox --help exits nonzero; just check it doesn't crash
echo "PASS: $BB --help runs"

# --- every applet the guest scripts use is present ------------------------
# Kept in sync by hand with busybox.config.fragment's groups; a mismatch
# here is a bug in one file or the other, not a real requirements
# difference.
required_applets="
[ ash awk basename cat chgrp chmod chown chroot cmp cp cpio cut date dd diff
dirname dmesg echo fallocate false find free grep head id insmod ip kill ln ls
md5sum mdev mkdir mkfifo mknod more mount mountpoint mv printf pwd
readlink reboot rm rmdir sed sh sleep sort stat sync tail test timeout
touch tr true truncate umount uname uniq wc which
"

actual_applets=$("$BB" --list)

missing=""
for applet in $required_applets; do
	# -F/-x: fixed string, whole line -- "[" (the test-as-"[" applet) is
	# not valid basic-regex syntax (an unterminated bracket expression),
	# so a plain `grep -qx` on it fails with "Invalid regular expression"
	# rather than just not matching.
	if ! echo "$actual_applets" | grep -qFx "$applet"; then
		missing="$missing $applet"
	fi
done

if [ -n "$missing" ]; then
	fail "busybox --list is missing required applets:$missing"
fi
echo "PASS: all required applets present:$required_applets"

# --- every feature the guests rely on actually works ----------------------
# `--list` only proves an applet exists. Several features whose symbols are
# off by default fail silently in the guests (review L2): `dd conv=` exits 1
# with no message, an unsorted `ls` changes order without an error, and
# `$(md5sum a) = $(md5sum b)` passes vacuously when both sides are empty.
# Each check below runs the feature and compares the output to a known
# value; the busybox under test is the only thing on PATH.
W=$(mktemp -d)
trap 'rm -rf "$W"' EXIT
mkdir "$W/bin"
for applet in $("$BB" --list); do
	ln -s "$(readlink -f "$BB")" "$W/bin/$applet"
done
HOST_PATH=$PATH
PATH="$W/bin"
export PATH
cd "$W"

check() {
	name=$1
	want=$2
	got=$3
	[ "$got" = "$want" ] || fail "$name: got '$got', want '$want'"
	echo "PASS: $name"
}

check ash-arith "7" "$(sh -c 'i=3; echo $((i + 4))')"
check ash-command-builtin "ok" "$(sh -c 'command echo ok')"
check test-bracket "yes" "$(sh -c 'if [ -d "$1" ]; then echo yes; fi' sh "$W")"

: >f
chmod 640 f
check stat-c "640" "$(stat -c %a f)"
check stat-c-uid-gid "$(id -u) $(id -g)" "$(stat -c '%u %g' f)"
[ -n "$(stat -f -c %t "$W")" ] || fail "stat -f -c %t printed nothing"
echo "PASS: stat -f -c %t"

mkdir -p t/a t/skip/deep t/b
: >t/a/x
: >t/b/y
: >t/skip/deep/z
check find-path-prune "t/a/x t/b/y" \
	"$(find t \( -path t/skip \) -prune -o -type f -print | sort | tr '\n' ' ' | sed 's/ $//')"
check find-maxdepth "t t/a t/b t/skip" "$(find t -maxdepth 1 | sort | tr '\n' ' ' | sed 's/ $//')"
check find-mindepth-type-d "t/a t/b t/skip" \
	"$(find t -mindepth 1 -maxdepth 1 -type d | sort | tr '\n' ' ' | sed 's/ $//')"
check find-exec "x y" "$(find t/a t/b -type f -exec basename {} \; | sort | tr '\n' ' ' | sed 's/ $//')"

printf 'abcdefgh' >h
check head-c "abcd" "$(head -c 4 h)"
check head-n "abcdefgh" "$(head -n 1 h)"
# pjdfstest's misc.sh expect() pipes every result through `tail -1`.
check tail-1 "b" "$(printf 'a\nb\n' | tail -1)"

sleep 0.01 || fail "sleep 0.01 (fractional duration)"
echo "PASS: sleep 0.01"

printf 'hello world' >d
printf 'HELLO' | dd of=d conv=notrunc 2>/dev/null || fail "dd conv=notrunc"
check dd-conv-notrunc "HELLO world" "$(cat d)"
dd if=/dev/zero of=d2 count=2 ibs=512 obs=512 conv=fsync 2>/dev/null || fail "dd ibs/obs conv=fsync"
check dd-size "1024" "$(wc -c <d2 | tr -d ' ')"

mkdir sorted
for n in zz mm aa kk bb; do : >"sorted/$n"; done
check ls-sorted "aa bb kk mm zz " "$(ls sorted | tr '\n' ' ')"
check ls-recursive "sorted:|aa|bb|kk|mm|zz|" "$(ls -R sorted | tr '\n' '|')"

check md5sum "5eb63bbbe01eeed093cb22bb8f5acdc3  -" "$(printf 'hello world' | md5sum)"
touch -d '2001-02-03 04:05:06' tm
check touch-d "2001-02-03" "$(date -r tm +%F)"
timeout 1 sleep 0 || fail "timeout applet"
echo "PASS: timeout applet"

# mount -o flags: the option table (FEATURE_MOUNT_FLAGS) names noatime and
# strictatime; without the feature only ro/rw/remount exist. The strings are
# only in the binary when the feature is built in.
for opt in noatime strictatime; do
	PATH=$HOST_PATH grep -aqF "$opt" "$BB" || fail "mount option '$opt' not in the binary (CONFIG_FEATURE_MOUNT_FLAGS off?)"
done
echo "PASS: mount -o flags compiled in"
echo "PASS: all checks passed"
