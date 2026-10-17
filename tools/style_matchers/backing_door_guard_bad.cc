// Known-bad input of backing_door_guard.query: a backing wrapper (in
// dcfs::syscalls, in a file the matcher reads as syscalls_backing's) that
// makes its syscall without passing the backing door.
#include <unistd.h>

#include "absl/status/status.h"

namespace dcfs {

absl::Status BackingDoor();

namespace syscalls {

absl::Status fsync(int fd) {  // HIT
  if (::fsync(fd) == -1) return absl::InternalError("fsync");
  return absl::OkStatus();
}

}  // namespace syscalls
}  // namespace dcfs
