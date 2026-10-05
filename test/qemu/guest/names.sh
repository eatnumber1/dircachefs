#!/bin/sh
# dcfs step 9: file names are bytes. One guest runs the whole name corpus
# (about 60 names, one per hazard class -- see docs/plan/phases/09-*.md and
# tools/testutil.c's names-* subcommands) through every operation, on
# whichever filesystem backs /src (qemu_test_matrix: ext4, xfs, btrfs):
#
#   pre   created directly on the backing filesystem before dcfs starts, so
#         dcfs meets every name through its populate/lookup-from-backing path;
#   post  created through the dcfs mount (create, mkdir, symlink, link, xattr,
#         a rename chain), so dcfs meets every name through its mutation path.
#
# Each tree is checked against the corpus (readdir exact bytes, stat,
# readlink, xattrs, content, NFS-style handles) and compared byte for byte
# with the backing filesystem's own view. Then dcfs restarts: the same checks
# pass again, metadata is served with zero backing reads, and the handles
# taken before the restart still open. Finally everything is removed through
# dcfs and the backing filesystem must be empty. Also: errno parity with the
# backing filesystem for ".", ".." and 256-byte names, a directory chain
# deeper than PATH_MAX, and a name with a newline in a log line.
FAILED=0
. "$(dirname "$0")/lib.sh"

DCFS=/bin/dcfs
TESTUTIL=/bin/testutil

SRC=/src
MNT=/mnt
DB=/cache/dcfs.db
LOG1=/tmp/dcfs-1.log
LOG2=/tmp/dcfs-2.log

DAEMON_PID=""
MOUNTED=0

cleanup() {
	rc=$?
	if [ "$rc" -ne 0 ] || [ "$FAILED" -ne 0 ]; then
		echo "--- dcfs stderr (run 1) ---"
		cat "$LOG1" 2>/dev/null
		echo "--- dcfs stderr (run 2) ---"
		cat "$LOG2" 2>/dev/null
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

echo "names.sh: kernel $(uname -r)"

# check NAME CMD...: passes if CMD exits 0; otherwise prints its output.
check() {
	name=$1
	shift
	if out=$("$@" 2>&1); then
		pass "$name"
	else
		fail "$name" "exit $?"
		echo "$out" | head -40
	fi
}

# same_dump NAME DIR-A DIR-B [meta]: two trees dump identically (names,
# types, modes, links, sizes, targets, xattrs and contents, all in hex).
same_dump() {
	name=$1
	a=$($TESTUTIL names-dump "$2" $4 | md5sum)
	b=$($TESTUTIL names-dump "$3" $4 | md5sum)
	if [ "$a" = "$b" ]; then
		pass "$name"
	else
		fail "$name" "dump of $2 differs from $3"
		$TESTUTIL names-dump "$2" $4 >/tmp/dump-a.txt
		$TESTUTIL names-dump "$3" $4 >/tmp/dump-b.txt
		command diff /tmp/dump-a.txt /tmp/dump-b.txt | head -20
	fi
}

mount /dev/vdb /src
mkdir -p /cache /mnt

FSTYPE=$(backing_fstype "$SRC")
echo "names.sh: backing filesystem $FSTYPE"

# --- pre: the corpus made directly on the backing filesystem ---------------

FAKE_NAME=$(printf 'fake\nFORGED line')
mkdir -p "/src/bdir/$FAKE_NAME"
mkdir /src/pre
check backing-create $TESTUTIL names-create /src/pre
check backing-verify $TESTUTIL names-verify /src/pre open
sync

if start_daemon "$LOG1" --v=1 --stderrthreshold=0; then
	pass mount
else
	fail mount "daemon did not mount within 10s"
	exit "$FAILED"
fi

# A mount point below --source, named with a newline: dcfs refuses to cache
# it and says so in the log (see "a newline in a name" below).
mount -t tmpfs none "/src/bdir/$FAKE_NAME"
ls "$MNT/bdir" >/dev/null 2>&1 || true

# --- pre, seen through dcfs (populate from the backing filesystem) ---------

check pre-verify $TESTUTIL names-verify "$MNT/pre" open
same_dump pre-matches-backing "$MNT/pre" /src/pre

# --- post: the corpus created through dcfs ----------------------------------

mkdir "$MNT/post"
check post-create $TESTUTIL names-create "$MNT/post"
check post-verify $TESTUTIL names-verify "$MNT/post" open
same_dump post-matches-backing "$MNT/post" /src/post
check post-backing-verify $TESTUTIL names-verify /src/post

# --- errno parity for ".", ".." and over-long names ------------------------

mkdir "$MNT/errs"
$TESTUTIL names-errs "$MNT/errs" >/tmp/errs-mnt.txt 2>&1
$TESTUTIL names-errs /src/errs >/tmp/errs-src.txt 2>&1
if [ -s /tmp/errs-src.txt ] && cmp -s /tmp/errs-mnt.txt /tmp/errs-src.txt; then
	pass errno-parity
else
	fail errno-parity "dcfs and the backing filesystem disagree"
	command diff /tmp/errs-mnt.txt /tmp/errs-src.txt | head -20
fi

# --- a directory chain deeper than PATH_MAX, walked by fd ------------------

mkdir "$MNT/chain"
check chain-create $TESTUTIL names-chain-create "$MNT/chain"
check chain-check $TESTUTIL names-chain-check "$MNT/chain"
check chain-backing-check $TESTUTIL names-chain-check /src/chain

# --- a newline in a name must not forge a log line -------------------------

# dcfs logged the name of the mount point it refused to cache (above; the
# VLOG(1) lines are on too, --v=1).
umount "/src/bdir/$FAKE_NAME" 2>/dev/null || true
ESCAPED='fake\\nFORGED line'
if grep -q '^FORGED' "$LOG1"; then
	fail log-no-forged-line "a log line starts with FORGED"
else
	pass log-no-forged-line
fi
if grep -q "refusing to cache $ESCAPED under" "$LOG1"; then
	pass log-shows-escape
else
	fail log-shows-escape "no log line shows the escaped name"
fi

# --- handles saved now must still open after a restart ---------------------

$TESTUTIL names-handles-save "$MNT/post" /tmp/handles-post.txt >/dev/null
$TESTUTIL names-handles-save "$MNT/pre" /tmp/handles-pre.txt >/dev/null

restart_daemon restart "$LOG2" || exit "$FAILED"

# Correctness first (a content read legitimately moves the backing device's
# counter), then the metadata-only warm measurement.
check restart-pre-verify $TESTUTIL names-verify "$MNT/pre"
check restart-post-verify $TESTUTIL names-verify "$MNT/post"
check restart-handles-pre $TESTUTIL names-handles-open "$MNT/pre" /tmp/handles-pre.txt
check restart-handles-post $TESTUTIL names-handles-open "$MNT/post" /tmp/handles-post.txt
check restart-chain-check $TESTUTIL names-chain-check "$MNT/chain"

drop_caches
before_vdb=$(sectors_read vdb)
$TESTUTIL names-dump "$MNT/pre" meta >/dev/null
$TESTUTIL names-dump "$MNT/post" meta >/dev/null
after_vdb=$(sectors_read vdb)
if [ "$after_vdb" = "$before_vdb" ]; then
	pass restart-warm-vdb
else
	fail restart-warm-vdb "sectors_read(vdb) $before_vdb -> $after_vdb"
fi

same_dump restart-pre-matches-backing "$MNT/pre" /src/pre
same_dump restart-post-matches-backing "$MNT/post" /src/post

# --- remove everything through dcfs ----------------------------------------

check post-remove $TESTUTIL names-remove "$MNT/post"
check pre-remove $TESTUTIL names-remove "$MNT/pre"
rmdir "$MNT/post" "$MNT/pre"
if ! [ -e /src/pre ] && ! [ -e /src/post ]; then
	pass remove-backing-empty
else
	fail remove-backing-empty "$(ls -A /src)"
fi

exit "$FAILED"
