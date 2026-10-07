#ifndef DCFS_TESTONLY_CHECKING_OBSERVERS_H_
#define DCFS_TESTONLY_CHECKING_OBSERVERS_H_

#include "dcfs/testonly/observers.h"

namespace dcfs::testonly {

// The testonly daemon's checker and cost counter, as one observer (one
// process-wide instance, made on first use): an InvariantChecker that also
// writes a violation to /dev/console, the QEMU guest's serial log, where
// test/qemu/scripts/run-qemu.sh fails the run on it (a guest script may
// never read the daemon's own log); and a CostCounter that writes its counts
// to $DCFS_COUNTERS_FILE after every request, when that is set. Logs the
// marker //test/qemu:invariant_checks_on_test looks for.
Observers &CheckingObservers();

}  // namespace dcfs::testonly

#endif  // DCFS_TESTONLY_CHECKING_OBSERVERS_H_
