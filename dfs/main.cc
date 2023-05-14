#include <cstdlib>
#include <iostream>
#include <utility>
#include <cstdint>
#include <unistd.h>

#include "absl/log/initialize.h"
#include "absl/status/status.h"
#include "absl/flags/flag.h"
#include "absl/flags/parse.h"
#include "absl/flags/usage.h"
#include "absl/log/log.h"
#include "dfs/io_uring.h"
#include "dfs/syscalls.h"
#include "dfs/fuse.h"

ABSL_FLAG(
    uint32_t, io_uring_submission_queue_entries, 128,
    "The minimum number of entries to fit in the io_uring submission queue.");
ABSL_FLAG(
    uint32_t, io_uring_completion_queue_entries, 0,
    "The minimum number of entries to fit in the io_uring completion queue. "
    "Automatically determined if 0.");

namespace dfs {

class Diskyphus {
 public:
  struct Options {
    IoUring::Options io_uring_opts;
  };

  Diskyphus(Options opts) : opts_(std::move(opts)) {}

  absl::Status DoStuff(std::string mountpoint) {

    // prctl(PR_SET_IO_FLUSHER, 1, 0, 0, 0);
    //
    // struct rlimit rlimit;
    // rlimit.rlim_cur = RLIM_INFINITY;
    // rlimit.rlim_max = RLIM_INFINITY;
    // rc = setrlimit(RLIMIT_MEMLOCK, &rlimit);
    // mlock all

    absl::StatusOr<IoUring> uring = IoUring::Create(opts_.io_uring_opts);
    if (!uring.ok()) return std::move(uring).status();

    LOG(INFO) << "Mounting to " << mountpoint;
    absl::StatusOr<FuseMount> mount = FuseMount::Create(
        std::move(mountpoint),
        {
            .fsname = "dfs",
            .mount_source = "mydisks",
            .mount_options = {"default_permissions"},
        });
    if (!mount.ok()) return std::move(mount).status();

    LOG(INFO) << "Success";

    return absl::OkStatus();
  }

 private:

  const Options opts_;
};

}  // namespace dfs

int main(int argc, char *argv[]) {
  absl::SetProgramUsageMessage("[flags] mountpoint");
  std::vector<char*> args = absl::ParseCommandLine(argc, argv);
  absl::InitializeLog();

  if (args.size() != 2) {
    std::cerr << "TODO usage here" << std::endl;
    return EXIT_FAILURE;
  }

  std::string mountpoint(args[1]);

  dfs::Diskyphus dfs({
    .io_uring_opts {
      .submission_queue_entries =
          absl::GetFlag(FLAGS_io_uring_submission_queue_entries),
      .completion_queue_entries =
          absl::GetFlag(FLAGS_io_uring_completion_queue_entries),
    },
  });
  absl::Status st = dfs.DoStuff(std::move(mountpoint));
  if (!st.ok()) {
    std::cerr << st << std::endl;
    return EXIT_FAILURE;
  }

  return EXIT_SUCCESS;
}
