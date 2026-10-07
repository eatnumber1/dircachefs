// MainInvariantChecks() for the testonly checking build of the daemon
// (//dcfs:main_static_checked): an InvariantChecker that also writes a
// violation to /dev/console, the QEMU guest's serial log, where
// test/qemu/scripts/run-qemu.sh fails the run on it (a guest script may never
// read the daemon's own log). See dcfs/invariant_checks.h.

#include <fcntl.h>

#include <utility>

#include "absl/log/log.h"
#include "absl/status/statusor.h"
#include "dcfs/fd.h"
#include "dcfs/invariant_checks.h"
#include "dcfs/syscalls.h"
#include "dcfs/testonly/invariant_checker.h"

namespace dcfs {

InvariantChecks &MainInvariantChecks() {
  static testonly::InvariantChecker *checker = [] {
    // No console (outside a guest): the abort on stderr is all there is.
    absl::StatusOr<FileDescriptor> console =
        syscalls::openat(AT_FDCWD, "/dev/console", O_WRONLY | O_NOCTTY);
    // The marker //test/qemu:invariant_checks_on_test looks for in the log
    // of a daemon in the fast tier's guest (the gate's self-check).
    LOG(WARNING) << "invariant checks: on (the testonly checking build; "
                    "docs/design.md, \"Runtime invariant checks\")";
    return new testonly::InvariantChecker(
        console.ok() ? std::move(*console).Release() : -1);
  }();
  return *checker;
}

}  // namespace dcfs
