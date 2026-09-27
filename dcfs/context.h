#ifndef DCFS_CONTEXT_H_
#define DCFS_CONTEXT_H_

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
};

}  // namespace dcfs

#endif  // DCFS_CONTEXT_H_
