#ifndef DCFS_CONTEXT_H_
#define DCFS_CONTEXT_H_

#include <cstdint>

#include "absl/container/flat_hash_set.h"
#include "absl/status/statusor.h"
#include "dcfs/mount_fds.h"
#include "dcfs/sqlite.h"

namespace dcfs {

// Everything a dcfs operation may touch, passed explicitly as the first
// argument of every cache/backing-layer call instead of living in globals.
// Keeping it explicit means there is exactly one place per thread (later:
// per coroutine runner) that owns this state, and tests can build one
// around an in-memory database without any setup beyond constructing it.
//
// Non-owning: the Connection and MountFds must outlive the Context.
struct Context {
  sqlite3::Connection &db;
  MountFds &mounts;
  // The inode ids (cache::InodeId) that currently have at least one
  // writable open outstanding, owned by DirCacheFS (which points this at
  // its own set in its constructor); null means none (e.g. in unit tests).
  // While an inode is in here the kernel may be writing to it behind the
  // cache's back (FUSE passthrough), so the backing layer keeps its cached
  // attributes marked unknown whenever it records fresh ones (see
  // backing::RefreshAttrsFromFd and the README's "Crash robustness").
  const absl::flat_hash_set<int64_t> *open_for_write = nullptr;
};

}  // namespace dcfs

#endif  // DCFS_CONTEXT_H_
