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
# reply-buffer boundary shows up as a mismatch. This is timed too: with the
# cache already fully warm (see create.sh's identical "warm-after-all"
# technique), a correct, buffer-bounded implementation does one cache
# lookup per entry across the whole listing; 49a9b9c's own commit message
# describes the pre-fix behavior as pulling *every remaining* entry from
# the cache on *every* READDIR call regardless of what fits, which is
# quadratic in directory size -- so on a directory big enough to need many
# round trips, a reverted fix should take much longer to list even though
# (per 49a9b9c's own analysis; see AppendDirEntries, unchanged by this fix)
# the actual bytes returned to the kernel, and hence correctness, do not
# differ. Both checks are kept: the correctness one because it is the
# thing that actually matters, the timing one because it is what would
# actually distinguish the fix from its revert.
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

# --- warm-listing timing: quadratic (pre-fix) vs. linear (fixed) ----------

sync
echo 3 >/proc/sys/vm/drop_caches

before=$(centiseconds)
find "$MNT/many" -mindepth 1 >/tmp/find_pass.txt
after=$(centiseconds)
elapsed_cs=$((after - before))
count=$(wc -l </tmp/find_pass.txt)
echo "readdir_boundary.sh: warm find pass over $count entries took ${elapsed_cs}cs"
if [ "$count" = "$N" ]; then
	pass find-sees-all-entries
else
	fail find-sees-all-entries "find saw $count entries, want $N"
fi
# Measured directly against this test's own N: the fixed implementation
# lists these 6000 already-cached entries in ~50cs (0.5s); reverting
# 49a9b9c's early-stop (see the commit message) back to "pull every
# remaining entry on every call" measured ~780cs (7.8s) for the same
# directory -- roughly 16x slower, consistent with the quadratic-vs-linear
# difference the fix describes. 250cs sits with wide margin (5x) on both
# sides of that gap, comfortably tolerating a slower CI host without
# masking the regression.
if [ "$elapsed_cs" -lt 250 ]; then
	pass warm-listing-is-not-quadratic
else
	fail warm-listing-is-not-quadratic "took ${elapsed_cs}cs (>= 250cs / 2.5s) for $N already-cached entries -- looks quadratic, not buffer-bounded"
fi

# --- "." and ".." inode numbers ---------------------------------------------
#
# Every entry's d_ino must be the backing inode number, "." and ".."
# included (".." of the root is the root itself). testutil readdir-ino
# reads "." alone first, so that ".." and the rest come from plain READDIR
# requests (with readdirplus "auto" the kernel sends READDIRPLUS only at
# offset 0) as well as from READDIRPLUS.
mkdir -p "$MNT/d1/d2"
for dir in "" /many /d1 /d1/d2; do
	src_dots=$("$TESTUTIL" readdir-ino "$SRC$dir" | grep -E '^\.\.? ')
	mnt_dots=$("$TESTUTIL" readdir-ino "$MNT$dir" | grep -E '^\.\.? ')
	if [ "$src_dots" = "$mnt_dots" ]; then
		pass "dot-inodes${dir:-/}"
	else
		fail "dot-inodes${dir:-/}" "src: $(echo $src_dots); mnt: $(echo $mnt_dots)"
	fi
done

exit "$FAILED"
