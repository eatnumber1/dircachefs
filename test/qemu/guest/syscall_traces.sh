#!/bin/sh
# dcfs step 26.3: golden backing-syscall traces per operation, observed with
# strace on the running daemon (russ: observe reality, not our accounting).
# Each check runs one operation through /mnt under strace_op (strace_lib.sh)
# and diffs the daemon's syscalls on the backing filesystem against a golden
# written below. Every golden is explained by docs/design.md ("Population
# policy", "The write-through protocol", "Writable opens and file
# contents"); the explanation is the comment above it. A golden changes only
# together with a sentence of design.md (test/qemu/README.md, "Syscall
# traces").
#
# Run as /tests/syscall_traces.sh by guest/init when booted with
# dcfs_test=syscall_traces.sh.
FAILED=0
. "$(dirname "$0")/lib.sh"
. "$(dirname "$0")/strace_lib.sh"

DCFS=/bin/dcfs
TESTUTIL=/bin/testutil

MNT=/mnt
LOG1=/tmp/dcfs-1.log
LOG2=/tmp/dcfs-2.log

DAEMON_PID=""
MOUNTED=0

cleanup() {
	rc=$?
	if [ "$rc" -ne 0 ] || [ "$FAILED" -ne 0 ]; then
		echo "--- dcfs stderr (cold run) ---"
		cat "$LOG1" 2>/dev/null
		echo "--- dcfs stderr (warm run) ---"
		cat "$LOG2" 2>/dev/null
	fi
	if [ "$MOUNTED" -eq 1 ]; then
		umount "$MNT" 2>/dev/null || true
	fi
	if [ -n "$DAEMON_PID" ] && kill -0 "$DAEMON_PID" 2>/dev/null; then
		kill "$DAEMON_PID" 2>/dev/null || true
		wait "$DAEMON_PID" 2>/dev/null || true
	fi
}
trap cleanup EXIT

echo "syscall_traces.sh: kernel $(uname -r)"

# stop_daemon: unmounts and stops the daemon.
stop_daemon() {
	kill -TERM "$DAEMON_PID" 2>/dev/null || true
	wait "$DAEMON_PID" 2>/dev/null || true
	DAEMON_PID=""
	if is_mounted "$MNT"; then
		umount "$MNT" 2>/dev/null || true
	fi
	MOUNTED=0
}

# --- the backing trees ----------------------------------------------------
#
# Two source directories on vdb. /src/cold holds one file: ext4 lists a
# directory in hash order (the seed is per filesystem), so a populate of
# more than one child would not have a stable order. /src/t holds what the
# mutations act on; its single populate is not traced.

mount /dev/vdb /src
mkdir /src/cold /src/t
echo x >/src/cold/known
echo x >/src/t/known
echo x >/src/t/chm
echo x >/src/t/victim
echo x >/src/t/rensrc
echo x >/src/t/wr

if ! command -v strace >/dev/null 2>&1 || ! strace -V >/dev/null 2>&1; then
	fail strace-runs "strace does not start in this guest"
	exit 1
fi
pass strace-runs

# --- cold: the first lookup in a directory populates it -------------------
#
# design.md "Population policy": one spin-up per directory, getdents64, then
# per child openat(O_PATH|O_NOFOLLOW), statx, the generation,
# name_to_handle_at, the symlink target and every xattr.

SRC=/src/cold
DB=/cache/cold.db
if ! start_daemon "$LOG1" --sync_interval_sec=1000000; then
	fail cold-mount "daemon did not start"
	exit 1
fi
MOUNTED=1
# Observed, in design.md's order: the directory is opened and its mount id
# read, getdents64 until the empty read, then for the one child openat
# O_PATH, statx, name_to_handle_at, the generation (FS_IOC_GETVERSION wants
# a real descriptor, so the O_PATH one is reopened through /proc/self/fd,
# design.md "File descriptors and handles, never paths" item 5), the xattr
# names (listxattr of the magic link: empty) and the closes. (design.md
# lists the generation before name_to_handle_at; the code does it after.)
strace_op cold-lookup stat "$MNT/known"
strace_golden cold-lookup cold-lookup <<'EOT'
openat(backing)
statx(backing)
getdents64(backing)
getdents64(backing)
openat(backing)
statx(backing)
name_to_handle_at(backing)
openat(procfd)
ioctl(backing)
close(backing)
listxattr(procfd)
close(backing)
close(backing)
EOT
stop_daemon

# --- warm: the same operations are answered from the cache ----------------

SRC=/src/t
DB=/cache/warm.db
if ! start_daemon "$LOG2" --sync_interval_sec=1000000; then
	fail warm-mount "daemon did not start"
	exit 1
fi
MOUNTED=1
ls "$MNT" >/dev/null
drop_caches_quiesced

# Warm: design.md "Population policy", "every lookup and listing in the
# directory is answered from the cache": no backing syscall, with the
# kernel's own caches dropped so the request reaches the daemon.
strace_op warm-lookup stat "$MNT/known"
strace_golden warm-lookup warm-lookup <<'EOT'
EOT

# The kernel's own caches answer: nothing reaches the daemon at all.
# Replies carry an attribute and entry timeout of one hour, so a second stat
# never reaches the daemon (no warm GETATTR check: the kernel answers it).
strace_op cached-stat stat "$MNT/known"
strace_golden cached-stat cached-stat <<'EOT'
EOT

drop_caches_quiesced
# A complete directory is listed from the cache (design.md "Readdir").
strace_op warm-readdir ls "$MNT"
strace_golden warm-readdir warm-readdir <<'EOT'
EOT

# CREATE. Phase 1 makes no backing call. Phase 2: the parent is opened
# ("." of the source root), openat O_CREAT, closed. Phase 3 fills the new
# child like a populate (openat O_PATH ... listxattr) and refreshes the
# parent's attributes (statx). Then, because the create is a writable open
# ("Open and Create reopen the object by handle and register the
# descriptor", "Writable opens"): open_by_handle_at O_RDWR, the identity
# check (statx, generation), BeginWriting's and the open's attribute and
# security.capability reads, and, at the release that follows within the
# quiesce, the final attribute read, the capability read-back and the O_PATH
# descriptor held for the FORGET hook ("mmap after close", held-fd
# workaround). The close of the O_RDWR descriptor is the last line.
strace_op create sh -c ": >$MNT/new"
strace_golden create create <<'EOT'
openat(backing)
openat(backing)
close(backing)
openat(backing)
statx(backing)
name_to_handle_at(backing)
openat(procfd)
ioctl(backing)
close(backing)
listxattr(procfd)
close(backing)
statx(backing)
close(backing)
open_by_handle_at(backing)
statx(backing)
openat(procfd)
ioctl(backing)
close(backing)
statx(backing)
statx(backing)
statx(backing)
statx(backing)
statx(backing)
getxattr(procfd) !ENODATA
openat(procfd)
close(backing)
EOT

# The last FORGET of a file written during the run: design.md "mmap after
# close": the reconciliation's statx goes through the O_PATH descriptor held
# since the last close, and the FORGET gives the descriptor's place back
# (close). Not "nothing" as plan step 26.3 guessed: the held fd is how the
# statx costs no disk read, not a reason for no syscall.
strace_op forget-written sh -c "sync; echo 3 >/proc/sys/vm/drop_caches"
strace_golden forget-written forget-written <<'EOT'
statx(backing)
close(backing)
EOT

# UNLINK. Resolving the child to verify it (open_by_handle_at O_PATH,
# statx, generation: "after verifying the resolved child"), then phase 2:
# parent opened, unlinkat, closed. Phase 3: the parent's attributes are
# refreshed (openat O_PATH, statx, close) and SettleUnlinkedFile reads the
# child's attributes through the descriptor it holds (statx: nlink 0, so
# the row goes) and closes it ("Row lifetime").
strace_op unlink rm "$MNT/victim"
strace_golden unlink unlink <<'EOT'
open_by_handle_at(backing)
statx(backing)
openat(procfd)
ioctl(backing)
close(backing)
openat(backing)
unlinkat(backing)
close(backing)
openat(backing)
statx(backing)
close(backing)
statx(backing)
close(backing)
EOT

# MKDIR: like create without the writable open: phase 2 mkdirat, phase 3
# the populate-style fill of the new directory, then the parent's statx.
strace_op mkdir mkdir "$MNT/newdir"
strace_golden mkdir mkdir <<'EOT'
openat(backing)
mkdirat(backing)
openat(backing)
statx(backing)
name_to_handle_at(backing)
openat(procfd)
ioctl(backing)
close(backing)
listxattr(procfd)
close(backing)
statx(backing)
close(backing)
EOT

# RENAME: both parents opened (the same directory twice), renameat, closed;
# phase 3 refreshes the parent (O_PATH, statx) and the renamed object
# (open_by_handle_at O_PATH, statx, generation, statx), as guarded fills.
strace_op rename mv "$MNT/rensrc" "$MNT/rendst"
strace_golden rename rename <<'EOT'
openat(backing)
openat(backing)
renameat(backing)
close(backing)
close(backing)
openat(backing)
statx(backing)
close(backing)
open_by_handle_at(backing)
statx(backing)
openat(procfd)
ioctl(backing)
close(backing)
statx(backing)
close(backing)
EOT

# WRITE-THROUGH: dd opens for write, writes, closes. With passthrough the
# writes bypass dcfs (no read/write line at all): the trace is the open
# (the handle reopen O_RDWR, BeginWriting's reads) and the last writable
# release (attributes refreshed, security.capability read back, the held
# O_PATH descriptor taken), "Attributes while a file is open for writing".
strace_op write dd if=/dev/zero of="$MNT/wr" bs=4096 count=4 conv=notrunc
strace_golden write write <<'EOT'
open_by_handle_at(backing)
statx(backing)
openat(procfd)
ioctl(backing)
close(backing)
statx(backing)
getxattr(procfd) !ENODATA
statx(backing)
statx(backing)
getxattr(procfd) !ENODATA
openat(procfd)
close(backing)
EOT

# FSYNC of a written file: the backing fsync, then the sync point that
# follows it (syncfs on the mount fd), "Sync points run after the kernel's
# FSYNC", between the open's and the release's reads.
strace_op fsync dd if=/dev/zero of="$MNT/wr" bs=4096 count=1 conv=notrunc,fsync
strace_golden fsync fsync <<'EOT'
open_by_handle_at(backing)
statx(backing)
openat(procfd)
ioctl(backing)
close(backing)
statx(backing)
getxattr(procfd) !ENODATA
statx(backing)
fsync(backing)
syncfs(backing)
statx(backing)
statx(backing)
getxattr(procfd) !ENODATA
close(backing)
EOT

# SETATTR chmod: resolve the inode (handle open O_PATH, statx, generation),
# statx of the mode, phase 2 fchmod through /proc/self/fd (O_PATH rejects
# fchmod: item 5), then the phase-3 refreshes as guarded fills: the
# attributes, then the side-effect xattr that a mode change can alter
# (system.posix_acl_access, "Attributes while ... side-effect xattrs").
strace_op chmod chmod 600 "$MNT/chm"
strace_golden chmod chmod <<'EOT'
open_by_handle_at(backing)
statx(backing)
openat(procfd)
ioctl(backing)
close(backing)
statx(backing)
openat(procfd)
fchmod(backing)
close(backing)
close(backing)
open_by_handle_at(backing)
statx(backing)
openat(procfd)
ioctl(backing)
close(backing)
statx(backing)
close(backing)
open_by_handle_at(backing)
statx(backing)
openat(procfd)
ioctl(backing)
close(backing)
getxattr(procfd) !ENODATA
close(backing)
EOT

require_no_reclaim no-reclaim

exit "$FAILED"
