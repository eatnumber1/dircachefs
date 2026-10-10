#!/bin/sh
# dcfs step 12.14: the model's abstraction of the cache database's
# durability, tested against what SQLite leaves on a disk that reorders
# writes (formal/README.md, "Abstractions"; formal/dcfs.tla, Commit and
# dbOpts; docs/design.md, "A power loss or kernel crash").
#
# The abstraction: every commit is normal (synchronous=NORMAL, no fsync) or
# synced (Durability::kSync, a WAL fsync); a power loss leaves the database
# in the state after some commit since the last synced one. So the commits
# that survive are a prefix of the commit order (a commit never survives an
# earlier lost one), every synced commit that was acknowledged survives, and
# a normal commit may be lost.
#
# How it is tested. The cache filesystem (vdc: ext4, xfs or btrfs, the
# target's matrix) sits on dm-log-writes, which records every write that
# reaches the disk, every FLUSH and FUA and our marks, in a log. dcfs first
# creates files until SQLite checkpoints its WAL (the autocheckpoint, at 1000
# frames) and restarts it, so that every later frame overwrites an older one
# in place: no file growth for the filesystem's journal to order, so the
# disk's reordering of frame writes reaches SQLite's recovery. Then dcfs
# runs a scripted sequence of mutations whose durability is known: creates,
# unlinks, a rename, a mkdir and rmdir, a write, a chmod, a setxattr (each
# phase 1 synced unless the inodes are durably dirty already: the fast path,
# normal), a sync point (an fsync through dcfs, after which the next phase 1
# is synced again), and a writeback of the WAL by the filesystem alone (an
# fsync of the -wal from outside, as periodic writeback would). After each
# one a STATFS through dcfs (served only after every request queued before
# it) orders what follows, and the test records the WAL's commits so far
# (crash_states wal-info: what SQLite's recovery would accept of the -wal as
# the page cache has it), the cost counter's transactions and synced
# transactions (the SqliteTransaction event: dcfs's intent, from
# $DCFS_COUNTERS_FILE), and puts a mark in the log. The daemon runs under
# strace, which gives every write and fsync of the -wal: each fsync names
# the commit it made durable (the one whose last frame it follows), and each
# operation's synced transactions must be as many WAL fsyncs. At the end a
# copy of the database and its WAL is the reference: the state after commit
# k is the reference database with the WAL cut after its k-th commit
# (crash_states wal-truncate), as SQLite recovers it. Then the daemon is
# killed and the cache unmounted, which writes back what was still cached
# (the end of the log).
#
# The crash states, CrashMonkey's method (Mohan et al., OSDI 2018) on the
# log: everything before a FLUSH is durable after it; writes after it may
# land in any subset; a FUA write is durable once done. dm-log-writes logs a
# FLUSH when it completes, after every ordinary write done before it was
# issued, and logs a FUA write when it completes, so between two FLUSHes the
# order of the ordinary writes against the FUA ones is not in the log. The
# states taken, each a replay of the log onto a copy of the disk:
#
#   prefix   every entry before each FLUSH from the first after the start
#            mark (all of them), and the whole log;
#   fua      in each epoch (from one FLUSH to the next), every prefix of its
#            FUA writes and none of its ordinary ones;
#   tear     all of the epoch's FUA writes, and one of its ordinary writes
#            of several 4 KiB blocks torn (some of its blocks landed: a disk
#            promises neither order nor atomicity within one request),
#            with every other ordinary write or with none;
#   subset   all of the epoch's FUA writes and a subset of its ordinary
#            ones: every subset when they fit the epoch's share of the
#            budget (dcfs_sd_budget= states in all, shared by the epochs
#            left, at least MIN_CAP each) or are no more than EXHAUSTIVE,
#            else each single write lost,
#            each single write kept and random subsets (seeded:
#            dcfs_sd_seed=) up to the share.
#
# Each state is replayed onto a dm-snapshot of the replay disk (kept at the
# epoch's FLUSH by xfstests' replay-log), mounted (the filesystem's own
# recovery), the database and its -wal copied out, opened by SQLite (its WAL
# recovery: crash_states db-fingerprint, which also requires
# integrity_check "ok") and fingerprinted (the md5 of the database with the
# recovered frames checkpointed into it). The oracle: the fingerprint must be
# the reference state after some commit k (else a commit survived an earlier
# lost one, or the database is no state at all), and k must be at least the
# lower bound: the last synced commit of every operation whose mark is
# logged before the epoch's FLUSH (acknowledged before the mark; a mark
# before the FLUSH was issued before the FLUSH completed, so before any
# crash in the epoch). Marks order nothing against the ordinary writes (the
# log holds those back until the next FLUSH), which is why only the FLUSHes
# are used. Several commits can leave the same database (a release record
# that changes nothing): k is the last of them. And the first of them must
# be at most the last commit of the first operation marked after the
# epoch's end (the operations after it began after the epoch: a later
# state would be a replay bug). A violation fails, naming the state, its
# write subset and the fingerprint.
#
# Reach: crash_states wal-info counts, in each judged state's WAL, the
# frames of the current generation past the recovered ones, and those after
# a frame that did not land. Some state of an epoch that ends before the cut
# mark must have one of the latter (a later frame kept while an earlier one
# was lost), or the reordering never reached SQLite.
#
# Self-checks of the oracle, before the replay: a WAL with a commit lost and
# the next one kept (crash_states wal-skip: a state no real power loss
# leaves; one whose database opens) must match no state; the state before
# the last synced commit, judged against it as the bound, must be refused;
# the state after it must be accepted. During it: our log-apply of one whole
# epoch must leave the disk replay-log leaves. The crash states themselves
# are checked on the host (//test/qemu:sqlite_durability_lib_test).
#
# The positive case: the whole log recovers exactly the final reference
# state, and dcfs started on it recovers (StartRun) to what the backing
# filesystem holds.
#
# Run as /tests/sqlite_durability.sh by guest/init; one "TEST ... PASS/FAIL"
# line per check, and the measurements on SQLITE-DURABILITY lines.
FAILED=0
. "$(dirname "$0")/lib.sh"
. "$(dirname "$0")/sqlite_durability_lib.sh"
. "$(dirname "$0")/ramdisk_lib.sh"

CS=/bin/crash_states
REPLAY_LOG=/bin/replay-log
DMSETUP=/sbin/dmsetup
TESTUTIL=/bin/testutil
MOUNT_DCFS=/sbin/mount.dcfs
SRC=/src
MNT=/mnt
CACHE_DIR=/cache
DB=$CACHE_DIR/dcfs.db
REPLAY_MNT=/replay
BACK_DEV=/dev/vdb
CACHE_DEV=/dev/vdc
# The log, the replay disk and the snapshots' copy-on-write store are loop
# devices over sparse files in a tmpfs (LOOPS): a guest has four usable
# virtio disks at most (the fifth's interrupt is the RTC's), and the
# coverage run adds one.
LOOPS=/loops
LOG_DEV=""
REPLAY_DEV=""
COW_DEV=""
LOGW=sdlog
SNAP=sdsnap
WORK=/tmp/sd
OPS=$WORK/ops
ENTRIES=$WORK/entries
REF_FPS=$WORK/ref.fps
DPID=""

cmdline() { sed -n "s/.*\b$1=\([^ ]*\).*/\1/p" /proc/cmdline; }
SEED=$(cmdline dcfs_sd_seed)
SEED=${SEED:-1}
BUDGET=$(cmdline dcfs_sd_budget)
BUDGET=${BUDGET:-1500}
SCRIPT=$(cmdline dcfs_sd_script)
SCRIPT=${SCRIPT:-full}
# Each epoch's share of the budget is at least this many states, and an
# epoch with no more states than EXHAUSTIVE has them all taken.
MIN_CAP=16
EXHAUSTIVE=64

cleanup() {
	rc=$?
	if [ "$rc" -ne 0 ] || [ "$FAILED" -ne 0 ]; then
		echo "--- syslog (dcfs) ---"
		logread 2>/dev/null | tail -n 60
	fi
	if [ -n "$DPID" ]; then
		kill -KILL "$DPID" 2>/dev/null || true
	fi
	umount -l "$MNT" 2>/dev/null || true
	umount "$REPLAY_MNT" 2>/dev/null || true
	"$DMSETUP" remove --noudevsync "$SNAP" 2>/dev/null || true
	umount "$CACHE_DIR" 2>/dev/null || umount -l "$CACHE_DIR" 2>/dev/null || true
	"$DMSETUP" remove --noudevsync "$LOGW" 2>/dev/null || true
	for dev in $LOG_DEV $REPLAY_DEV $COW_DEV; do
		losetup -d "$dev" 2>/dev/null || true
	done
	umount "$LOOPS" 2>/dev/null || true
	umount "$SRC" 2>/dev/null || true
}
trap cleanup EXIT

echo "sqlite_durability.sh: kernel $(uname -r), script $SCRIPT, seed $SEED, budget $BUDGET"
require_commands flock losetup mkfifo pidof strace syslogd logread
mkdir -p "$WORK" "$SRC" "$MNT" "$CACHE_DIR" "$REPLAY_MNT" "$LOOPS"
syslogd -C256

sectors() { cat "/sys/class/block/${1#/dev/}/size"; }

# --- dcfs -------------------------------------------------------------------

# start_dcfs [traced]: mount.dcfs daemonized: it returns once dcfs has
# answered FUSE_INIT (no wait of ours). Sets DPID. With "traced", under
# strace -D (the tracer a detached grandchild, so the shell waits for the
# wrapper alone), recording the daemon's writes and fsyncs of files into
# the FIFO $WORK/strace.fifo, which a cat of ours (STRACE_CAT) copies to
# $WORK/strace.out until the tracer exits with the daemon.
STRACE_CAT=""
start_dcfs() {
	sd_opts="dcfs.fstype=bind,dcfs.cache_db=$DB,dcfs.sync_interval_sec=3600"
	if [ "$1" = traced ]; then
		rm -f "$WORK/strace.fifo"
		mkfifo "$WORK/strace.fifo"
		cat "$WORK/strace.fifo" >"$WORK/strace.out" &
		STRACE_CAT=$!
		strace -D -f -qq -y -e signal=none \
			-e trace=pwrite64,pwritev,pwritev2,write,writev,fsync,fdatasync \
			-o "$WORK/strace.fifo" \
			"$MOUNT_DCFS" -o "$sd_opts" "$SRC" "$MNT" >"$WORK/mount.out" 2>&1 ||
			return 1
	else
		"$MOUNT_DCFS" -o "$sd_opts" "$SRC" "$MNT" >"$WORK/mount.out" 2>&1 ||
			return 1
	fi
	DPID=$(pidof mount.dcfs)
	[ -n "$DPID" ]
}

# kill_dcfs: SIGKILL, then wait for the daemon's lock on the database to go
# (the README's wait: the lock goes with the process).
kill_dcfs() {
	kill -KILL "$DPID"
	umount -l "$MNT"
	flock "$DB" true
	DPID=""
}

# counters: "<transactions> <synced transactions>" from the cost counter.
counters() {
	awk '$1 == "transactions" { t = $2 } $1 == "durable_transactions" { d = $2 }
		END { print t + 0, d + 0 }' "$WORK/counters"
}

# wal_field NAME: one field of crash_states wal-info for the live -wal.
wal_field() {
	"$CS" wal-info "$DB-wal" | tr ' ' '\n' | sed -n "s/^$1=//p"
}

# record NAME: after an operation, orders it before what follows and
# records it: one line of $OPS, "<n> <name> <commits before> <commits
# after> <transactions> <synced transactions> <salt>", and the mark op<n>.
OPN=0
LAST_COMMITS=0
record() {
	# STATFS adds no transaction, and dcfs answers it only after every
	# request queued before it (one thread, requests in order): the
	# counters then hold the operation's every transaction, and the -wal
	# its every commit, whatever STATFS's own end writes. This assumes
	# dcfs serves one request at a time; under Phase 22's coroutines a
	# request suspended in a backing syscall would let STATFS overtake it,
	# and the barrier needs another form (wait for the daemon to have no
	# request in flight).
	stat -f "$MNT" >/dev/null
	r_c1=$(counters)
	stat -f "$MNT" >/dev/null
	r_c2=$(counters)
	if [ "$r_c1" != "$r_c2" ]; then
		fail "counters-stable" "$1: STATFS changed the transaction counts: $r_c1, then $r_c2"
	fi
	r_commits=$(wal_field commits)
	r_salt=$(wal_field salt)
	OPN=$((OPN + 1))
	set -- "$1" $r_c1
	echo "$OPN $1 $LAST_COMMITS $r_commits $2 $3 $r_salt" >>"$OPS"
	LAST_COMMITS=$r_commits
	"$DMSETUP" message "$LOGW" 0 mark "op$OPN"
}

# op NAME CMD...: runs one operation of the script and records it.
# op NAME KIND CMD...: runs one operation of the script and records it,
# with the durability dcfs's rules give it in $WORK/expect: "synced" (a
# phase 1 naming an inode that is not durably dirty: the first create, mkdir
# or setattr since the start or a sync point in a directory or of an inode),
# "normal" (every inode it names durably dirty already: the fast path; a
# sync point's ClearDirty), or "-" (no commit of dcfs's own, or no rule
# that fixes it).
op() {
	o_name=$1
	o_kind=$2
	shift 2
	"$@" >/dev/null 2>"$WORK/op.err" ||
		fail "op-$o_name" "$* failed: $(cat "$WORK/op.err")"
	record "$o_name"
	echo "$OPN $o_kind" >>"$WORK/expect"
}

creates() {
	c_dir=$1
	shift
	for c_name in "$@"; do
		: >"$c_dir/$c_name" || return 1
	done
}

write_file() { echo data >"$1"; }

# cache_flush: a FLUSH of the cache disk that writes none of the WAL: an
# fsync of another file on the cache filesystem (its journal commit or log
# force). The unlink before it has its mark before this FLUSH and its WAL
# pages after it (they reach the disk at the unmount), so the crash states
# of the epochs between lose a completed operation, whatever the
# filesystem's own journal timer did.
cache_flush() {
	echo flush >"$CACHE_DIR/flush" && "$TESTUTIL" fsync "$CACHE_DIR/flush"
}

# The script. Whether a commit is synced is dcfs's to decide (a phase 1 is
# synced unless every inode it names is durably dirty since the last sync
# point; since step 23.11 a created file is born dirty, so n creates in a
# directory not yet durably dirty make one synced commit); each op names
# what the rules give it, and the ops table and the strace say what dcfs
# decided (durability-as-dcfs-rules-say compares). Each ends with an unlink
# of a file both of whose inodes are durably dirty: normal commits only, which
# reach the disk only when the cache is unmounted, and a FLUSH after it
# (cache_flush), so that some crash states lose a completed operation (and
# a commit that changed the database: the release records after a create's
# last synced commit can leave it as it was).
script_full() {
	op warm - ls "$MNT/d1" "$MNT/d2" "$MNT/d3" "$MNT/d4" "$MNT/d5"
	op create-d1-a synced creates "$MNT/d1" a
	op create-d1-b normal creates "$MNT/d1" b
	op mkdir-d2-x synced mkdir "$MNT/d2/x"
	op create-d3-f1-f4 synced creates "$MNT/d3" f1 f2 f3 f4
	op unlink-d1-a normal rm "$MNT/d1/a"
	op rename-d1-b-c normal mv "$MNT/d1/b" "$MNT/d1/c"
	op wal-writeback - "$TESTUTIL" fsync "$DB-wal"
	op create-d3-g1-g2 normal creates "$MNT/d3" g1 g2
	op sync-point normal "$TESTUTIL" fsync "$MNT/d3/g1"
	op create-d3-g3 synced creates "$MNT/d3" g3
	op write-d4-w synced write_file "$MNT/d4/w"
	op chmod-d4-keep synced chmod 600 "$MNT/d4/keep"
	op setxattr-d4-keep normal "$TESTUTIL" setxattr "$MNT/d4/keep" user.k v
	# x is born dirty by mkdir's phase 3, a normal commit; no phase 1 named
	# it, so it is not durably dirty and the rmdir's phase 1 is synced.
	op rmdir-d2-x synced rmdir "$MNT/d2/x"
	op create-d5-t1-t6 synced creates "$MNT/d5" t1 t2 t3 t4 t5 t6
	op unlink-d5-t1 normal rm "$MNT/d5/t1"
	op cache-flush - cache_flush
}

script_short() {
	op warm - ls "$MNT/d1" "$MNT/d3"
	op create-d1-a synced creates "$MNT/d1" a
	op create-d1-b normal creates "$MNT/d1" b
	op unlink-d1-a normal rm "$MNT/d1/a"
	op sync-point normal "$TESTUTIL" fsync "$MNT/d1/b"
	op create-d3-t1-t3 synced creates "$MNT/d3" t1 t2 t3
	op unlink-d3-t1 normal rm "$MNT/d3/t1"
	op cache-flush - cache_flush
}

# --- reference states and the oracle ------------------------------------------

# fingerprint DIR: the md5 of DIR/dcfs.db after SQLite recovered the
# DIR/dcfs.db-wal beside it into it; nothing (and a message on stderr) if
# it does not open or fails integrity_check.
fingerprint() {
	"$CS" db-fingerprint "$1/dcfs.db" >&2 &&
		md5sum "$1/dcfs.db" | cut -d' ' -f1
}

# ref_state K DIR: the reference database after commit K in DIR.
ref_state() {
	rm -rf "$2"
	mkdir -p "$2"
	cp "$WORK/ref/dcfs.db" "$2/dcfs.db"
	"$CS" wal-truncate "$WORK/ref/dcfs.db-wal" "$1" "$2/dcfs.db-wal"
}

# oracle FINGERPRINT LOWER: "ok K J", K the last and J the first commit
# whose state has the fingerprint (several commits can leave the same
# database), if K >= LOWER; "lost K J" if K is below LOWER; "none" if no
# state has it.
oracle() {
	awk -v fp="$1" -v lower="$2" '
		$2 == fp {
			if (!found || $1 + 0 < first) first = $1 + 0
			if (!found || $1 + 0 > best) best = $1 + 0
			found = 1
		}
		END {
			if (!found) print "none"
			else if (best >= lower) print "ok", best, first
			else print "lost", best, first
		}' "$REF_FPS"
}

# --- the log --------------------------------------------------------------

# is_flush and the like: an entry's flags (field 4 of $ENTRIES).
flags_have() { case "|$1|" in *"|$2|"*) return 0 ;; esac; return 1; }

# mark_index NAME: the log index of the mark NAME.
mark_index() { awk -v m="$1" '$4 ~ /MARK/ && $5 == m { print $1; exit }' "$ENTRIES"; }

# synced_commits: from the daemon's strace and the reference WAL's commits,
# one line per fsync of the -wal since the WAL's last restart (its last
# header write, at offset 0), "<commit> <exact|partial|header>": the last
# commit whose frames were all written before it ("exact": the fsync came
# right after that commit's last frame; "header": nothing but the WAL's
# 32-byte header was written, which SQLite syncs whatever the synchronous
# level when it starts a WAL, before the first frame). SQLite writes each
# frame with pwrite64 at its offset, and a synced commit's fsync follows
# its commit frame. A line "split <n>" is a syscall on the -wal that strace
# printed in two halves (two threads at once), which would hide an offset.
synced_commits() {
	awk '
		FNR == NR { end[$1] = $2; n = $1; next }
		/dcfs\.db-wal>/ && /unfinished/ { print "split", FNR; next }
		/dcfs\.db-wal>/ && /^[0-9]+ +pwrite64\(/ {
			if (match($0, /, [0-9]+\) += [0-9]+$/)) {
				split(substr($0, RSTART + 2), f, /[^0-9]+/)
				if (f[1] == 0) { maxend = 0; m = 0 }
				if (f[1] + f[2] > maxend) maxend = f[1] + f[2]
			}
			next
		}
		/dcfs\.db-wal>/ && /^[0-9]+ +f(data)?sync\(/ && / = 0$/ {
			c = 0
			for (k = 1; k <= n; k++) if (end[k] <= maxend) c = k
			if (c == 0 && maxend == 32) line[++m] = "0 header"
			else line[++m] = c " " (c > 0 && end[c] == maxend ? "exact" : "partial")
		}
		END { for (i = 1; i <= m; i++) print line[i] }' "$WORK/commits" "$WORK/strace.out"
}

# bounds INDEX NEXT: "<lower> <acknowledged> <upper>" for the states of the
# epoch from the log entry INDEX (a FLUSH) to NEXT (the next FLUSH, or the
# end). <lower> is the commit every such state must have: the last synced
# commit of the operations whose marks are logged before INDEX (each
# acknowledged before its operation's mark, so before any crash at or after
# INDEX). <acknowledged> is the last commit of those operations. <upper> is
# the last commit such a state can hold: that of the first operation whose
# mark is logged after NEXT (the operations after it began after NEXT
# completed, so wrote nothing of the epoch's), or the last commit.
bounds() {
	awk -v at="$1" -v next_="$2" -v last="$REF_COMMITS" '
		FILENAME == ARGV[1] { if ($4 ~ /MARK/) mark[$5] = $1; next }
		FILENAME == ARGV[2] { synced_op[$1] = 1; synced[$1] = $2; next }
		{ m = $1 == 0 ? "dcfs-up" : "op" $1 }
		m in mark && mark[m] < at {
			if ($1 in synced_op && synced[$1] > lo) lo = synced[$1]
			if ($4 > acked) acked = $4
		}
		upper == "" && m in mark && mark[m] > next_ { upper = $4 }
		END { print lo + 0, acked + 0, (upper == "" ? last : upper) }' \
		"$ENTRIES" "$WORK/synced" "$OPS"
}

STATES=0
VIOLATIONS=0
LOST_ACKED=0
MIN_MARGIN=""
REACH_BEYOND=0
REACH_GAP=0
REACH_GAP_SCRIPT=0
REACH_EXAMPLE=""
# 1 while the states judged are of an epoch that ends before the cut mark
# (between two of the script's FLUSHes; an epoch that ends at the unmount's
# FLUSH holds the unmount's writeback, which the log holds back until then).
IN_SCRIPT=0
# check_state NAME LOWER ACKED UPPER [ENTRY...]: replays the entries ENTRY...
# (crash_states log-apply's arguments) onto a snapshot of the replay disk (as
# it is: every entry before the epoch's FLUSH), recovers it and asks the
# oracle; a violation fails NAME. LOWER: the bound; UPPER: the last commit
# the state can hold (a later one would be a replay that put writes from
# after the crash on the disk); ACKED: the last commit of an operation done
# before the crash, for the count of states that lost one. Counts the states
# whose WAL holds this generation's frames past the recovered ones
# (REACH_BEYOND), and those of them after a frame that did not land
# (REACH_GAP: a later frame kept, an earlier one lost).
check_state() {
	cs_name=$1
	cs_lower=$2
	cs_acked=$3
	cs_upper=$4
	shift 4
	cs_entries="$*"
	STATES=$((STATES + 1))
	"$DMSETUP" create --noudevsync "$SNAP" \
		--table "0 $SECTORS snapshot $REPLAY_DEV $COW_DEV N 8" || {
		fail "$cs_name-snapshot" "dmsetup create failed"
		return
	}
	if [ "$#" -gt 0 ]; then
		"$CS" log-apply "$LOG_DEV" "/dev/mapper/$SNAP" "$@" ||
			fail "$cs_name-apply" "log-apply failed"
	fi
	rm -rf "$WORK/st"
	mkdir -p "$WORK/st"
	if mount -t "$CACHE_FSTYPE" "/dev/mapper/$SNAP" "$REPLAY_MNT" 2>"$WORK/mount.err"; then
		cp "$REPLAY_MNT/dcfs.db" "$WORK/st/" 2>/dev/null
		[ -e "$REPLAY_MNT/dcfs.db-wal" ] && cp "$REPLAY_MNT/dcfs.db-wal" "$WORK/st/"
		umount "$REPLAY_MNT"
		cs_mounted=1
	else
		cs_mounted=0
	fi
	"$DMSETUP" remove --noudevsync "$SNAP"
	if [ "$cs_mounted" = 0 ]; then
		VIOLATIONS=$((VIOLATIONS + 1))
		fail "$cs_name" "the crash state does not mount: $(cat "$WORK/mount.err"); entries: $*"
		return
	fi
	cs_info=""
	[ -e "$WORK/st/dcfs.db-wal" ] && cs_info=$("$CS" wal-info "$WORK/st/dcfs.db-wal")
	cs_beyond=$(echo "$cs_info" | tr ' ' '\n' | sed -n 's/^beyond=//p')
	cs_gap=$(echo "$cs_info" | tr ' ' '\n' | sed -n 's/^aftergap=//p')
	cs_fp=$(fingerprint "$WORK/st" 2>"$WORK/fp.err")
	if [ -z "$cs_fp" ]; then
		VIOLATIONS=$((VIOLATIONS + 1))
		fail "$cs_name" "the database does not open as a database: $(cat "$WORK/fp.err"); entries: $*"
		return
	fi
	set -- $(oracle "$cs_fp" "$cs_lower")
	cs_verdict=$1
	cs_best=${2:-}
	cs_min=${3:-}
	echo "$cs_name ${cs_best:-none}" >>"$WORK/recovered"
	if [ "${cs_beyond:-0}" -gt 0 ]; then
		REACH_BEYOND=$((REACH_BEYOND + 1))
		if [ "${cs_gap:-0}" -gt 0 ]; then
			REACH_GAP=$((REACH_GAP + 1))
			if [ "$IN_SCRIPT" = 1 ]; then
				REACH_GAP_SCRIPT=$((REACH_GAP_SCRIPT + 1))
				[ -n "$REACH_EXAMPLE" ] ||
					REACH_EXAMPLE="$cs_name: recovered commit ${cs_best:-none} ($cs_info); entries: $cs_entries"
			fi
		fi
	fi
	case "$cs_verdict" in
	ok)
		if [ "$cs_min" -gt "$cs_upper" ]; then
			VIOLATIONS=$((VIOLATIONS + 1))
			fail "$cs_name" "recovered the state after commit $cs_min (or a later one alike), past the last the crash point allows ($cs_upper): a replay bug; entries: $cs_entries"
		fi
		[ "$cs_best" -lt "$cs_acked" ] && LOST_ACKED=$((LOST_ACKED + 1))
		cs_margin=$((cs_best - cs_lower))
		if [ -z "$MIN_MARGIN" ] || [ "$cs_margin" -lt "$MIN_MARGIN" ]; then
			MIN_MARGIN=$cs_margin
		fi
		;;
	lost)
		VIOLATIONS=$((VIOLATIONS + 1))
		fail "$cs_name" "an acknowledged synced commit was lost: recovered the state after commit $cs_best, the bound is $cs_lower; entries: $cs_entries"
		;;
	*)
		VIOLATIONS=$((VIOLATIONS + 1))
		fail "$cs_name" "the recovered database ($cs_fp; $cs_info) is the state after no commit: a commit survived an earlier lost one; entries: $cs_entries"
		;;
	esac
}

# --- setup ----------------------------------------------------------------

mount "$BACK_DEV" "$SRC" || {
	fail setup "mounting the backing filesystem failed"
	exit "$FAILED"
}
for d in d0 d1 d2 d3 d4 d5; do
	mkdir "$SRC/$d"
	echo keep >"$SRC/$d/keep"
done
sync

SECTORS=$(sectors "$CACHE_DEV")
mount -t tmpfs -o size=1g loops "$LOOPS" &&
	truncate -s 256M "$LOOPS/log" &&
	truncate -s 256M "$LOOPS/cow" &&
	# The replay disk starts as the cache disk is before the log starts.
	"$CS" copy-sparse "$CACHE_DEV" "$LOOPS/replay" &&
	LOG_DEV=$(ram_disk_attach "$LOOPS/log") &&
	REPLAY_DEV=$(ram_disk_attach "$LOOPS/replay") &&
	COW_DEV=$(ram_disk_attach "$LOOPS/cow") || {
	fail setup "making the loop devices failed"
	exit "$FAILED"
}
if [ "$(sectors "$REPLAY_DEV")" != "$SECTORS" ]; then
	fail setup "the replay disk ($(sectors "$REPLAY_DEV") sectors) is not the cache disk's size ($SECTORS)"
	exit "$FAILED"
fi
"$DMSETUP" create --noudevsync "$LOGW" \
	--table "0 $SECTORS log-writes $CACHE_DEV $LOG_DEV" || {
	fail setup "creating the log-writes device failed"
	exit "$FAILED"
}
mount "/dev/mapper/$LOGW" "$CACHE_DIR" || {
	fail setup "mounting the cache filesystem on the log-writes device failed"
	exit "$FAILED"
}
CACHE_FSTYPE=$(backing_fstype "$CACHE_DIR")
echo "sqlite_durability.sh: cache filesystem $CACHE_FSTYPE, $SECTORS sectors"

DCFS_COUNTERS_FILE=$WORK/counters
export DCFS_COUNTERS_FILE
if start_dcfs traced; then
	pass mount
else
	fail mount "mount.dcfs failed: $(cat "$WORK/mount.out")"
	exit "$FAILED"
fi
# The warm-up: creates until SQLite has checkpointed the WAL (at 1000
# frames, the autocheckpoint) and restarted it (a new salt). From then on
# each frame overwrites an old one in place: no file growth for the
# filesystem's journal to order against, so the disk's reordering of the
# frame writes reaches SQLite, whose recovery must stop at the first frame
# from before the restart (the old salt) or not yet written whole.
stat -f "$MNT" >/dev/null
OLD_SALT=$(wal_field salt)
w=0
while [ "$(wal_field salt)" = "$OLD_SALT" ] && [ "$w" -lt 400 ]; do
	: >"$MNT/d0/w$w"
	stat -f "$MNT" >/dev/null
	w=$((w + 1))
done
FIRST_SALT=$(wal_field salt)
if [ "$FIRST_SALT" != "$OLD_SALT" ]; then
	pass wal-restarted
else
	fail wal-restarted "$w creates did not make SQLite checkpoint and restart the WAL"
fi
LAST_COMMITS=$(wal_field commits)
DB_MD5=$(md5sum "$DB" | cut -d' ' -f1)
echo "SQLITE-DURABILITY warm-up: $w creates, salt $OLD_SALT then $FIRST_SALT, $LAST_COMMITS commits since, WAL $(stat -c %s "$DB-wal") bytes"
set -- $(counters)
echo "0 start 0 $LAST_COMMITS $1 $2 $FIRST_SALT" >"$OPS"
"$DMSETUP" message "$LOGW" 0 mark dcfs-up

# --- the script -----------------------------------------------------------

T0=$(cut -d' ' -f1 /proc/uptime)
"script_$SCRIPT"
echo "SQLITE-DURABILITY ops: n name commits-before commits-after transactions synced-transactions salt"
sed 's/^/SQLITE-DURABILITY op: /' "$OPS"

# The reference: the database and its WAL as dcfs left them (the page cache's
# view: every commit, synced or not).
mkdir -p "$WORK/ref"
cp "$DB" "$DB-wal" "$WORK/ref/"
REF_COMMITS=$("$CS" wal-info "$WORK/ref/dcfs.db-wal" | sed 's/.*commits=\([0-9]*\).*/\1/')
REF_FRAMES=$("$CS" wal-info "$WORK/ref/dcfs.db-wal" | sed 's/frames=\([0-9]*\).*/\1/')
if [ "$REF_COMMITS" = "$LAST_COMMITS" ] && [ "$REF_COMMITS" -gt 0 ]; then
	pass reference-has-every-commit
else
	fail reference-has-every-commit "the reference WAL has $REF_COMMITS commits, the last operation left $LAST_COMMITS"
fi
# No checkpoint ran after the restart (it would have rewritten the database
# file under the reference): one salt since, and the database file as it was.
if [ "$(awk '{ print $7 }' "$OPS" | sort -u | wc -l)" = 1 ] &&
	[ "$(md5sum "$WORK/ref/dcfs.db" | cut -d' ' -f1)" = "$DB_MD5" ]; then
	pass no-checkpoint
else
	fail no-checkpoint "salts $(awk '{ print $7 }' "$OPS" | sort -u | tr '\n' ' '), the database file changed: $(md5sum "$WORK/ref/dcfs.db")"
fi
# Both durability levels exercised: an operation that made a synced commit
# and one that made commits, all normal (dcfs's intent, the cost counter).
# And each is the one dcfs's rules say (op's KIND).
awk 'FNR == NR { kind[$1] = $2; next }
	FNR > 1 {
		got = $6 > prev ? "synced" : ($4 > $3 ? "normal" : "none")
		print $1, $2, (($1 in kind) ? kind[$1] : "-"), got
	}
	{ prev = $6 }' "$WORK/expect" "$OPS" >"$WORK/kinds"
OPS_SYNCED=$(awk '$4 == "synced"' "$WORK/kinds" | wc -l)
OPS_NORMAL=$(awk '$4 == "normal"' "$WORK/kinds" | wc -l)
if [ "$OPS_SYNCED" -ge 1 ] && [ "$OPS_NORMAL" -ge 1 ]; then
	pass both-durability-levels
else
	fail both-durability-levels "$OPS_SYNCED operations made a synced commit and $OPS_NORMAL only normal ones"
fi
sd_wrong=$(awk '$3 != "-" && $3 != $4 { printf "%s (%s): %s, not %s; ", $1, $2, $4, $3 }' "$WORK/kinds")
if [ -z "$sd_wrong" ]; then
	pass durability-as-dcfs-rules-say
else
	fail durability-as-dcfs-rules-say "$sd_wrong"
fi

: >"$REF_FPS"
k=0
while [ "$k" -le "$REF_COMMITS" ]; do
	ref_state "$k" "$WORK/r"
	echo "$k $(fingerprint "$WORK/r")" >>"$REF_FPS"
	k=$((k + 1))
done
if [ "$(awk 'NF == 2' "$REF_FPS" | wc -l)" = $((REF_COMMITS + 1)) ]; then
	pass reference-states-open
else
	fail reference-states-open "$(awk 'NF != 2 { printf "%s ", $1 }' "$REF_FPS")do not open as databases"
fi
T1=$(cut -d' ' -f1 /proc/uptime)
echo "SQLITE-DURABILITY reference: $REF_COMMITS commits, $REF_FRAMES frames, $(cut -d' ' -f2 "$REF_FPS" | sort -u | wc -l) distinct states; script and reference $(awk -v a="$T0" -v b="$T1" 'BEGIN { printf "%.1f", b - a }') s"

# --- the cut --------------------------------------------------------------

"$DMSETUP" message "$LOGW" 0 mark cut
kill_dcfs
wait "$STRACE_CAT"

# Which commits were synced, from the strace: each fsync of the -wal, against
# the operation whose commits it follows. dcfs's intent (the cost counter's
# synced transactions of each operation) must be what SQLite did.
"$CS" wal-commits "$WORK/ref/dcfs.db-wal" >"$WORK/commits"
synced_commits >"$WORK/fsyncs"
awk '
	FNR == NR { if ($2 == "exact" || $2 == "partial") c[++n] = $1; next }
	{
		k = 0
		for (i = 1; i <= n; i++) if (c[i] > $3 && c[i] <= $4) { k++; last = c[i] }
		if (k) print $1, last
		printf "%s %s %d\n", $1, $6 - prev, k >"/dev/stderr"
		prev = $6
	}' "$WORK/fsyncs" "$OPS" >"$WORK/synced" 2>"$WORK/synced.counts"
echo "SQLITE-DURABILITY fsyncs of the WAL (commit, after its last frame or not): $(tr '\n' ' ' <"$WORK/fsyncs")"
echo "SQLITE-DURABILITY synced: op, its last synced commit: $(tr '\n' ' ' <"$WORK/synced")"
if grep -q "split\|partial" "$WORK/fsyncs" || [ ! -s "$WORK/fsyncs" ]; then
	fail wal-fsyncs-follow-commits "an fsync of the WAL did not follow a commit's last frame, or a write was split: $(tr '\n' ' ' <"$WORK/fsyncs")"
else
	pass wal-fsyncs-follow-commits
fi
# (Not the start: its count holds the warm-up's, from before the restart.)
sd_mismatch=$(awk '$1 > 0 && $2 != $3 { printf "op %s: %s synced transactions, %s WAL fsyncs; ", $1, $2, $3 }' "$WORK/synced.counts")
if [ -z "$sd_mismatch" ]; then
	pass synced-transactions-fsync-the-wal
else
	fail synced-transactions-fsync-the-wal "$sd_mismatch"
fi

# --- the oracle's self-checks ----------------------------------------------

# The last operation that made commits and no synced one (two commits at
# least), and the last synced commit of the last operation that made one.
NORMAL_OP=$(awk 'FNR == NR { synced[$1] = 1; next }
	!($1 in synced) && $4 >= $3 + 2 { op = $0 } END { print op }' "$WORK/synced" "$OPS")
set -- $(tail -n 1 "$WORK/synced")
SC_OP=$1
SC_SYNCED=$2
set -- $NORMAL_OP
# The commits j to lose (j + 1, kept, a normal commit): those of that
# operation first, then every other. A forgery whose database fails
# integrity_check (a table page from one commit, an index page from
# another) proves nothing about the oracle, so the next j is tried until
# one opens.
SC_CANDIDATES=$(awk -v first=$(($3 + 1)) -v last="$4" -v n="$REF_COMMITS" '
	FNR == NR { if ($2 == "exact") synced[$1] = 1; next }
	END {
		for (j = first; j < last; j++) if (!((j + 1) in synced)) print j
		for (j = 1; j < n; j++)
			if ((j < first || j >= last) && !((j + 1) in synced)) print j
	}' "$WORK/fsyncs" /dev/null)
sc_fp=""
sc_tried=""
for sc_j in $SC_CANDIDATES; do
	rm -rf "$WORK/forged"
	mkdir -p "$WORK/forged"
	cp "$WORK/ref/dcfs.db" "$WORK/forged/"
	if "$CS" wal-skip "$WORK/ref/dcfs.db-wal" "$sc_j" "$WORK/forged/dcfs.db-wal" >"$WORK/skip.out" 2>&1; then
		sc_fp=$(fingerprint "$WORK/forged" 2>/dev/null)
		[ -n "$sc_fp" ] || sc_tried="$sc_tried $sc_j(corrupt)"
	else
		sc_tried="$sc_tried $sc_j(prefix)"
	fi
	[ -n "$sc_fp" ] && break
done
if [ -n "$sc_fp" ]; then
	sc_verdict=$(oracle "$sc_fp" 0)
	if [ "$sc_verdict" = none ]; then
		pass oracle-refuses-a-commit-kept-after-a-lost-one
		echo "SQLITE-DURABILITY forged: commit $sc_j lost, $((sc_j + 1)) kept ($(cat "$WORK/skip.out")); tried before:${sc_tried:- none}"
	else
		fail oracle-refuses-a-commit-kept-after-a-lost-one "commit $sc_j lost and $((sc_j + 1)) kept ($(cat "$WORK/skip.out")): the oracle said '$sc_verdict'"
	fi
else
	fail oracle-refuses-a-commit-kept-after-a-lost-one "no forgery opens as a database:$sc_tried"
fi
ref_state $((SC_SYNCED - 1)) "$WORK/forged"
sc_verdict=$(oracle "$(fingerprint "$WORK/forged")" "$SC_SYNCED")
case "$sc_verdict" in
lost*) pass oracle-refuses-a-lost-synced-commit ;;
*) fail oracle-refuses-a-lost-synced-commit "the state before synced commit $SC_SYNCED (operation $SC_OP), bound $SC_SYNCED: the oracle said '$sc_verdict'" ;;
esac
ref_state "$SC_SYNCED" "$WORK/forged"
set -- $(oracle "$(fingerprint "$WORK/forged")" "$SC_SYNCED")
if [ "${1:-}" = ok ] && [ "$2" -ge "$SC_SYNCED" ] && [ "$3" -le "$SC_SYNCED" ]; then
	pass oracle-accepts-an-allowed-state
else
	fail oracle-accepts-an-allowed-state "the state after synced commit $SC_SYNCED: the oracle said '$*'"
fi

umount "$CACHE_DIR"
"$DMSETUP" remove --noudevsync "$LOGW"
"$CS" log-list "$LOG_DEV" >"$ENTRIES" || {
	fail log "reading the log failed"
	exit "$FAILED"
}
N=$(wc -l <"$ENTRIES")
# The log's sectors per 4 KiB block, for the torn writes.
LOG_SECTOR=$("$CS" log-info "$LOG_DEV" | sed 's/.*sectorsize=\([0-9]*\).*/\1/')
SPB=$((4096 / LOG_SECTOR))
[ "$SPB" -ge 1 ] || SPB=1
UP=$(mark_index dcfs-up)
CUT=$(mark_index cut)
FLUSHES=$(awk -v up="$UP" '$1 > up && $4 ~ /(^|[|])FLUSH([|]|$)/ { print $1 }' "$ENTRIES")
NFLUSH=$(echo $FLUSHES | wc -w)
echo "SQLITE-DURABILITY log: $N entries, start mark at $UP, $NFLUSH FLUSHes after it, $(grep -c FUA "$ENTRIES") FUA writes"
if [ "$NFLUSH" -lt 2 ]; then
	fail log "fewer than two FLUSHes after the start mark"
	exit "$FAILED"
fi

# --- the replay -----------------------------------------------------------

# replay_to FROM TO: the replay disk from every entry before FROM to every
# entry before TO (discards are skipped by both replayers: a discarded
# range keeps its old content, one of the two it may read back as).
replay_to() {
	[ "$2" -gt "$1" ] || return 0
	"$REPLAY_LOG" --log "$LOG_DEV" --replay "$REPLAY_DEV" --no-discard \
		--start-entry "$1" --limit $(($2 - $1)) >/dev/null
}

: >"$WORK/recovered"
T2=$(cut -d' ' -f1 /proc/uptime)
EPOCHS=$NFLUSH
REMAINING=$BUDGET
CROSS_DONE=0
replay_to 0 "$(echo $FLUSHES | cut -d' ' -f1)" || fail replay "replay-log to the first FLUSH failed"
e=0
set -- $FLUSHES
for flush in $FLUSHES; do
	shift
	next=${1:-$N}
	set -- $(bounds "$flush" "$next") "$@"
	lower=$1
	acked=$2
	upper=$3
	shift 3
	IN_SCRIPT=0
	[ "$next" -lt "$CUT" ] && IN_SCRIPT=1
	check_state "prefix-$flush" "$lower" "$acked" "$upper"
	cap=$((REMAINING / (EPOCHS - e)))
	[ "$cap" -lt "$MIN_CAP" ] && cap=$MIN_CAP
	epoch_states "$ENTRIES" "$flush" "$next" "$cap" "$SEED" "$EXHAUSTIVE" "$SPB" >"$WORK/epoch"
	read -r _ es_fua es_ord es_total es_taken es_mode es_tears <"$WORK/epoch"
	echo "SQLITE-DURABILITY epoch $e [$flush, $next): $es_fua FUA, $es_ord ordinary, $es_taken states ($es_tears torn writes, $es_mode of $((es_fua + es_total)) subsets), bound $lower"
	sed 1d "$WORK/epoch" >"$WORK/epoch.states"
	s=0
	# fd 3, so that nothing the loop runs reads the states as its stdin.
	while read -r kind idx <&3; do
		s=$((s + 1))
		# shellcheck disable=SC2086 # the indices are words
		check_state "state-$flush-$kind-$s" "$lower" "$acked" "$upper" $idx
	done 3<"$WORK/epoch.states"
	REMAINING=$((REMAINING - es_taken))
	[ "$REMAINING" -lt 0 ] && REMAINING=0
	# Once, on the first epoch of the script with two ordinary writes or
	# more: our log-apply of the whole epoch must leave the disk replay-log
	# leaves (same bytes), or the states judged are not the log's.
	cross=0
	if [ "$CROSS_DONE" = 0 ] && [ "$IN_SCRIPT" = 1 ] && [ "$es_ord" -ge 2 ]; then
		cross=1
		CROSS_DONE=1
		"$DMSETUP" create --noudevsync "$SNAP" \
			--table "0 $SECTORS snapshot $REPLAY_DEV $COW_DEV N 8" &&
			"$CS" log-apply "$LOG_DEV" "/dev/mapper/$SNAP" $(seq "$flush" $((next - 1))) &&
			cross_ours=$(md5sum "/dev/mapper/$SNAP" | cut -d' ' -f1)
		"$DMSETUP" remove --noudevsync "$SNAP"
	fi
	replay_to "$flush" "$next" || fail replay "replay-log from $flush to $next failed"
	if [ "$cross" = 1 ]; then
		cross_theirs=$(md5sum "$REPLAY_DEV" | cut -d' ' -f1)
		if [ -n "${cross_ours:-}" ] && [ "$cross_ours" = "$cross_theirs" ]; then
			pass log-apply-agrees-with-replay-log
		else
			fail log-apply-agrees-with-replay-log "epoch [$flush, $next): log-apply ${cross_ours:-failed}, replay-log $cross_theirs"
		fi
	fi
	e=$((e + 1))
done
set -- $(bounds "$N" "$N")
check_state full-log "$1" "$2" "$3"
T3=$(cut -d' ' -f1 /proc/uptime)
echo "SQLITE-DURABILITY replay: $STATES states, $VIOLATIONS violations, $LOST_ACKED lost a commit of a completed operation, $(cut -d' ' -f2 "$WORK/recovered" | sort -un | wc -l) distinct recovered states, smallest margin over the bound $MIN_MARGIN commits, $(awk -v a="$T2" -v b="$T3" 'BEGIN { printf "%.1f", b - a }') s"
echo "SQLITE-DURABILITY recovered (commit: states): $(cut -d' ' -f2 "$WORK/recovered" | sort -n | uniq -c | awk '{ printf "%s:%s ", $2, $1 }')"
echo "SQLITE-DURABILITY reach: $REACH_BEYOND states with this generation's frames past the recovered ones, $REACH_GAP with one after a frame that did not land, $REACH_GAP_SCRIPT of them in the script's epochs (ending before the cut mark)"
[ -n "$REACH_EXAMPLE" ] && echo "SQLITE-DURABILITY reach example: $REACH_EXAMPLE"
if [ "$VIOLATIONS" = 0 ]; then
	pass crash-states-allowed-by-the-model
fi
# What "no commit survives an earlier lost one" needs to mean something: a
# judged state of the script's epochs (where synced commits are written,
# not the unmount's writeback) whose WAL kept a later frame of this
# generation while an earlier one did not land (recovery must stop before
# it).
# Not on btrfs: its data is copy-on-write, so a frame's write goes to a new
# extent that only the log tree's commit, at the epoch's FLUSH, makes part
# of the file, and every state of an epoch recovers the same database.
if [ "$CACHE_FSTYPE" = btrfs ]; then
	skip some-state-kept-a-later-frame "btrfs writes the WAL copy-on-write: no frame of an epoch reaches the file before its FLUSH ($REACH_GAP states held a frame past a gap)"
elif [ "$REACH_GAP_SCRIPT" -gt 0 ]; then
	pass some-state-kept-a-later-frame
else
	fail some-state-kept-a-later-frame "no judged state of an epoch before the cut mark held a WAL frame past one that did not land ($REACH_BEYOND states held frames past the recovered ones, $REACH_GAP after a gap, all at the unmount): the reordering of the WAL's writes never reached SQLite"
fi
if [ "$LOST_ACKED" -gt 0 ]; then
	pass some-state-lost-a-normal-commit
else
	fail some-state-lost-a-normal-commit "no crash state lost a commit of a completed operation: the replay did not exercise what the abstraction allows"
fi

# --- the positive case ----------------------------------------------------

FULL=$(awk '$1 == "full-log" { print $2 }' "$WORK/recovered")
if [ "$FULL" = "$REF_COMMITS" ]; then
	pass full-log-recovers-the-final-state
else
	fail full-log-recovers-the-final-state "the whole log recovered the state after commit '$FULL' of $REF_COMMITS"
fi
mount -t "$CACHE_FSTYPE" "$REPLAY_DEV" "$CACHE_DIR" || fail restart-mount "mounting the replayed cache failed"
# The recovery, read from the database (syslog is asynchronous): the dirty
# inodes whose attributes are valid before the start, and after it, before
# any request touches them (RecoverDirty makes them unknown).
dirty_valid() {
	"$TESTUTIL" sql "$DB" "SELECT count(*) FROM inodes WHERE attrs_valid = 1 AND id IN (SELECT inode FROM dirty)"
}
DIRTY_ROWS=$("$TESTUTIL" sql "$DB" "SELECT count(*) FROM dirty")
VALID_BEFORE=$(dirty_valid)
if start_dcfs; then
	pass restart
	VALID_AFTER=$(dirty_valid)
	if [ "${DIRTY_ROWS:-0}" -ge 1 ] && [ "$VALID_AFTER" = 0 ]; then
		pass restart-recovers-dirty-rows
	else
		fail restart-recovers-dirty-rows "$DIRTY_ROWS dirty rows; dirty inodes with valid attributes: $VALID_BEFORE before the start, $VALID_AFTER after"
	fi
	echo "SQLITE-DURABILITY restart: $DIRTY_ROWS dirty rows, dirty inodes with valid attributes $VALID_BEFORE before the start, $VALID_AFTER after"
	drop_caches
	snapshot "$SRC" >"$WORK/backing.snap"
	snapshot "$MNT" >"$WORK/served.snap" 2>&1
	if command diff "$WORK/served.snap" "$WORK/backing.snap" >"$WORK/snap.diff" 2>&1; then
		pass restart-serves-the-backing-filesystem
	else
		fail restart-serves-the-backing-filesystem "served (<) and backing (>) differ: $(tr '\n' '|' <"$WORK/snap.diff")"
	fi
	umount "$MNT"
	flock "$DB" true
	DPID=""
else
	fail restart "mount.dcfs failed: $(cat "$WORK/mount.out")"
fi
umount "$CACHE_DIR"

exit "$FAILED"
