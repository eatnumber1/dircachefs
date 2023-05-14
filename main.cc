#include <cstdlib>
#include <iostream>

#include "absl/status/status.h"
#include "absl/flags/flag.h"
#include "absl/flags/parse.h"
#include "absl/flags/usage.h"

#include "fuse/fuse_kernel.h"
#include "uring/liburing.h"

namespace dfs {

absl::Status Main() {

  // prctl(PR_SET_IO_FLUSHER, 1, 0, 0, 0);
  //
  // struct rlimit rlimit;
  // rlimit.rlim_cur = RLIM_INFINITY;
  // rlimit.rlim_max = RLIM_INFINITY;
  // rc = setrlimit(RLIMIT_MEMLOCK, &rlimit);
  // mlock all

  return absl::OkStatus();
}

}  // namespace dfs

int main(int argc, char *argv[]) {
  absl::SetProgramUsageMessage("TODO");
  absl::ParseCommandLine(argc, argv);

  absl::Status st = dfs::Main();
  if (!st.ok()) {
    std::cerr << st << std::endl;
    return EXIT_FAILURE;
  }

  return EXIT_SUCCESS;
}
