#!/bin/sh
# dcfs phase 10: memory. dcfs's resident memory after a `find` over a large
# tree, and after the kernel drops its inode cache (which sends FORGETs):
# dcfs must hold memory for what the kernel references, not for the tree.
#
# What RSS can and cannot show: dcfs's per-inode state is one entry of the
# kernel-lookup-count map (~16 bytes) plus whatever sqlite caches, and the
# map is an Abseil flat_hash_map, which keeps its capacity when entries are
# erased; so RSS does not fall back to its pre-find value after the drop
# (measured: it is a little higher, from sqlite's page cache and the
# allocator), and "shrinks back near baseline" cannot be asserted. The
# test instead asserts the property the design needs, that memory follows
# the kernel's referenced inodes and not the tree:
#   - find-grows-rss: the find did add to dcfs's RSS (the test measures
#     something);
#   - bytes-per-inode: what a full find adds is under 256 bytes per entry;
#   - second-tree: a capacity-keeping map is fine as long as it is bounded
#     by what the kernel referenced at once, not by the tree: after find
#     over half the tree and a drop, a find over the OTHER half and a drop
#     must add at most half of what the first half added (if FORGET left
#     the first half's entries in the map, the second half would add its
#     own on top, about as much again, and the test fails).
# bytes_per_inode and the RSS at each step are printed.
FAILED=0
. "$(dirname "$0")/lib.sh"

DCFS=/bin/dcfs
BENCH=/bin/dcfs_bench
SRC=/src
MNT=/mnt
DB=/cache/dcfs.db
LOG=/tmp/dcfs.log

# Under TCG a find over 50000 entries does not fit the test's timeout; 20000
# still gives two top-level directories (one per half).
if [ "${DCFS_ACCEL:-kvm}" = tcg ]; then
	ENTRIES=${ENTRIES:-20000}
else
	ENTRIES=${ENTRIES:-50000}
fi

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

# rss_kb: dcfs's VmRSS in KiB.
rss_kb() {
	while read -r key val _; do
		if [ "$key" = "VmRSS:" ]; then
			echo "$val"
			return
		fi
	done <"/proc/$DAEMON_PID/status"
}

echo "memory.sh: kernel $(uname -r), ENTRIES=$ENTRIES"

mount /dev/vdb $SRC
echo "memory.sh: making the tree: $(date +%s)"
"$BENCH" mktree "$SRC" "$ENTRIES" 0 || {
	fail mktree "could not make the tree"
	exit 1
}
sync

echo "memory.sh: tree made: $(date +%s)"
mkdir -p /cache /mnt
if start_daemon "$LOG"; then
	pass mount
else
	fail mount "daemon did not mount within 10s"
	exit "$FAILED"
fi

tops=$(((ENTRIES + 9999) / 10000))
# First half of the tree: t/000..t/<tops/2-1>; second half: the rest.
first=""
second=""
n=0
while [ "$n" -lt "$tops" ]; do
	d=$(printf '%s/t/%03d' "$MNT" "$n")
	if [ "$n" -lt $((tops / 2)) ]; then first="$first $d"; else second="$second $d"; fi
	n=$((n + 1))
done

rss0=$(rss_kb)
find "$MNT" >/dev/null
rssf=$(rss_kb)
echo 2 >/proc/sys/vm/drop_caches
sleep 2
rssd=$(rss_kb)
echo "memory.sh: RSS KiB: mounted=$rss0 after-find=$rssf after-drop=$rssd"

# Second-tree check, on a fresh process so that the first phase's capacity
# does not hide anything: restart, find half, drop, find the other half, drop.
restart_daemon restart "$LOG" || exit "$FAILED"
rss_m=$(rss_kb)
find $first >/dev/null
rss_a=$(rss_kb)
echo 2 >/proc/sys/vm/drop_caches
sleep 2
rss_ad=$(rss_kb)
find $second >/dev/null
rss_b=$(rss_kb)
echo 2 >/proc/sys/vm/drop_caches
sleep 2
rss_bd=$(rss_kb)
echo "memory.sh: RSS KiB: fresh=$rss_m first-half=$rss_a dropped=$rss_ad second-half=$rss_b dropped=$rss_bd"

grew=$((rssf - rss0))
bytes_per_inode=$((grew * 1024 / ENTRIES))
echo "memory.sh: bytes per referenced inode: $bytes_per_inode (find added ${grew} KiB for $ENTRIES entries)"

if [ "$grew" -gt $((ENTRIES / 100)) ]; then
	pass find-grows-rss
else
	fail find-grows-rss "RSS grew only ${grew} KiB over $ENTRIES entries"
fi

if [ "$bytes_per_inode" -le 256 ]; then
	pass bytes-per-inode
else
	fail bytes-per-inode "$bytes_per_inode bytes per entry (limit 256)"
fi

# Everything the second half adds on top of the first drop is sqlite page
# cache and allocator noise; if FORGET left entries in the map the second
# half would add its own, about what the first half added. Half of that
# growth is the limit.
first_growth=$((rss_a - rss_m))
second_growth=$((rss_bd - rss_ad))
if [ "$((second_growth * 2))" -le "$first_growth" ]; then
	pass second-tree
else
	fail second-tree "second half added ${second_growth} KiB after the first drop; the first half added ${first_growth} KiB (limit: half): FORGET does not release per-inode state"
fi

exit "$FAILED"
