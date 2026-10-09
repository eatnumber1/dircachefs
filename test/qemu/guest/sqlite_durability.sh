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
# that changes nothing): k is the last of them. A violation fails, naming
# the state, its write subset and the fingerprint.
#
# Self-checks of the oracle, before the replay: a WAL with a commit lost and
# the next one kept (crash_states wal-skip: a state no real power loss
# leaves) must match no state; the state before the last synced commit,
# judged against it as the bound, must be refused; the state after it must
# be accepted.
#
# The positive case: the whole log recovers exactly the final reference
# state, and dcfs started on it recovers (StartRun) to what the backing
# filesystem holds.
#
# Run as /tests/sqlite_durability.sh by guest/init; one "TEST ... PASS/FAIL"
# line per check, and the measurements on SQLITE-DURABILITY lines.
FAILED=0
. "$(dirname "$0")/lib.sh"

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

# loop FILE: attaches FILE to a free loop device and prints its name.
loop() {
	l_dev=$(losetup -f) && losetup "$l_dev" "$1" && echo "$l_dev"
}

# --- dcfs -------------------------------------------------------------------

# start_dcfs [traced]: mount.dcfs daemonized: it returns once dcfs has
# answered FUSE_INIT (no wait of ours). Sets DPID. With "traced", under
# strace -D (the tracer a detached grandchild, so the shell waits for the
# wrapper alone), recording the daemon's writes and fsyncs of files into
# the FIFO $WORK/strace.fifo, which a cat of ours (STRACE_CAT) copies to
# $WORK/strace.out until the tracer exits with the daemon.
STRACE_CAT=""
start_dcfs() {
	sd_opts="dcfs.fstype=none,dcfs.cache_db=$DB,dcfs.sync_interval_sec=3600"
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
	# its every commit, whatever STATFS's own end writes.
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
op() {
	o_name=$1
	shift
	"$@" >/dev/null 2>"$WORK/op.err" ||
		fail "op-$o_name" "$* failed: $(cat "$WORK/op.err")"
	record "$o_name"
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
# point; a create's writable open is a phase 1 of the new file's); the ops
# table and the strace say what it decided. Each ends with an unlink of a
# file both of whose inodes are durably dirty: normal commits only, which
# reach the disk only when the cache is unmounted, and a FLUSH after it
# (cache_flush), so that some crash states lose a completed operation (and
# a commit that changed the database: the release records after a create's
# last synced commit can leave it as it was).
script_full() {
	op warm ls "$MNT/d1" "$MNT/d2" "$MNT/d3" "$MNT/d4" "$MNT/d5"
	op create-d1-a creates "$MNT/d1" a
	op create-d1-b creates "$MNT/d1" b
	op mkdir-d2-x mkdir "$MNT/d2/x"
	op create-d3-f1-f4 creates "$MNT/d3" f1 f2 f3 f4
	op unlink-d1-a rm "$MNT/d1/a"
	op rename-d1-b-c mv "$MNT/d1/b" "$MNT/d1/c"
	op wal-writeback "$TESTUTIL" fsync "$DB-wal"
	op create-d3-g1-g2 creates "$MNT/d3" g1 g2
	op sync-point "$TESTUTIL" fsync "$MNT/d3/g1"
	op create-d3-g3 creates "$MNT/d3" g3
	op write-d4-w write_file "$MNT/d4/w"
	op chmod-d4-keep chmod 600 "$MNT/d4/keep"
	op setxattr-d4-keep "$TESTUTIL" setxattr "$MNT/d4/keep" user.k v
	op rmdir-d2-x rmdir "$MNT/d2/x"
	op create-d5-t1-t6 creates "$MNT/d5" t1 t2 t3 t4 t5 t6
	op unlink-d5-t1 rm "$MNT/d5/t1"
	op cache-flush cache_flush
}

script_short() {
	op warm ls "$MNT/d1" "$MNT/d3"
	op create-d1-a creates "$MNT/d1" a
	op create-d1-b creates "$MNT/d1" b
	op unlink-d1-a rm "$MNT/d1/a"
	op sync-point "$TESTUTIL" fsync "$MNT/d1/b"
	op create-d3-t1-t3 creates "$MNT/d3" t1 t2 t3
	op unlink-d3-t1 rm "$MNT/d3/t1"
	op cache-flush cache_flush
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

# oracle FINGERPRINT LOWER: "ok K", K the last commit whose state has the
# fingerprint, if K >= LOWER; "lost K" if every such K is below LOWER; "none"
# if no state has it.
oracle() {
	awk -v fp="$1" -v lower="$2" '
		$2 == fp { found = 1; if ($1 + 0 > best) best = $1 + 0 }
		END {
			if (!found) print "none"
			else if (best >= lower) print "ok", best
			else print "lost", best
		}' best=-1 "$REF_FPS"
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
# its commit frame. A line "split <n>" is a syscall strace printed in two
# halves (two threads at once), which would hide an offset.
synced_commits() {
	awk '
		FNR == NR { end[$1] = $2; n = $1; next }
		/unfinished|resumed/ { print "split", FNR; next }
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

# bounds INDEX: "<lower> <acknowledged>" for the states at or after the log
# entry INDEX (a FLUSH). <lower> is the commit every such state must have:
# the last synced commit of the operations whose marks are logged before
# INDEX (each acknowledged before its operation's mark, so before any crash
# at or after INDEX). <acknowledged> is the last commit of those operations.
bounds() {
	awk -v at="$1" '
		FILENAME == ARGV[1] { if ($4 ~ /MARK/) mark[$5] = $1; next }
		FILENAME == ARGV[2] { synced_op[$1] = 1; synced[$1] = $2; next }
		{ m = $1 == 0 ? "dcfs-up" : "op" $1 }
		m in mark && mark[m] < at {
			if ($1 in synced_op && synced[$1] > lo) lo = synced[$1]
			if ($4 > acked) acked = $4
		}
		END { print lo + 0, acked + 0 }' "$ENTRIES" "$WORK/synced" "$OPS"
}

STATES=0
VIOLATIONS=0
LOST_ACKED=0
MIN_MARGIN=""
# check_state NAME LOWER ACKED [INDEX...]: replays the entries INDEX... onto
# a snapshot of the replay disk (as it is: every entry before the epoch's
# FLUSH), recovers it and asks the oracle (LOWER: the bound; ACKED: the last
# commit of an operation done before the crash, for the count of states that
# lost one); a violation fails NAME.
check_state() {
	cs_name=$1
	cs_lower=$2
	cs_acked=$3
	shift 3
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
	cs_fp=$(fingerprint "$WORK/st" 2>"$WORK/fp.err")
	if [ -z "$cs_fp" ]; then
		VIOLATIONS=$((VIOLATIONS + 1))
		fail "$cs_name" "the database does not open as a database: $(cat "$WORK/fp.err"); entries: $*"
		return
	fi
	cs_verdict=$(oracle "$cs_fp" "$cs_lower")
	echo "$cs_name ${cs_verdict#* }" >>"$WORK/recovered"
	case "$cs_verdict" in
	ok*)
		[ "${cs_verdict#ok }" -lt "$cs_acked" ] && LOST_ACKED=$((LOST_ACKED + 1))
		cs_margin=$((${cs_verdict#ok } - cs_lower))
		if [ -z "$MIN_MARGIN" ] || [ "$cs_margin" -lt "$MIN_MARGIN" ]; then
			MIN_MARGIN=$cs_margin
		fi
		;;
	lost*)
		VIOLATIONS=$((VIOLATIONS + 1))
		fail "$cs_name" "an acknowledged synced commit was lost: recovered the state after commit ${cs_verdict#lost }, the bound is $cs_lower; entries: $*"
		;;
	*)
		VIOLATIONS=$((VIOLATIONS + 1))
		fail "$cs_name" "the recovered database ($cs_fp) is the state after no commit: a commit survived an earlier lost one; entries: $*"
		;;
	esac
}

# epoch_states FROM TO CAP: the write subsets of the epoch [FROM, TO) of the
# log, one per line ("<kind> <index>..."), and a first line "COV <fua>
# <ordinary> <subsets> <taken> <exhaustive|sampled>".
epoch_states() {
	awk -v from="$1" -v to="$2" -v cap="$3" -v seed="$SEED" -v all="$EXHAUSTIVE" '
		$1 >= from && $1 < to && $3 > 0 && $4 !~ /MARK/ && $4 !~ /DISCARD/ {
			if ($4 ~ /FUA/) fua[a++] = $1
			else { sectors[b] = $3; ord[b++] = $1 }
		}
		function emit(line) { if (!(line in seen)) { seen[line] = 1; out[n++] = line } }
		END {
			srand(seed + from)
			allf = ""
			for (i = 0; i < a; i++) allf = allf " " fua[i]
			# Every prefix of the FUA writes, no ordinary write.
			for (p = 1; p <= a; p++) {
				line = ""
				for (i = 0; i < p; i++) line = line " " fua[i]
				emit("fua" line)
			}
			# A torn write: all FUA writes, and one ordinary write of more
			# than one 4 KiB block of which only some blocks landed (its
			# first, its first half, all but its last, its last, all but its
			# first), with every other ordinary write or with none.
			tears = 0
			for (k = 0; k < b; k++) {
				nb = int(sectors[k] / 8)
				if (nb < 2) continue
				cut[1] = "0-8"; cut[2] = "0-" 8 * int(nb / 2)
				cut[3] = "0-" 8 * (nb - 1); cut[4] = 8 * (nb - 1) "-" 8 * nb
				cut[5] = "8-" 8 * nb
				for (c = 1; c <= 5; c++) {
					others = ""
					for (i = 0; i < b; i++) if (i != k) others = others " " ord[i]
					before = n
					emit("tear" allf others " " ord[k] ":" cut[c])
					emit("tearalone" allf " " ord[k] ":" cut[c])
					tears += n - before
				}
			}
			# All FUA writes, a proper nonempty subset of the ordinary ones
			# (none: the last FUA prefix; all: the next FLUSH prefix).
			total = b >= 2 ? 2 ^ b - 2 : 0
			if (total <= cap || total <= all) {
				mode = "exhaustive"
				for (mask = 1; mask < 2 ^ b - 1; mask++) {
					line = "subset" allf
					for (i = 0; i < b; i++)
						if (int(mask / 2 ^ i) % 2) line = line " " ord[i]
					emit(line)
				}
			} else {
				mode = "sampled"
				# The share is for subsets: the torn writes come on top.
				for (k = 0; k < b && n - tears < cap; k++) {
					line = "lose1" allf
					for (i = 0; i < b; i++) if (i != k) line = line " " ord[i]
					emit(line)
				}
				for (k = 0; k < b && n - tears < cap; k++) emit("keep1" allf " " ord[k])
				for (tries = 0; n - tears < cap && tries < 50 * cap; tries++) {
					line = "random" allf
					got = 0
					for (i = 0; i < b; i++) if (rand() < 0.5) { line = line " " ord[i]; got++ }
					if (got > 0 && got < b) emit(line)
				}
			}
			print "COV", a + 0, b + 0, total, n + 0, mode, tears + 0
			for (i = 0; i < n; i++) print out[i]
		}' "$ENTRIES"
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
	LOG_DEV=$(loop "$LOOPS/log") &&
	REPLAY_DEV=$(loop "$LOOPS/replay") &&
	COW_DEV=$(loop "$LOOPS/cow") || {
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
OPS_SYNCED=$(awk '$6 > prev && NR > 1 { n++ } { prev = $6 } END { print n + 0 }' "$OPS")
OPS_NORMAL=$(awk '$6 == prev && $4 > $3 && NR > 1 { n++ } { prev = $6 } END { print n + 0 }' "$OPS")
if [ "$OPS_SYNCED" -ge 3 ] && [ "$OPS_NORMAL" -ge 1 ]; then
	pass both-durability-levels
else
	fail both-durability-levels "$OPS_SYNCED operations made a synced commit and $OPS_NORMAL only normal ones"
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
sc_first=$(($3 + 1))
sc_last=$4
sc_j=$sc_first
sc_done=0
while [ "$sc_j" -lt "$sc_last" ] && [ "$sc_done" = 0 ]; do
	rm -rf "$WORK/forged"
	mkdir -p "$WORK/forged"
	cp "$WORK/ref/dcfs.db" "$WORK/forged/"
	if "$CS" wal-skip "$WORK/ref/dcfs.db-wal" "$sc_j" "$WORK/forged/dcfs.db-wal" >"$WORK/skip.out" 2>&1; then
		sc_done=1
	else
		sc_j=$((sc_j + 1))
	fi
done
if [ "$sc_done" = 1 ]; then
	sc_fp=$(fingerprint "$WORK/forged")
	sc_verdict=$(oracle "$sc_fp" 0)
	if [ "$sc_verdict" = none ]; then
		pass oracle-refuses-a-commit-kept-after-a-lost-one
	else
		fail oracle-refuses-a-commit-kept-after-a-lost-one "commit $sc_j lost and $((sc_j + 1)) kept ($(cat "$WORK/skip.out")): the oracle said '$sc_verdict'"
	fi
else
	fail oracle-refuses-a-commit-kept-after-a-lost-one "no commit of operation '$NORMAL_OP' writes a page the next one does not"
fi
ref_state $((SC_SYNCED - 1)) "$WORK/forged"
sc_verdict=$(oracle "$(fingerprint "$WORK/forged")" "$SC_SYNCED")
case "$sc_verdict" in
lost*) pass oracle-refuses-a-lost-synced-commit ;;
*) fail oracle-refuses-a-lost-synced-commit "the state before synced commit $SC_SYNCED (operation $SC_OP), bound $SC_SYNCED: the oracle said '$sc_verdict'" ;;
esac
ref_state "$SC_SYNCED" "$WORK/forged"
sc_verdict=$(oracle "$(fingerprint "$WORK/forged")" "$SC_SYNCED")
case "$sc_verdict" in
ok*) if [ "${sc_verdict#ok }" -ge "$SC_SYNCED" ]; then
	pass oracle-accepts-an-allowed-state
else
	fail oracle-accepts-an-allowed-state "the state after synced commit $SC_SYNCED: the oracle said '$sc_verdict'"
fi ;;
*) fail oracle-accepts-an-allowed-state "the state after synced commit $SC_SYNCED (operation $SC_OP), bound $SC_SYNCED: the oracle said '$sc_verdict'" ;;
esac

umount "$CACHE_DIR"
"$DMSETUP" remove --noudevsync "$LOGW"
"$CS" log-list "$LOG_DEV" >"$ENTRIES" || {
	fail log "reading the log failed"
	exit "$FAILED"
}
N=$(wc -l <"$ENTRIES")
UP=$(mark_index dcfs-up)
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
replay_to 0 "$(echo $FLUSHES | cut -d' ' -f1)" || fail replay "replay-log to the first FLUSH failed"
e=0
set -- $FLUSHES
for flush in $FLUSHES; do
	shift
	next=${1:-$N}
	set -- $(bounds "$flush") "$@"
	lower=$1
	acked=$2
	shift 2
	check_state "prefix-$flush" "$lower" "$acked"
	cap=$((REMAINING / (EPOCHS - e)))
	[ "$cap" -lt "$MIN_CAP" ] && cap=$MIN_CAP
	epoch_states "$flush" "$next" "$cap" >"$WORK/epoch"
	read -r _ es_fua es_ord es_total es_taken es_mode es_tears <"$WORK/epoch"
	echo "SQLITE-DURABILITY epoch $e [$flush, $next): $es_fua FUA, $es_ord ordinary, $es_taken states ($es_tears torn writes, $es_mode of $((es_fua + es_total)) subsets), bound $lower"
	sed 1d "$WORK/epoch" >"$WORK/epoch.states"
	s=0
	# fd 3, so that nothing the loop runs reads the states as its stdin.
	while read -r kind idx <&3; do
		s=$((s + 1))
		# shellcheck disable=SC2086 # the indices are words
		check_state "state-$flush-$kind-$s" "$lower" "$acked" $idx
	done 3<"$WORK/epoch.states"
	REMAINING=$((REMAINING - es_taken))
	[ "$REMAINING" -lt 0 ] && REMAINING=0
	replay_to "$flush" "$next" || fail replay "replay-log from $flush to $next failed"
	e=$((e + 1))
done
set -- $(bounds "$N")
check_state full-log "$1" "$2"
T3=$(cut -d' ' -f1 /proc/uptime)
echo "SQLITE-DURABILITY replay: $STATES states, $VIOLATIONS violations, $LOST_ACKED lost a commit of a completed operation, $(cut -d' ' -f2 "$WORK/recovered" | sort -un | wc -l) distinct recovered states, smallest margin over the bound $MIN_MARGIN commits, $(awk -v a="$T2" -v b="$T3" 'BEGIN { printf "%.1f", b - a }') s"
echo "SQLITE-DURABILITY recovered (commit: states): $(cut -d' ' -f2 "$WORK/recovered" | sort -n | uniq -c | awk '{ printf "%s:%s ", $2, $1 }')"
if [ "$VIOLATIONS" = 0 ]; then
	pass crash-states-allowed-by-the-model
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
RECOVERIES=$(logread | grep -c "recovered [1-9][0-9]* dirty")
if start_dcfs; then
	pass restart
	if [ "$(logread | grep -c "recovered [1-9][0-9]* dirty")" -gt "$RECOVERIES" ]; then
		pass restart-recovers-dirty-rows
	else
		fail restart-recovers-dirty-rows "no recovery WARNING naming dirty rows: $(logread | grep -i recover | tail -n 3)"
	fi
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
