// Step 26.14f: what a fork does to source-based coverage in continuous mode,
// for //test/qemu:coverage_pipeline_test.
//
//   cov_fork_fixture twice   a function that returns in both processes (what
//                            ForkDaemon did): the processes share the
//                            profile's counters, the function is entered once
//                            and left twice, and llvm-cov derives a negative
//                            count (printed as 4294967295, DA lines as 2^64-1)
//   cov_fork_fixture split   the same job with dcfs::ForkSplit, which returns
//                            in the child only: every count is right
//
// Both modes fork one child that exits 0 after `Child()` and a parent that
// waits for it. The parent's work is `Parent()`, the child's `Child()`: each
// runs once per fixture run, so the lcov must say 1 for each.
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cstdlib>
#include <cstring>
#include <optional>

#include "dcfs/fork_split.h"

namespace {

struct Forked {
  std::optional<int> parent_status;
};

// Returns in the parent and in the child.
Forked ForkAndReturnTwice() {
  pid_t child = fork();
  if (child == 0) return Forked{};
  int wait_status = 0;
  waitpid(child, &wait_status, 0);
  return Forked{.parent_status = WIFEXITED(wait_status) ? 0 : 1};
}

int Parent() { return 0; }
int Child() { return 0; }

int Twice() {
  Forked forked = ForkAndReturnTwice();
  if (forked.parent_status.has_value()) return Parent();
  return Child();
}

// The same job with ForkSplit: only the child returns.
int Split() {
  absl::StatusOr<int> child = dcfs::ForkSplit(
      [] { return Child(); },
      [](pid_t pid) {
        int wait_status = 0;
        waitpid(pid, &wait_status, 0);
        std::exit(WIFEXITED(wait_status) ? Parent() : 1);
      });
  return child.ok() ? *child : 1;
}

}  // namespace

int main(int argc, char *argv[]) {
  if (argc == 2 && std::strcmp(argv[1], "twice") == 0) return Twice();
  if (argc == 2 && std::strcmp(argv[1], "split") == 0) return Split();
  return 2;
}
