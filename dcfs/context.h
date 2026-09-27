#ifndef DCFS_CONTEXT_H_
#define DCFS_CONTEXT_H_

#include "absl/status/statusor.h"
#include "dcfs/device_id.h"
#include "dcfs/mount_fds.h"
#include "dcfs/sqlite.h"

namespace dcfs {

// How the backing layer identifies the filesystem an fd is on; see
// Context::device_id_fn.
using DeviceIdFn = absl::StatusOr<DeviceId> (*)(int fd);

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
  // Production never changes this: filesystem identity is always
  // FS_IOC_GETFSUUID (GetDeviceId). It is a member only so that tests on
  // kernels without that ioctl (it arrived in Linux 6.9) can inject a fake.
  DeviceIdFn device_id_fn = &GetDeviceId;

  // Set by backing::OpenNode the first time open_by_handle_at fails EPERM
  // (this process lacks CAP_DAC_READ_SEARCH): remembers that so every later
  // OpenNode call goes straight to walking cached dentry names instead of
  // retrying a call already known to fail. Process-local runtime state, not
  // configuration -- it is never read from or written to the database, and
  // starts fresh (false) every time the daemon restarts.
  bool open_by_handle_denied = false;
};

}  // namespace dcfs

#endif  // DCFS_CONTEXT_H_
