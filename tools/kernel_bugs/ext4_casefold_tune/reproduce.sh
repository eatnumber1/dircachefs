#!/bin/sh
# Reproduces a Linux kernel NULL pointer dereference in ext4: after
# EXT4_IOC_SET_TUNE_SB_PARAM (6.18+) turned the casefold feature on under a
# mounted filesystem, listing a directory marked casefold (chattr +F) oopses
# in utf8byte() (ext4fs_dirhash <- ext4_readdir). See README.md.
#
# Usage, as root, on a machine whose kernel may be lost (an oops can leave
# the machine unusable; run it in a VM):
#
#   reproduce.sh DEVICE
#
# DEVICE is a block device or a regular file (mounted through a loop device)
# that holds nothing you want: the script runs mkfs.ext4 on it, without the
# casefold feature, unless MKFS=0 is in the environment (the filesystem is
# then used as it is and must not have the feature either).
#
# Environment: MKFS=0 skips mkfs; HELPER=<path> names casefold_helper (default:
# the one beside this script, built with `cc -static -o casefold_helper
# casefold_helper.c`).
#
# Exit status: 0 the bug did not show (kernel fine, or it refuses the ioctl or
# the flag: also correct); 1 the kernel logged the oops (the last line printed
# is "kernel: <its first line>"); 2 the script could not do its work.
HERE=$(dirname "$0")
HELPER=${HELPER:-$HERE/casefold_helper}
DEVICE=${1:-}
MNT=""

die() {
  echo "reproduce.sh: $*" >&2
  exit 2
}

cleanup() {
  if [ -n "$MNT" ]; then
    # Lazy: a task killed by the oops may still hold the directory.
    umount -l "$MNT" 2>/dev/null
    rmdir "$MNT" 2>/dev/null
  fi
}
trap cleanup EXIT

# oopses: how many oops reports the kernel log holds.
oopses() {
  dmesg | grep -c -E 'BUG: kernel NULL pointer dereference|Oops:'
}

[ "$(id -u)" = 0 ] || die "must run as root"
[ -n "$DEVICE" ] || die "usage: reproduce.sh DEVICE"
[ -b "$DEVICE" ] || [ -f "$DEVICE" ] ||
  die "$DEVICE is not a block device or file"
[ -x "$HELPER" ] ||
  die "no $HELPER (cc -static -o casefold_helper casefold_helper.c)"

echo "kernel version: $(uname -r)"
if [ "${MKFS:-1}" != 0 ]; then
  mkfs.ext4 -q -F "$DEVICE" || die "mkfs.ext4 failed"
fi
MNT=$(mktemp -d) || die "mktemp failed"
OPTS=""
[ -f "$DEVICE" ] && OPTS="-o loop"
# shellcheck disable=SC2086 # OPTS is one option or nothing
mount $OPTS -t ext4 "$DEVICE" "$MNT" || die "mount failed"

mkdir "$MNT/d" || die "mkdir failed"
before=$(oopses)

echo "+ casefold_helper enable $MNT   (EXT4_IOC_SET_TUNE_SB_PARAM, casefold on)"
out=$("$HELPER" enable "$MNT")
rc=$?
if [ "$rc" -ne 0 ]; then
  echo "$out"
  if [ "$rc" -eq 2 ]; then
    echo "the kernel refuses the ioctl: the bug cannot show (a fixed kernel, or" \
      "one older than 6.18)"
    exit 0
  fi
  die "casefold_helper enable failed"
fi

echo "+ casefold_helper mark $MNT/d   (chattr +F $MNT/d)"
out=$("$HELPER" mark "$MNT/d")
rc=$?
if [ "$rc" -ne 0 ]; then
  echo "$out"
  if [ "$rc" -eq 2 ]; then
    echo "the kernel refuses +F: the bug cannot show (a fixed kernel)"
    exit 0
  fi
  die "casefold_helper mark failed"
fi

echo "+ ls $MNT/d   (readdir of the casefold directory)"
# The oops kills ls; the kernel goes on. The group's redirection hides the
# shell's own "Killed" message.
if { ls "$MNT/d" >/dev/null; } 2>/dev/null; then
  echo "the listing works: no oops"
fi

after=$(oopses)
if [ "$after" -gt "$before" ]; then
  echo "--- kernel log, from the oops ---"
  dmesg |
    sed -n '/BUG: kernel NULL pointer dereference/,/end trace/p' |
    head -n 70
  echo "---"
  first=$(dmesg | grep -m 1 -E 'BUG: kernel NULL pointer dereference|Oops:' |
    sed 's/^\[[^]]*\] *//')
  echo "kernel: $first"
  exit 1
fi
echo "no oops in the kernel log ($before before, $after after)"
exit 0
