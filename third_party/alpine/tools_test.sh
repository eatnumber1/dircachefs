#!/bin/bash
# Runs the Alpine host tools (musl binaries) on this glibc host through the
# wrappers third_party/alpine/alpine.bzl writes: QEMU (version, accelerators,
# a KVM start of a paused microvm guest), mke2fs and debugfs (ext4 made with
# the checked-in profile), mkfs.xfs and mkfs.btrfs, and busybox. Versions are
# only checked for their shape: the branch decides them.
#
# Usage: tools_test.sh QEMU MKE2FS DEBUGFS MKFS_XFS MKFS_BTRFS BUSYBOX \
#                      MKE2FS_CONF
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
  check qemu-has-tcg bash -c "'${qemu}' -accel help | grep -qx tcg"
  # TCG starts too (the fallback when a runner has no /dev/kvm).
  check qemu-tcg-start bash -c \
    "echo quit | '${qemu}' -M microvm,accel=tcg -cpu max -S -display none \
      -nodefaults -monitor stdio -m 16"
  # KVM really works, and the machine's data comes from the wrapper's -L.
  if [[ -w /dev/kvm ]]; then
    check qemu-kvm-start bash -c \
      "echo quit | '${qemu}' -M microvm,accel=kvm -cpu host -S -display none \
        -nodefaults -monitor stdio -m 16"
  fi

  # A wrapper that is not next to its tree must not run anything: with an
  # empty root it would start the host's /lib/ld-musl and /usr/bin programs.
  cp "${mke2fs}" "${TMP}/lonely-wrapper"
  check wrapper-without-tree-exits-127 bash -c \
    "'${TMP}/lonely-wrapper' -V 2>&1; [[ \$? -eq 127 ]]"
  check wrapper-without-tree-says-why bash -c \
    "'${TMP}/lonely-wrapper' -V 2>&1 | grep -q 'cannot find root/ next to'"
  # The loader's own search path file is empty: a library missing from the
  # tree is an error, never a host library of /lib or /usr/lib.
  local tree
  tree=$(cd "$(dirname "${mke2fs}")/../root" && pwd)
  check ld-path-file-is-empty bash -c \
    "[[ -e '${tree}/etc/ld-musl-x86_64.path' && ! -s \
       '${tree}/etc/ld-musl-x86_64.path' ]]"

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
