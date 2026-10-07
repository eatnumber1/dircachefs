#!/bin/sh
# dcfs step 4.14 regression test for 49a9b9c: Readdir/Readdirplus must stop
# adding entries exactly when the next one would not fit in the reply
# buffer, and a follow-up call at the returned (resume) offset must pick up
# from there without skipping or duplicating any entry.
#
# 49a9b9c's own commit message says this can't usefully be unit tested (the
# helpers are file-local and sizing depends on FUSE wire format, not cache
# state) -- this needs a live mount with a directory big enough that a
# single READDIR/READDIRPLUS reply cannot hold it all, forcing the kernel
# to make several round trips, each resuming at the previous one's cursor.
#
# A large directory (a few thousand entries) is created *through* dcfs
# itself (mkdir/touch), then a full listing is taken both through dcfs and
# directly against the backing filesystem and compared byte for byte after
# sorting -- any entry silently dropped, duplicated, or corrupted at a
# reply-buffer boundary shows up as a mismatch. The listing's cost is checked
# too, as a RATIO of the daemon's own CPU ticks (/proc/PID/stat) listing 4N
# entries against N entries, both already cached: a buffer-bounded listing
# costs time linear in the entries (about 4x), whereas 49a9b9c's pre-fix
# behavior (pulling every remaining entry from the cache on every READDIR
# call) is quadratic (about 16x). A ratio tolerates a uniformly slow or
# loaded host, which an absolute wall-clock budget (2.5 s) did not: it
# failed at 3.2, 2.98 and 3.04 s under load. The deterministic form of the
# same check is the harness's step counting (dir_cache_fs_test.cc,
# ReaddirWorkTest).
#
# Uses only busybox applets/options (verified against the exact busybox
# baked into the initramfs).
#
# Run as /tests/readdir_boundary.sh by guest/init when booted with
# dcfs_test=readdir_boundary.sh; prints one "TEST ... PASS/FAIL" line per
# check and exits nonzero if any check failed. init turns that into the
# final ALL-TESTS-PASSED / TEST-FAILED verdict.
FAILED=0
. "$(dirname "$0")/lib.sh"

DCFS=/bin/dcfs
TESTUTIL=/bin/testutil

SRC=/src
MNT=/mnt
DB=/cache/dcfs.db
LOG=/tmp/dcfs.log
N=6000

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

echo "readdir_boundary.sh: kernel $(uname -r)"

# Hundredths of a second since boot (field 1 of /proc/uptime, which busybox
# reports with 2 decimal places), as an integer -- good enough resolution
# for the multi-second gap this test looks for.
centiseconds() {
	read -r up _ </proc/uptime
	whole=${up%.*}
	frac=${up#*.}
	# Force base-10: a leading zero (e.g. frac="09") would otherwise be
	# parsed as an (invalid) octal literal by $(( )).
	echo $((10#$whole * 100 + 10#${frac:-0}))
}

mount /dev/vdb /src
sync

mkdir -p /cache /mnt
if start_daemon "$LOG"; then
	pass mount
else
	fail mount "daemon did not mount within 10s"
	exit "$FAILED"
fi

# A directory with enough entries -- of varying, non-trivial name length --
# that no single READDIR/READDIRPLUS reply (however large a buffer the
# kernel or busybox's own getdents64 use) can hold it all in one call.
# Created *through* dcfs (mkdir/touch), like create.sh, so the parent's
# listing is already known-complete afterward and no full backing populate
# happens during the timed pass below.
mkdir "$MNT/many"
i=0
while [ "$i" -lt "$N" ]; do
	: >"$MNT/many/file-$(printf '%05d' "$i")-the-quick-brown-fox-jumped"
	i=$((i + 1))
done
# A quarter of it, for the ratio below.
mkdir "$MNT/quarter"
i=0
while [ "$i" -lt "$((N / 4))" ]; do
	: >"$MNT/quarter/file-$(printf '%05d' "$i")-the-quick-brown-fox-jumped"
	i=$((i + 1))
done

# --- correctness: full listing matches the backing filesystem exactly ----

ls -1a "$MNT/many" | sort >/tmp/mnt_listing.txt
ls -1a "$SRC/many" | sort >/tmp/src_listing.txt
mnt_count=$(wc -l </tmp/mnt_listing.txt)
src_count=$(wc -l </tmp/src_listing.txt)
if [ "$mnt_count" = "$src_count" ] && [ "$mnt_count" = "$((N + 2))" ]; then
	pass listing-count
else
	fail listing-count "mnt=$mnt_count src=$src_count want $((N + 2))"
fi
if cmp -s /tmp/mnt_listing.txt /tmp/src_listing.txt; then
	pass listing-matches-exactly
else
	fail listing-matches-exactly "sorted listings differ -- an entry was skipped, duplicated, or corrupted at a reply-buffer boundary"
	diff /tmp/src_listing.txt /tmp/mnt_listing.txt | head -40
fi
# No duplicate names within the dcfs listing itself (uniq -d prints only
# repeated lines).
dupes=$(uniq -d </tmp/mnt_listing.txt)
if [ -z "$dupes" ]; then
	pass no-duplicate-entries
else
	fail no-duplicate-entries "duplicated across reply-buffer boundaries: $dupes"
fi

# --- warm-listing cost: quadratic (pre-fix) vs. linear (fixed) ------------

# The daemon's CPU ticks (1/100 s) for one `find` of directory $1 with the
# kernel's caches dropped; sets TICKS and COUNT.
list_cost() {
	sync
	# Quiesced: the sync point owed for the files created above must not
	# land inside the measured window.
	drop_caches_quiesced
	t0=$(cpu_ticks "$DAEMON_PID")
	find "$1" -mindepth 1 >/tmp/find_pass.txt
	t1=$(cpu_ticks "$DAEMON_PID")
	TICKS=$((t1 - t0))
	COUNT=$(wc -l </tmp/find_pass.txt)
}

list_cost "$MNT/quarter"
quarter_ticks=$TICKS
quarter_count=$COUNT
list_cost "$MNT/many"
many_ticks=$TICKS
count=$COUNT
echo "readdir_boundary.sh: warm find pass: $quarter_count entries ${quarter_ticks} ticks, $count entries ${many_ticks} ticks of daemon CPU"
if [ "$count" = "$N" ]; then
	pass find-sees-all-entries
else
	fail find-sees-all-entries "find saw $count entries, want $N"
fi
# Linear is about 4x for 4x the entries; quadratic about 16x. A floor of 2
# ticks on the small side keeps a very fast small listing from making the
# ratio meaningless.
small=$quarter_ticks
[ "$small" -lt 2 ] && small=2
if [ "$many_ticks" -le $((small * 8)) ]; then
	pass warm-listing-is-not-quadratic
else
	fail warm-listing-is-not-quadratic "$count entries cost ${many_ticks} ticks against ${quarter_ticks} for $quarter_count: more than 8x for 4x the entries, looks quadratic, not buffer-bounded"
fi

# --- "." and ".." inode numbers ---------------------------------------------
#
# Every entry's d_ino must be the backing inode number, "." and ".."
# included (".." of the root is the root itself). The $MNT side reads with
# "small-first" (testutil's first call fits exactly one record), so that
# on a FUSE mount with readdirplus "auto" the rest comes from plain
# READDIR requests (the kernel uses READDIRPLUS only at offset 0) as well
# as from READDIRPLUS -- exercising both of dcfs's own code paths.
#
# The $SRC side reads normally (no "small-first"): unlike dcfs, which
# always places "." then ".." first in its very first reply (see
# DirCacheFS::Readdir/Readdirplus), a plain backing directory makes no
# such promise -- getdents64(2) is free to return "." and ".." (and every
# other entry) in any order, e.g. hashed by name, not creation order. This
# was learned the hard way: with "small-first" on *both* sides,
# readdir_boundary_test flaked (5 of 10 runs failed on a repeated local
# run, `bazel test //test/qemu:readdir_boundary_test --runs_per_test=10
# --cache_test_results=no`), e.g.:
#
#   TEST dot-inodes/ FAIL (src: ; mnt: . 2 .. 2)
#   TEST dot-inodes/d1 FAIL (src: .. 2 . 6013; mnt: . 6013 .. 2)
#
# -- not a dcfs bug: $MNT's order was always ". <ino> .. <ino>", exactly
# as DirCacheFS::Readdir/Readdirplus always emit it; $SRC's order (and,
# with "small-first" forcing a 24-byte first getdents64(2) on the raw
# ext4 directory, even whether the call succeeded at all) varied run to
# run instead -- confirmed by instrumenting testutil's call on $SRC to
# print its raw, unfiltered output: across 15 repeats, $SRC returned "."
# and ".." in either order (e.g. "d1 6013\n.. 2\n. 2\n..."), and twice
# failed outright with EINVAL (small-first's 24-byte buffer doesn't fit
# whichever entry the directory happens to return first, if that entry
# isn't a dot entry -- e.g. "lost+found" needs 32 bytes). "src: ;" above
# is that EINVAL case: testutil's "ERR EINVAL" line doesn't match the
# grep pattern below, silently emptying $src_dots. Fix: only $MNT uses
# "small-first" (the one side whose READDIR-vs-READDIRPLUS split it is
# meant to force); $SRC reads with testutil's normal, full-buffer-sized
# first call, which cannot EINVAL on any entry used here. The remaining,
# entirely legitimate "." vs ".." ordering difference between the two
# independent reads is handled by sorting both sides before comparing --
# what must match is the *set* of (name, inode) pairs, not their order.
mkdir -p "$MNT/d1/d2"
for dir in "" /many /d1 /d1/d2; do
	src_dots=$("$TESTUTIL" readdir-ino "$SRC$dir" | grep -E '^\.\.? ' | sort)
	mnt_dots=$("$TESTUTIL" readdir-ino "$MNT$dir" small-first | grep -E '^\.\.? ' | sort)
	if [ "$src_dots" = "$mnt_dots" ]; then
		pass "dot-inodes${dir:-/}"
	else
		fail "dot-inodes${dir:-/}" "src: $(echo "$src_dots" | tr '\n' ,); mnt: $(echo "$mnt_dots" | tr '\n' ,)"
	fi
done

exit "$FAILED"
