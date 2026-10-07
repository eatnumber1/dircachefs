#!/bin/bash
# Runs the Alpine host tools (musl binaries) on this glibc host through the
# wrappers third_party/alpine/alpine.bzl writes: QEMU (version, accelerators,
# a KVM start of a paused microvm guest), mke2fs and debugfs (ext4 made with
# the checked-in profile), mkfs.xfs and mkfs.btrfs, and busybox. Versions are
# only checked for their shape: the branch decides them.
#
# Usage: tools_test.sh QEMU MKE2FS DEBUGFS MKFS_XFS MKFS_BTRFS BUSYBOX MKE2FS_CONF
set -euo pipefail

FAILED=0

check() {
  local name=$1
  shift
  if "$@" >"${TMP}/out" 2>&1; then
    echo "PASS ${name}"
  else
    echo "FAIL ${name}"
    cat "${TMP}/out"
    FAILED=1
  fi
}

main() {
  local qemu=$1 mke2fs=$2 debugfs=$3 mkfs_xfs=$4 mkfs_btrfs=$5 busybox=$6
  local conf
  conf=$(realpath "$7")
  TMP=$(mktemp -d)
  trap 'rm -rf "${TMP}"' EXIT

  check qemu-version bash -c \
    "'${qemu}' --version | grep -Eq '^QEMU emulator version [0-9]+\\.[0-9]+'"
  check qemu-has-kvm bash -c "'${qemu}' -accel help | grep -qx kvm"
  # KVM really works, and the machine's data comes from the wrapper's -L.
  if [[ -w /dev/kvm ]]; then
    check qemu-kvm-start bash -c \
      "echo quit | '${qemu}' -M microvm,accel=kvm -cpu host -S -display none \
        -nodefaults -monitor stdio -m 16"
  fi

  truncate -s 64M "${TMP}/e"
  truncate -s 320M "${TMP}/x"
  truncate -s 128M "${TMP}/b"
  check mke2fs env MKE2FS_CONFIG="${conf}" "${mke2fs}" -q -F -t ext4 "${TMP}/e"
  check debugfs-reads-it bash -c \
    "'${debugfs}' -R 'show_super_stats -h' '${TMP}/e' |
       grep -q 'Filesystem features:.*extent'"
  check mkfs-xfs "${mkfs_xfs}" -q -f "${TMP}/x"
  check mkfs-btrfs "${mkfs_btrfs}" -q -f "${TMP}/b"
  check busybox bash -c \
    "'${busybox}' | head -n 1 | grep -Eq 'BusyBox v[0-9]+\\.[0-9]+'"
  exit "${FAILED}"
}

main "$@"
