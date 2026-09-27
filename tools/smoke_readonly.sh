#!/bin/sh
# Read-only smoke test for dcfs.
#
# Mounts a small tree twice-listed: pass 1 exercises the initial backing-
# filesystem population (PopulateDirectory etc.), pass 2 exercises the fully
# cached path -- no content reads yet (Open/Read are still ENOSYS stubs as of
# step 3.2), just metadata: `find -ls`, xattrs, and a readlink. When strace is
# available, it is attached for pass 2 only, and the log is checked for any
# syscall that touched the backing source tree: with a warm cache there
# should be none at all.
#
# Usage: smoke_readonly.sh <dcfs-binary> <source-dir> <mountpoint> <cache-db>
set -eu

if [ "$#" -ne 4 ]; then
  echo "usage: $0 <dcfs-binary> <source-dir> <mountpoint> <cache-db>" >&2
  exit 2
fi

DCFS=$1
SRC=$2
MNT=$3
DB=$4

DAEMON_LOG="$DB.daemon.log"
STRACE_LOG="$DB.strace.log"

DAEMON_PID=""
STRACE_PID=""
MOUNTED=0

is_mounted() {
  if command -v mountpoint >/dev/null 2>&1; then
    mountpoint -q "$MNT"
  else
    grep -qs " $MNT " /proc/mounts
  fi
}

cleanup() {
  set +e
  if [ -n "$STRACE_PID" ]; then
    kill -INT "$STRACE_PID" 2>/dev/null
    wait "$STRACE_PID" 2>/dev/null
    STRACE_PID=""
  fi
  if [ "$MOUNTED" -eq 1 ]; then
    fusermount3 -u "$MNT" 2>/dev/null
    MOUNTED=0
  fi
  if [ -n "$DAEMON_PID" ]; then
    kill "$DAEMON_PID" 2>/dev/null
    wait "$DAEMON_PID" 2>/dev/null
    DAEMON_PID=""
  fi
}
trap cleanup EXIT INT TERM

fail() {
  echo "FAIL: $1"
  exit 1
}

# --- Populate a small source tree, if it's empty ----------------------------

mkdir -p "$SRC"
if [ -z "$(ls -A "$SRC" 2>/dev/null)" ]; then
  mkdir -p "$SRC/a/b/c"
  echo "root file" > "$SRC/root.txt"
  echo "file one" > "$SRC/a/file1.txt"
  echo "file two" > "$SRC/a/b/file2.txt"
  echo "file three" > "$SRC/a/b/c/file3.txt"
  ln -s root.txt "$SRC/a/link_to_root"
  echo "hardlinked" > "$SRC/a/hardlink1"
  ln "$SRC/a/hardlink1" "$SRC/a/hardlink2"
  if command -v setfattr >/dev/null 2>&1; then
    setfattr -n user.test -v hello "$SRC/root.txt" || true
  fi
fi

mkdir -p "$MNT"

# --- Mount -------------------------------------------------------------------

"$DCFS" --source="$SRC" --cache_db="$DB" --foreground=true "$MNT" \
    > "$DAEMON_LOG" 2>&1 &
DAEMON_PID=$!

i=0
while [ "$i" -lt 10 ]; do
  if is_mounted; then
    MOUNTED=1
    break
  fi
  if ! kill -0 "$DAEMON_PID" 2>/dev/null; then
    cat "$DAEMON_LOG" >&2
    fail "daemon exited before mounting"
  fi
  i=$((i + 1))
  sleep 1
done
[ "$MOUNTED" -eq 1 ] || fail "mountpoint did not appear within 10s"

# --- The read-only listing pass, run twice ----------------------------------

run_pass() {
  echo "--- find -ls ---"
  find "$MNT" -ls
  if command -v getfattr >/dev/null 2>&1; then
    echo "--- getfattr -d -R ---"
    getfattr -d -R "$MNT" 2>&1 || true
  fi
  echo "--- readlink ---"
  readlink "$MNT/a/link_to_root"
}

echo "=== pass 1 ==="
PASS1_OUT=$(run_pass) || fail "pass 1 failed"
printf '%s\n' "$PASS1_OUT"
PASS1_INO=$(stat -c %i "$MNT/root.txt") || fail "pass 1 stat failed"

HAVE_STRACE=0
if command -v strace >/dev/null 2>&1; then
  HAVE_STRACE=1
fi

if [ "$HAVE_STRACE" -eq 1 ]; then
  strace -f -y -p "$DAEMON_PID" \
      -e trace=%file,getdents64,ioctl,%desc \
      -o "$STRACE_LOG" &
  STRACE_PID=$!
  # Give strace a moment to actually attach before generating traffic.
  sleep 1
fi

echo "=== pass 2 ==="
PASS2_OUT=$(run_pass) || fail "pass 2 failed"
printf '%s\n' "$PASS2_OUT"
PASS2_INO=$(stat -c %i "$MNT/root.txt") || fail "pass 2 stat failed"

if [ "$HAVE_STRACE" -eq 1 ]; then
  sleep 1
  kill -INT "$STRACE_PID" 2>/dev/null || true
  wait "$STRACE_PID" 2>/dev/null || true
  STRACE_PID=""

  # fstatfs/fstatvfs/statfs are allowed even though they'd show up here: a
  # `df` on the mount is answered cheaply from the mount fd (Statfs ->
  # backing::StatFilesystem), without opening anything under the source
  # tree. This test never actually calls df/statvfs, so in practice no line
  # should match even this exception -- it exists only to document that a
  # real dcfs run is allowed to make those calls without failing the check.
  BAD=$(grep -F "$SRC" "$STRACE_LOG" 2>/dev/null \
      | grep -Ev '\b(statfs|fstatfs|fstatvfs)\b' || true)
  if [ -n "$BAD" ]; then
    echo "$BAD" >&2
    fail "pass 2 touched the backing source tree (see strace log above)"
  fi
  echo "strace: pass 2 made no syscalls on the backing source tree"
else
  echo "strace not available; skipping the zero-backing-syscalls check"
fi

# --- Inode numbers must be stable and equal the backing inode --------------

SRC_INO=$(stat -c %i "$SRC/root.txt")
if [ "$PASS2_INO" != "$PASS1_INO" ]; then
  fail "root.txt's inode changed between passes ($PASS1_INO vs $PASS2_INO)"
fi
if [ "$PASS2_INO" != "$SRC_INO" ]; then
  fail "mount inode ($PASS2_INO) != backing inode ($SRC_INO) for root.txt"
fi

echo "PASS"
