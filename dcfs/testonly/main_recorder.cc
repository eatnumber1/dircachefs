// MainProtocolEvents() for the testonly recording build of the daemon
// (//dcfs:main_static_traced): a TraceRecorder writing to $DCFS_TRACE_FILE,
// or to /dev/console (the QEMU guest's serial log, which outlives a killed
// daemon and is where the host collects the trace) if that is unset. The
// trace is named $DCFS_TRACE_ID, or "e2e"; with the invariant checker and
// the cost counter of the checking build (testonly/checking_observers.h:
// a traced run is checked too). See dcfs/protocol_events.h.

#include <fcntl.h>

#include <cstdlib>
#include <string>
#include <utility>

#include "absl/log/check.h"
#include "absl/status/statusor.h"
#include "dcfs/fd.h"
#include "dcfs/protocol_events.h"
#include "dcfs/syscalls_backing.h"
#include "dcfs/testonly/checking_observers.h"
#include "dcfs/testonly/observers.h"
#include "dcfs/testonly/trace_recorder.h"

namespace dcfs {

ProtocolEvents &MainProtocolEvents() {
  static testonly::Observers *observers = [] {
    const char *path = std::getenv("DCFS_TRACE_FILE");
    if (path == nullptr || *path == '\0') path = "/dev/console";
    absl::StatusOr<FileDescriptor> opened = syscalls::openat(
        AT_FDCWD, path, O_WRONLY | O_APPEND | O_CREAT | O_NOCTTY, 0600);
    CHECK_OK(opened) << "opening the trace file " << path;
    // Never closed: the recorder lives for the life of the process.
    const int fd = std::move(*opened).Release();
    const char *id = std::getenv("DCFS_TRACE_ID");
    return new testonly::Observers(
        {new testonly::TraceRecorder(
             fd, id != nullptr && *id != '\0' ? id : "e2e"),
         &testonly::CheckingObservers()});
  }();
  return *observers;
}

}  // namespace dcfs
