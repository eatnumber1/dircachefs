#!/bin/sh
# Reproduces a Linux kernel WARNING in btrfs: when an inode read fails with an
# I/O error and the inode is a DIRECTORY whose inode item has only been
# updated in memory (a delayed inode: here an atime update by readdir), the
# error path (btrfs_read_locked_inode -> iget_failed -> make_bad_inode ->
# btrfs_destroy_inode) trips WARN_ON(inode->csum_bytes) in
# btrfs_destroy_inode(): csum_bytes shares its storage with index_cnt, which
# the delayed-inode path had set to (u64)-1 for the directory, and
# make_bad_inode() has turned the directory into a "regular file". See
# README.md.
#
# Usage, as root, on a machine whose kernel may be lost (a WARNING is not
# fatal, but the device fails on purpose and the filesystem may go read-only;
# run it in a VM):
#
#   reproduce.sh DEVICE
#
# DEVICE is a block device or a regular file (attached to a loop device) of at
# least 320 MiB that holds nothing you want: the script runs mkfs.btrfs on it
# unless MKFS=0 is in the environment (the filesystem is then used as it is).
# The script puts a device-mapper device over it (dm-linear, later dm-flakey:
# the dm_flakey module is loaded if it is a module), makes the metadata large
# enough to span many leaves (6000 files), makes two groups of directories (the
# "listed" ones are read with ls, which updates their atime in memory; the
# "clean" ones are not), drops the kernel's caches, switches the table to one
# that fails every read, asks for all the directories by file handle
# (open_by_handle_at), first the clean ones and then the listed ones, and
# switches back. The WARNING is expected for the listed ones only.
#
# Environment: MKFS=0 skips mkfs; HELPER=<path> names handle_helper (default:
# the one beside this script, built with `cc -static -o handle_helper
# handle_helper.c`); DMSETUP=<path> names dmsetup; FILES=<n> the number of
# padding files (default 6000).
#
# Exit status: 0 the bug did not show; 1 the kernel logged the WARNING (the
# last line printed is "kernel: <its first line>"); 2 the script could not do
# its work.
HERE=$(dirname "$0")
HELPER=${HELPER:-$HERE/handle_helper}
DMSETUP=${DMSETUP:-dmsetup}
FILES=${FILES:-6000}
DEVICE=${1:-}
NAME=kbug_flakey
MNT=""
LOOP=""
DM=0
WORK=""

die() {
  echo "reproduce.sh: $*" >&2
  exit 2
}

cleanup() {
  if [ "$DM" = 1 ]; then
    dm_table linear
    [ -n "$MNT" ] && umount "$MNT" 2>/dev/null
    "$DMSETUP" remove "$NAME" 2>/dev/null
  fi
  [ -n "$MNT" ] && rmdir "$MNT" 2>/dev/null
  [ -n "$LOOP" ] && losetup -d "$LOOP" 2>/dev/null
  [ -n "$WORK" ] && rm -rf "$WORK"
}
trap cleanup EXIT

# warnings: how many btrfs_destroy_inode warnings the kernel log holds.
warnings() {
  dmesg | grep -c 'WARNING: CPU.*btrfs_destroy_inode'
}

# dm_table linear|flakey: switches the device's table, without freezing the
# filesystem. flakey: in its down interval for ever, failing every read.
dm_table() {
  if [ "$1" = flakey ]; then
    table="0 $SECTORS flakey $BASE 0 0 1 1 error_reads"
  else
    table="0 $SECTORS linear $BASE 0"
  fi
  "$DMSETUP" suspend --nolockfs --noudevsync "$NAME" || return 1
  "$DMSETUP" load "$NAME" --table "$table"
  rc=$?
  "$DMSETUP" resume --noudevsync "$NAME" || return 1
  return "$rc"
}

[ "$(id -u)" = 0 ] || die "must run as root"
[ -n "$DEVICE" ] || die "usage: reproduce.sh DEVICE"
[ -b "$DEVICE" ] || [ -f "$DEVICE" ] ||
  die "$DEVICE is not a block device or file"
[ -x "$HELPER" ] ||
  die "no $HELPER (cc -static -o handle_helper handle_helper.c)"

echo "kernel version: $(uname -r)"
BASE=$DEVICE
if [ -f "$DEVICE" ]; then
  LOOP=$(losetup -f --show "$DEVICE") || die "losetup failed"
  BASE=$LOOP
fi
if [ "${MKFS:-1}" != 0 ]; then
  mkfs.btrfs -q -f "$BASE" || die "mkfs.btrfs failed"
fi
modprobe dm_flakey 2>/dev/null
SECTORS=$(blockdev --getsz "$BASE") || die "blockdev failed"
"$DMSETUP" create --noudevsync "$NAME" --table "0 $SECTORS linear $BASE 0" ||
  die "dmsetup create failed (dm_flakey missing?)"
DM=1
MNT=$(mktemp -d) || die "mktemp failed"
WORK=$(mktemp -d) || die "mktemp failed"
mount -t btrfs "/dev/mapper/$NAME" "$MNT" || die "mount failed"

echo "+ $FILES files in $MNT/pad, so that the metadata spans many leaves"
mkdir "$MNT/pad" || die "mkdir failed"
seq 1 "$FILES" | sed "s|^|$MNT/pad/file-with-a-longish-name-to-fill-leaves-|" |
  xargs touch || die "creating the files failed"
: >"$WORK/clean"
: >"$WORK/listed"
for i in 1 2 3 4 5 6; do
  for group in clean listed; do
    mkdir "$MNT/$group$i" || die "mkdir failed"
    echo a >"$MNT/$group$i/a"
    "$HELPER" handle "$MNT/$group$i" >>"$WORK/$group" || die "handle failed"
  done
done
sync
echo "+ ls of the listed directories: relatime updates their atime," \
  "in memory only"
for i in 1 2 3 4 5 6; do
  ls "$MNT/listed$i" >/dev/null
done
echo 3 >/proc/sys/vm/drop_caches
before=$(warnings)

echo "+ the device fails every read from now on (dm-flakey error_reads)"
dm_table flakey || die "switching the dm table failed"
echo "+ open_by_handle_at of the 6 clean directories, none cached"
"$HELPER" open "$MNT" "$WORK/clean" | tail -n 1
clean=$(warnings)
echo "  warnings from the clean directories: $((clean - before))"
echo "+ open_by_handle_at of the 6 listed directories, none cached"
"$HELPER" open "$MNT" "$WORK/listed" | tail -n 1
echo "+ the device works again"
dm_table linear || die "switching the dm table back failed"
sync

after=$(warnings)
echo "  warnings from the listed directories: $((after - clean))"
if [ "$after" -gt "$before" ]; then
  echo "--- kernel log, from the first warning ---"
  dmesg | awk '
    /cut here/ { start = NR }
    /WARNING: CPU.*btrfs_destroy_inode/ && !first { first = start }
    { line[NR] = $0 }
    END {
      for (i = first; first && i <= NR && i < first + 80; i++)
        print line[i]
    }'
  echo "---"
  first=$(dmesg | grep -m 1 'WARNING: CPU.*btrfs_destroy_inode' |
    sed 's/^\[[^]]*\] *//')
  echo "kernel: $first"
  exit 1
fi
echo "no btrfs_destroy_inode warning in the kernel log" \
  "($before before, $after after)"
exit 0
