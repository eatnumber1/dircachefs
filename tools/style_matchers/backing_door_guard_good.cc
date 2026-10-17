// Known-good input of backing_door_guard.query: a backing wrapper whose body
// passes the backing door before its syscall, and a function outside
// dcfs::syscalls, which needs no door.
#include <unistd.h>

#include "absl/status/status.h"

namespace dcfs {

absl::Status BackingDoor();

namespace syscalls {

absl::Status fdatasync(int fd) {
  if (absl::Status door = BackingDoor(); !door.ok()) return door;
  if (::fdatasync(fd) == -1) return absl::InternalError("fdatasync");
  return absl::OkStatus();
}

}  // namespace syscalls

int NotAWrapper(int x) { return x + 1; }

}  // namespace dcfs
