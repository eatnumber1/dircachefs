// MainProtocolEvents() for the testonly recording build of the daemon
// (//dcfs:main_static_traced): a TraceRecorder writing to $DCFS_TRACE_FILE,
// or to /dev/console (the QEMU guest's serial log, which outlives a killed
// daemon and is where the host collects the trace) if that is unset. The
// trace is named $DCFS_TRACE_ID, or "e2e". See dcfs/protocol_events.h.

#include <fcntl.h>

#include <cstdlib>
#include <string>

#include "absl/log/check.h"
#include "dcfs/protocol_events.h"
#include "dcfs/testonly/trace_recorder.h"

namespace dcfs {

ProtocolEvents &MainProtocolEvents() {
  static testonly::TraceRecorder *recorder = [] {
    const char *path = std::getenv("DCFS_TRACE_FILE");
    if (path == nullptr || *path == '\0') path = "/dev/console";
    const int fd =
        ::open(path, O_WRONLY | O_APPEND | O_CREAT | O_NOCTTY | O_CLOEXEC, 0600);
    PCHECK(fd >= 0) << "opening the trace file " << path;
    const char *id = std::getenv("DCFS_TRACE_ID");
    return new testonly::TraceRecorder(
        fd, id != nullptr && *id != '\0' ? id : "e2e");
  }();
  return *recorder;
}

}  // namespace dcfs
