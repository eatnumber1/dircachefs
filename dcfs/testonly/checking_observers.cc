#include "dcfs/testonly/checking_observers.h"

#include <fcntl.h>

#include <cstdlib>
#include <utility>

#include "absl/log/check.h"
#include "absl/log/log.h"
#include "absl/status/statusor.h"
#include "dcfs/fd.h"
#include "dcfs/syscalls_backing.h"
#include "dcfs/testonly/cost_counter.h"
#include "dcfs/testonly/invariant_checker.h"
#include "dcfs/testonly/observers.h"

namespace dcfs::testonly {

Observers &CheckingObservers() {
  static Observers *observers = [] {
    // No console (outside a guest): the abort on stderr is all there is.
    absl::StatusOr<FileDescriptor> console =
        syscalls::openat(AT_FDCWD, "/dev/console", O_WRONLY | O_NOCTTY);
    // The marker //test/qemu:invariant_checks_on_test looks for in the log
    // of a daemon in the fast tier's guest (the gate's self-check).
    LOG(WARNING) << "invariant checks: on (the testonly checking build; "
                    "docs/design.md, \"Runtime invariant checks\")";
    auto *checker =
        new InvariantChecker(console.ok() ? std::move(*console).Release() : -1);
    int counters_fd = -1;
    if (const char *path = std::getenv("DCFS_COUNTERS_FILE");
        path != nullptr && *path != '\0') {
      absl::StatusOr<FileDescriptor> opened = syscalls::openat(
          AT_FDCWD, path, O_WRONLY | O_CREAT | O_TRUNC | O_NOCTTY, 0644);
      CHECK_OK(opened) << "opening the counters file " << path;
      counters_fd = std::move(*opened).Release();  // for the process's life
    }
    return new Observers({checker, new CostCounter(counters_fd)});
  }();
  return *observers;
}

}  // namespace dcfs::testonly
