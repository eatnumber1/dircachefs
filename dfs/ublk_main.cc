#include <stdlib.h>
#include <sched.h>
#include <pthread.h>
#include <getopt.h>
#include <stdarg.h>
#include <errno.h>
#include <error.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>
#include <sys/stat.h>
#include <cstdlib>
#include <iostream>
#include <utility>
#include <cstdint>
#include <signal.h>
#include <unistd.h>
#include <thread>
#include <memory>

#include "absl/flags/flag.h"
#include "absl/flags/parse.h"
#include "absl/flags/usage.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/log/initialize.h"
#include "absl/log/check.h"
#include "absl/log/log.h"
#include "ublksrv/ublksrv.h"
#include "ublksrv/ublksrv_utils.h"
#include "dfs/syscalls.h"
#include "dfs/status.h"
#include "dfs/ublk.h"
#include "dfs/io_worker.h"
#include "absl/container/fixed_array.h"

namespace dfs {
namespace {
int DfsHandleIoAsync(const ublksrv_queue *q, const ublk_io_data *data) {
  const struct ublksrv_io_desc *iod = data->iod;

  ublksrv_complete_io(q, data->tag, iod->nr_sectors << 9);

  // Return negative errno on errors, 0 on success.
  return 0;
}
}  // namespace

absl::StatusOr<int> Main(int argc, char *argv[]) {
  absl::InitializeLog();
  absl::SetProgramUsageMessage("TODO");
  std::vector<char*> args = absl::ParseCommandLine(argc, argv);

  sigset_t sigmask;
  sigemptyset(&sigmask);
  sigaddset(&sigmask, SIGTERM);
  sigaddset(&sigmask, SIGINT);

  // Block signals so that they aren't handled ccording to their default
  // dispositions.
  ASSIGN_OR_RETURN(auto block, ScopedSignalMask::Create(SIG_BLOCK, sigmask));
  ASSIGN_OR_RETURN(
      dfs::FileDescriptor sigfd,
      syscalls::signalfd(sigmask, /*flags=*/SFD_CLOEXEC));

  unsigned short nr_hw_queues = DEF_NR_HW_QUEUES;

  struct ublksrv_tgt_type target_type = {
    .handle_io_async = &DfsHandleIoAsync,
    .name = "dfs",
  };

  // TODO make the options flags?
  ASSIGN_OR_RETURN(auto dev, UblkDevice::Create({
    .data = {
      .dev_id = -1,
      .max_io_buf_bytes = DEF_BUF_SIZE,
      .nr_hw_queues = nr_hw_queues,
      .queue_depth = DEF_QD,
      .tgt_type = "dfs",
      .tgt_ops = &target_type,
      .flags = 0,
    },
    .params = {
      .types = UBLK_PARAM_TYPE_BASIC,
      // Sets kernel params here
      // https://sourcegraph.com/github.com/torvalds/linux/-/blob/drivers/block/ublk_drv.c?L210
      .basic = {
        // Logical block size 512
        .logical_bs_shift = 9,
        // Physical block size 4096
        .physical_bs_shift = 12,
        // Optimal request size 4096
        .io_opt_shift = 12,
        // Minimum request size 512
        .io_min_shift = 9,
        // Maximum sectors per request set to the max buffer size, in multiples
        // of 512 bytes
        .max_sectors = (/*data.max_io_buf_bytes=*/DEF_BUF_SIZE) >> 9,
        // Number of sectors in the device, in multiples of 512 bytes
        .dev_sectors = (250UL * 1024 * 1024 * 1024) >> 9,
      }
    },
  }));

  std::vector<std::unique_ptr<UserIoWorkerThread>> workers;
  workers.reserve(nr_hw_queues);
  for (int i = 0; i < nr_hw_queues; i++) {
    workers.emplace_back(
        std::make_unique<UserIoWorkerThread>(dev, /*queue_id=*/i));
  }

  {
    ASSIGN_OR_RETURN(UblkDevice::Stopper device_stopper, dev.Start());

    LOG(INFO) << "Device is available at " << dev;

    /* wait until we are terminated */
    {
      signalfd_siginfo fdsi;
      RETURN_IF_ERROR(syscalls::read(*sigfd, &fdsi, sizeof(fdsi)));
      LOG(INFO) << "got signal " << fdsi.ssi_signo;
    }
  }

  for (std::unique_ptr<UserIoWorkerThread> &worker : workers) {
    RETURN_IF_ERROR(worker->Join());
  }

  return EXIT_SUCCESS;
}

}  // namespace dfs

int main(int argc, char *argv[]) {
  absl::StatusOr<int> ret = dfs::Main(argc, argv);
  if (!ret.ok()) {
    std::cerr << ret.status() << std::endl;
    return EXIT_FAILURE;
  }
  return *ret;
}
