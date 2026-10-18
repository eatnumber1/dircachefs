#!/bin/sh
# SPDX-License-Identifier: MIT
# Glue for /bin/kernel_facts (test/qemu/kernel_facts.cc), which pins what the
# kernel and dcfs do for O_DIRECT and inotify. It gives the binary a backing
# file system at /src, a directory of that file system dcfs never touches
# (/src/native, for the differential half of its tests) and dcfs mounted from
# /src on /mnt; the checks are in the binary.
FAILED=0
. "$(dirname "$0")/lib.sh"

SRC=/src
MNT=/mnt
DB=/cache/dcfs.db
LOG=/tmp/dcfs.log
DAEMON_PID=""
MOUNTED=0
trap 'daemon_cleanup "$LOG"' EXIT

mount /dev/vdb "$SRC"
mkdir -p "$SRC/native" /cache "$MNT"
start_daemon "$LOG" || exit 1
/bin/kernel_facts || FAILED=1
# The residency checks read the backing's page cache: reclaim would empty it.
require_no_reclaim no-reclaim
exit "$FAILED"
