#ifndef DCFS_FORK_SPLIT_H_
#define DCFS_FORK_SPLIT_H_

// fork(2) without a frame that returns twice (step 26.14f).
//
// A function that forks and then returns in both processes is entered once
// and left twice. Under source-based coverage in continuous mode (the guests'
// mode: the processes share the profile's counters) llvm-cov derives some of
// a function's counts from the others, assuming what flows in flows out, and
// here it does not: a branch after the fork gets a negative count, printed as
// 4294967295 (docs/coverage.md, "Known coverage artifacts"). The daemon's
// fork was the case: dcfs/main.cc and dcfs/startup_channel.cc showed six
// such counts. So the two processes never return through the same code:
// ForkSplit runs `child` in the child and returns its value, and runs
// `parent` in the parent, which must not return (it exits).

#include <sys/types.h>

#include <cstdlib>
#include <type_traits>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "dcfs/syscalls_process.h"

namespace dcfs {

// Not instrumented: it is the one function that does return twice, and it has
// no branch worth counting. `child` and `parent` are ordinary, counted
// functions that each run once per call. Returns the fork's error in the
// parent when the fork fails.
template <typename Child, typename Parent>
__attribute__((no_profile_instrument_function))
absl::StatusOr<std::invoke_result_t<Child>> ForkSplit(Child child,
                                                      Parent parent) {
  absl::StatusOr<pid_t> pid = syscalls::fork();
  if (!pid.ok()) return pid.status();
  if (*pid == 0) return child();
  parent(*pid);
  std::abort();  // `parent` returned: a bug, and it must not run on.
}

}  // namespace dcfs

#endif  // DCFS_FORK_SPLIT_H_
