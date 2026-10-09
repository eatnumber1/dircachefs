#ifndef DCFS_CONTEXT_H_
#define DCFS_CONTEXT_H_

#include <cstddef>
#include <cstdint>
#include <string_view>

#include "absl/container/flat_hash_map.h"
#include "absl/container/flat_hash_set.h"
#include "absl/random/bit_gen_ref.h"
#include "absl/status/statusor.h"
#include "absl/time/clock_interface.h"
#include "absl/time/time.h"
#include "dcfs/interrupts.h"
#include "dcfs/mount_fds.h"
#include "dcfs/protocol_events.h"
#include "dcfs/sqlite.h"

namespace dcfs {

// In-memory bookkeeping for the durable dirty set (the `dirty` table; see
// schema.sql and the README's "Crash robustness"). Only cache::BeginMutation,
// cache::MarkDirty, cache::ClearDirty and cache::RecoverDirty change it.
struct DirtyState {
  // Whether the dirty table may hold a row that is not atime_only (a
  // mutation's). Conservatively true until startup recovery or a sync point
  // has emptied it; lets the periodic sync (DirCacheFS::MaybeSyncBacking)
  // skip idle periods without a query per request. Only these rows make a
  // sync point run (step 23.8): an atime_only row waits for one that runs
  // for another reason, for the clean shutdown's, or for recovery.
  bool any = true;
  // Whether it may hold an atime_only row (cache::MarkAtimeDirty), likewise
  // conservative: a clean shutdown needs both false.
  bool atime = true;
  // How many atime-only inserts (cache::MarkAtimeDirty) were made: a sync
  // point's ClearDirty takes its fast path only if none was made since its
  // BeginSync (a cold open's insert advances no fill guard).
  uint64_t inserts = 0;
  // How old an atime-only row may grow before it drives a sync point after
  // all: the kernel's dirtytime expiry (/proc/sys/vm/dirtytime_expire_seconds,
  // read at startup), after which it writes access times back anyway.
  absl::Duration atime_expiry = absl::Hours(12);
  // When the oldest atime-only row may have been added (InfiniteFuture: none
  // since a sync point or recovery left none): with atime_expiry, when they
  // drive a sync point (DirCacheFS::MaybeSyncBacking).
  absl::Time atime_since = absl::InfiniteFuture();
  // Inodes whose dirty row is known to be durable: committed with
  // sqlite3::Durability::kSync since the table was last cleared. A phase 1
  // touching only these needs no WAL fsync of its own, since startup
  // recovery will treat all of their cached state as unknown anyway (see
  // cache::BeginMutation).
  absl::flat_hash_set<int64_t> durable;
};

// In-memory bookkeeping that keeps a cache fill (a population: reading the
// backing filesystem, then recording what it read as present) from
// overwriting a newer mutation's result, or caching data read while a
// mutation was changing it (audit-tristate F1). Only cache::BeginMutation,
// cache::Mutation and cache::BeginFill/CanFill use it; see them.
//
// Deliberately not in the database: it guards only in-process concurrency
// (a crash makes everything a mutation touched unknown through the dirty
// set anyway), and a fill must be able to check a row it did not know about
// when it started (PopulateDirectory's children).
struct FillGuards {
  // A logical clock, advanced by every mutation's phase 1 and end.
  uint64_t seq = 0;
  // Snapshots older than this are invalid for every inode (set when
  // `touched` is pruned).
  uint64_t floor = 0;
  // Inode id -> number of mutations between their phase 1 and their end.
  // Entries are erased when they reach 0.
  absl::flat_hash_map<int64_t, int> inflight;
  // Inode id -> `seq` at the latest phase 1 or end of a mutation of it.
  // Pruned (cleared, raising `floor`) when it would grow past
  // `max_touched` entries.
  absl::flat_hash_map<int64_t, uint64_t> touched;
  // The prune bound: memory for `touched` against how often a prune makes
  // the fills running at that moment skip caching (and a sync point
  // running then keep every dirty row). A setting rather than a constant:
  // the bound trades memory against how often a prune happens.
  size_t max_touched = size_t{1} << 16;
};

// When the backing filesystem updates an access time on a read (its
// mount's atime option: statvfs's ST_NOATIME and ST_RELATIME). dcfs applies
// it to the reads it serves from the cache itself, a directory's listing and
// a symlink's target (step 23.8; cache::TouchAtime), and records the result
// in the cache only. A regular file's reads go through passthrough and the
// backing filesystem stamps them; dcfs reads that back (DirCacheFS's held
// fills) and predicts nothing.
enum class AtimePolicy {
  kRelative,  // relatime (the default): if older than mtime or ctime, or
              // more than a day old
  kStrict,    // strictatime: on every read
  kNever,     // noatime
};

// Everything a dcfs operation may touch, passed explicitly as the first
// argument of every cache/backing-layer call instead of living in globals.
// Keeping it explicit means there is exactly one place per thread (later:
// per coroutine runner) that owns this state, and tests can build one
// around an in-memory database without any setup beyond constructing it.
//
// Non-owning: the Connection, MountFds and random generator must outlive
// the Context.
struct Context {
  sqlite3::Connection &db;
  MountFds &mounts;
  // The source of every new row's FUSE generation (cache::UpsertInode): a
  // uniformly random 32-bit value, never 0 (the root's). Owned by whoever
  // builds the Context (main() owns an absl::BitGen; tests may pass a
  // seeded one).
  absl::BitGenRef rng;
  // The inode ids (cache::InodeId) that currently have at least one
  // writable open outstanding, owned by DirCacheFS (which points this at
  // its own set in its constructor); null means none (e.g. in unit tests).
  // While an inode is in here the kernel may be writing to it behind the
  // cache's back (FUSE passthrough), so the backing layer keeps its cached
  // attributes marked unknown whenever it records fresh ones (see
  // backing::RefreshAttrsFromFd and the README's "Crash robustness").
  const absl::flat_hash_set<int64_t> *open_for_write = nullptr;
  // The inode ids that currently have an open backing file (a passthrough
  // open, read-only or not: DirCacheFS::backing_files_), owned by DirCacheFS
  // likewise; null means none. The kernel may read such a file at any
  // moment, and the backing filesystem then changes its access time in
  // memory and writes it back lazily, so (step 23.8) any record of its
  // attributes marks its row dirty, atime only (cache::MarkAtimeDirty), and
  // a sync point keeps its dirty row (backing::SyncBacking).
  const absl::flat_hash_set<int64_t> *open_files = nullptr;
  // Owned here (unlike the members above): per-connection state that must
  // stay in step with ctx.db's dirty table.
  DirtyState dirty;
  FillGuards fills;
  // The source filesystem's atime policy, from its mount options
  // (backing::InitRoot).
  AtimePolicy atime = AtimePolicy::kRelative;
  // Whether the source filesystem was read-only when this run started
  // (backing::InitRoot) and no sync point has found it writable since. If
  // not, a sync point that finds it read-only fails (backing::SyncBacking,
  // steps 11.5, 11.5b).
  bool source_read_only_at_start = false;
  // The protocol events (dcfs/protocol_events.h): records nothing in
  // production; trace validation's recorder in the testonly builds. Never
  // null; not owned.
  ProtocolEvents *events = &NoProtocolEvents();
  // Whether the request being served was interrupted (dcfs/interrupts.h):
  // SessionLoop in the daemon, never in unit tests unless they set one.
  // Never null; not owned.
  Interrupts *interrupts = &NoInterrupts();
  // The clock every time-based decision reads (the periodic sync point,
  // relatime at a listing or readlink): the real clock in production, an
  // absl::SimulatedClock in tests. Never null; not owned. Production code
  // reads the time nowhere else (tools/banned_symbols.txt).
  absl::Clock *clock = &absl::Clock::GetRealClock();
  // How often a request reached the backing filesystem so far (each
  // ProtocolEvents::BackingCall site), and the first such call since
  // `first_backing_call` was last cleared. The request handler (fuse_ops.cc)
  // reads the difference around a request for its `--v=1` line and for the
  // INFO line about the first backing access after an idle period.
  // `first_backing_call` is a view of the string literal each BackingCall
  // site passes, so it is safe to keep. `first_backing_at` is the clock then
  // (the idle line's time), read only for a request's first call.
  uint64_t backing_calls = 0;
  std::string_view first_backing_call;
  absl::Time first_backing_at;
  void NoteBackingCall(std::string_view what) {
    ++backing_calls;
    if (first_backing_call.empty()) {
      first_backing_call = what;
      first_backing_at = clock->TimeNow();
    }
  }
};

// Installs `events` (never null; not owned) as `ctx`'s observer: its
// Context::events, and the observer of its database's statements and
// transactions (sqlite3::Connection::set_observer, the cost counters).
inline void Observe(Context &ctx, ProtocolEvents *events) {
  ctx.events = events;
  ctx.db.set_observer(events);
}

}  // namespace dcfs

#endif  // DCFS_CONTEXT_H_
