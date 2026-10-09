#!/bin/sh
# dcfs step 6.3 (audit F10): an object that is removed while something
# still refers to it -- a process whose working directory is removed, an
# O_PATH descriptor on a file that is then unlinked -- must behave as it
# does on the backing filesystem, not fail with ESTALE. Step 23.2: that
# includes changing it (truncate, chmod, chown, utimes, xattrs, fsync,
# through an open descriptor, an O_PATH descriptor's /proc/self/fd magic
# link, or a removed working directory) and reopening an unlinked file
# through /proc/self/fd.
#
# The kernel keeps such an object's FUSE nodeid until its last reference
# goes (then it sends FORGET), and keeps asking dcfs about it meanwhile
# (GETATTR for a stat, OPENDIR for an open of "."). The reference results
# are measured on the backing ext4 filesystem itself, before dcfs mounts
# it, so that dcfs's exclusive access is never violated.
#
# Run as /tests/removed.sh by guest/init when booted with
# dcfs_test=removed.sh; prints one "TEST ... PASS/FAIL" line per check and
# exits nonzero if any check failed.
FAILED=0
. "$(dirname "$0")/lib.sh"

DCFS=/bin/dcfs
TESTUTIL=/bin/testutil

SRC=/src
MNT=/mnt
DB=/cache/dcfs.db
LOG=/tmp/dcfs.log

DAEMON_PID=""
MOUNTED=0

cleanup() {
	rc=$?
	if [ "$rc" -ne 0 ] || [ "$FAILED" -ne 0 ]; then
		echo "--- dcfs stderr ---"
		cat "$LOG" 2>/dev/null
	fi
	if [ "$MOUNTED" -eq 1 ]; then
		umount "$MNT" 2>/dev/null || umount -l "$MNT" 2>/dev/null || true
	fi
	if [ -n "$DAEMON_PID" ] && kill -0 "$DAEMON_PID" 2>/dev/null; then
		kill "$DAEMON_PID" 2>/dev/null || true
		wait "$DAEMON_PID" 2>/dev/null || true
	fi
}
trap cleanup EXIT

echo "removed.sh: kernel $(uname -r)"

# expect_same NAME WANT GOT
expect_same() {
	if [ "$2" = "$3" ]; then
		pass "$1"
	else
		fail "$1" "backing filesystem: '$2'; dcfs: '$3'"
	fi
}

mount /dev/vdb /src

# --- what ext4 does ---------------------------------------------------------

mkdir /src/ref_cwd
ref_cwd=$("$TESTUTIL" rmcwd /src/ref_cwd 2>&1)
echo "ext4 removed cwd: $ref_cwd"
echo data >/src/ref_file
ref_file=$("$TESTUTIL" opath-unlink-stat /src/ref_file 2>&1)
echo "ext4 unlinked O_PATH file: $ref_file"
mkdir /src/ref_ls
ref_ls=$(cd /src/ref_ls && rmdir /src/ref_ls && ls -a . 2>&1; echo "rc=$?")
ref_umut=$("$TESTUTIL" unlinked-mutate /src/ref_umut 2>&1)
echo "ext4 changes through an unlinked file's open fd: $ref_umut"
ref_omut=$("$TESTUTIL" opath-unlinked-mutate /src/ref_omut 2>&1)
echo "ext4 changes through an unlinked file's O_PATH fd: $ref_omut"
mkdir /src/ref_cmut
ref_cmut=$("$TESTUTIL" rmcwd-mutate /src/ref_cmut 2>&1)
echo "ext4 changes to a removed cwd: $ref_cmut"
# Step 23.9: linking a removed object back (ENOENT for a file, EPERM for a
# directory, on every Linux filesystem).
echo data >/src/ref_lfile
ref_lfile=$("$TESTUTIL" removed-link /src/ref_lfile 2>&1)
echo "ext4 link of an unlinked file: $ref_lfile"
mkdir /src/ref_ldir
ref_ldir=$("$TESTUTIL" removed-link /src/ref_ldir 2>&1)
echo "ext4 link of a removed directory: $ref_ldir"
mkdir /src/ref_tdir
ref_tlink=$("$TESTUTIL" tmpfile-link /src/ref_tdir t 2>&1)
echo "ext4 link of a closed O_TMPFILE file: $ref_tlink"
sync

# --- the same through dcfs --------------------------------------------------

mkdir -p /cache /mnt
if start_daemon "$LOG"; then
	pass mount
else
	fail mount "daemon did not mount within 10s"
	exit "$FAILED"
fi

mkdir "$MNT/cwd"
expect_same removed-cwd "$ref_cwd" "$("$TESTUTIL" rmcwd "$MNT/cwd" 2>&1)"

echo data >"$MNT/lfile"
expect_same removed-file-link "$ref_lfile" \
	"$("$TESTUTIL" removed-link "$MNT/lfile" 2>&1)"
mkdir "$MNT/ldir"
expect_same removed-dir-link "$ref_ldir" \
	"$("$TESTUTIL" removed-link "$MNT/ldir" 2>&1)"
mkdir "$MNT/tdir"
expect_same closed-tmpfile-link "$ref_tlink" \
	"$("$TESTUTIL" tmpfile-link "$MNT/tdir" t 2>&1)"
if [ "$(stat -c %i "$MNT/tdir/t" 2>&1)" = "$(stat -c %i "$SRC/tdir/t" 2>&1)" ]; then
	pass closed-tmpfile-link-served
else
	fail closed-tmpfile-link-served "dcfs: $(stat -c %i "$MNT/tdir/t" 2>&1); backing: $(stat -c %i "$SRC/tdir/t" 2>&1)"
fi

echo data >"$MNT/file"
expect_same unlinked-opath-file "$ref_file" \
	"$("$TESTUTIL" opath-unlink-stat "$MNT/file" 2>&1)"

mkdir "$MNT/ls"
expect_same removed-cwd-ls "$ref_ls" \
	"$(cd "$MNT/ls" && rmdir "$MNT/ls" && ls -a . 2>&1; echo "rc=$?")"

# A removed directory that was listed through dcfs first (its row and
# listing are cached), and a file that was stat'ed (its attributes are
# cached), so the cached state cannot answer by accident.
mkdir "$MNT/listed"
touch "$MNT/listed/x"
ls -la "$MNT/listed" >/dev/null
rm "$MNT/listed/x"
expect_same removed-listed-cwd "$ref_cwd" \
	"$("$TESTUTIL" rmcwd "$MNT/listed" 2>&1)"
echo data >"$MNT/statted"
stat "$MNT/statted" >/dev/null
expect_same unlinked-statted-file "$ref_file" \
	"$("$TESTUTIL" opath-unlink-stat "$MNT/statted" 2>&1)"

# Step 23.2: changing them. Each also with its attributes cached first, so
# a cached value cannot answer by accident.
expect_same unlinked-open-mutate "$ref_umut" \
	"$("$TESTUTIL" unlinked-mutate "$MNT/umut" 2>&1)"
echo x >"$MNT/umut2"
stat "$MNT/umut2" >/dev/null
expect_same unlinked-open-mutate-statted "$ref_umut" \
	"$("$TESTUTIL" unlinked-mutate "$MNT/umut2" 2>&1)"
expect_same unlinked-opath-mutate "$ref_omut" \
	"$("$TESTUTIL" opath-unlinked-mutate "$MNT/omut" 2>&1)"
mkdir "$MNT/cmut"
expect_same removed-cwd-mutate "$ref_cmut" \
	"$("$TESTUTIL" rmcwd-mutate "$MNT/cmut" 2>&1)"
mkdir "$MNT/cmut2"
ls -la "$MNT/cmut2" >/dev/null
expect_same removed-cwd-mutate-listed "$ref_cmut" \
	"$("$TESTUTIL" rmcwd-mutate "$MNT/cmut2" 2>&1)"

# Once the references are gone the objects are gone for good: the names
# stay absent, and dcfs keeps serving the rest of the tree.
for name in cwd file ls listed statted umut umut2 omut cmut cmut2; do
	if [ -e "$MNT/$name" ] || [ -e "$SRC/$name" ]; then
		fail "gone-$name" "$name still exists"
	else
		pass "gone-$name"
	fi
done
mkdir "$MNT/after"
if [ -d "$SRC/after" ]; then
	pass still-serving
else
	fail still-serving "mkdir after the removals did not reach /src"
fi

# dcfs counts the kernel's lookups to know when the last reference to a
# removed object is gone; a FORGET of more lookups than it counted is
# logged as an error. Walk the tree (lookups and readdirplus), then make
# the kernel forget everything it can.
mkdir -p "$MNT/tree/a/b"
touch "$MNT/tree/a/f1" "$MNT/tree/a/b/f2"
ln "$MNT/tree/a/f1" "$MNT/tree/a/b/f1link"
ln -s f1 "$MNT/tree/a/sym"
ls -laR "$MNT" >/dev/null
find "$MNT" -exec stat {} + >/dev/null
echo 2 >/proc/sys/vm/drop_caches
ls -laR "$MNT" >/dev/null
echo 2 >/proc/sys/vm/drop_caches
if grep -q 'lookups of nodeid' "$LOG"; then
	fail lookup-counts "$(grep 'lookups of nodeid' "$LOG" | head -3)"
else
	pass lookup-counts
fi

exit "$FAILED"
