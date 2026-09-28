#ifndef DCFS_CONTEXT_H_
#define DCFS_CONTEXT_H_

#include <cstdint>
#include <string>

#include "absl/container/flat_hash_set.h"
#include "absl/random/bit_gen_ref.h"
#include "absl/status/statusor.h"
#include "dcfs/mount_fds.h"
#include "dcfs/sqlite.h"

namespace dcfs {

// In-memory bookkeeping for the durable dirty set (the `dirty` table; see
// schema.sql and the README's "Crash robustness"). Only cache::BeginMutation,
// cache::MarkDirty, cache::ClearDirty and cache::RecoverDirty change it.
struct DirtyState {
  // Whether the dirty table may be non-empty. Conservatively true until
  // startup recovery or a sync point has emptied it; lets the periodic
  // sync (DirCacheFS::MaybeSyncBacking) skip idle periods without a query
  // per request.
  bool any = true;
  // Inodes whose dirty row is known to be durable: committed with
  // sqlite3::Durability::kSync since the table was last cleared. A phase 1
  // touching only these needs no WAL fsync of its own, since startup
  // recovery will treat all of their cached state as unknown anyway (see
  // cache::BeginMutation).
  absl::flat_hash_set<int64_t> durable;
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
  // Owned here (unlike the members above): per-connection state that must
  // stay in step with ctx.db's dirty table.
  DirtyState dirty;
};

}  // namespace dcfs

#endif  // DCFS_CONTEXT_H_
