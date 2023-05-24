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

#define TGT_DEV_SIZE (250UL * 1024 * 1024 * 1024)

namespace dfs {

int demo_init_tgt(struct ublksrv_dev *dev, int, int, char **data) {
  const struct ublksrv_ctrl_dev_info &info =
    *ublksrv_ctrl_get_dev_info(ublksrv_get_ctrl_dev(dev));
  struct ublksrv_tgt_info &tgt = dev->tgt;
  tgt.dev_size = TGT_DEV_SIZE;
  tgt.tgt_ring_depth = info.queue_depth;
  tgt.nr_fds = 0;
  return 0;
}

static int demo_handle_io_async(const struct ublksrv_queue *q,
    const struct ublk_io_data *data)
{
  const struct ublksrv_io_desc *iod = data->iod;

  ublksrv_complete_io(q, data->tag, iod->nr_sectors << 9);

  return 0;
}

// TODO delete this
void PrintUblkDevice(UblkDevice &dev) {
  const struct ublksrv_ctrl_dev_info *info = &dev.GetInfo();
  LOG(INFO) << "Device is available at " << dev;
  struct ublk_params p = {};
  p.devt.char_major = 42;

  if (int ret = ublksrv_ctrl_get_params(&dev.GetControlDevice(), &p);
      ret < 0) {
    fprintf(stderr, "failed to get params %m\n");
    return;
  }

  printf("dev id %d: nr_hw_queues %d queue_depth %d block size %d dev_capacity %lld\n",
      info->dev_id,
                        info->nr_hw_queues, info->queue_depth,
                        1 << p.basic.logical_bs_shift, p.basic.dev_sectors);
  printf("\tmax rq size %d daemon pid %d flags 0x%llx\n",
                        info->max_io_buf_bytes,
      info->ublksrv_pid, info->flags);
  // TODO devt is incorrect (uninitialized?)
  printf("\tublkc: %u:%d ublkb: %u:%u owner: %u:%u\n",
      p.devt.char_major, p.devt.char_minor,
      p.devt.disk_major, p.devt.disk_minor,
      info->owner_uid, info->owner_gid);
}

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
    .handle_io_async = demo_handle_io_async,
    .init_tgt = demo_init_tgt,
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
        .dev_sectors = TGT_DEV_SIZE >> 9,
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

    ublksrv_ctrl_get_info(&dev.GetControlDevice());
    PrintUblkDevice(dev);

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
