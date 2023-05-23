#include "dfs/io_worker.h"

#include <utility>

#include "absl/log/check.h"
#include "ublksrv/ublksrv.h"
#include "ublksrv/ublksrv_utils.h"

namespace dfs {

UserIoWorkerThread::~UserIoWorkerThread() {
  if (!thread_.joinable()) return;
  absl::Status st = Join();
  LOG_IF(WARNING, !st.ok()) << "UserIoWorkerThread failed: " << st;
}

UserIoWorkerThread::UserIoWorkerThread(
    const UblkDevice &device, int queue_id,
    std::promise<absl::Status> result_promise)
    : device_(device), queue_id_(queue_id),
      result_(result_promise.get_future()),
      // TODO thread creation can throw an exception. Convert to Status
      thread_(&UserIoWorkerThread::ThreadMain, this, std::move(result_promise))
{}

UserIoWorkerThread::UserIoWorkerThread(const UblkDevice &device, int queue_id)
    : UserIoWorkerThread(device, queue_id, /*result_promise=*/{}) {}

absl::Status UserIoWorkerThread::Join() {
  thread_.join();
  CHECK(result_.valid());
  return result_.get();
}

void UserIoWorkerThread::ThreadMain(std::promise<absl::Status> result) {
  result.set_value(Run());
}

absl::Status UserIoWorkerThread::Run() {
  const struct ublksrv_dev *dev = device_.Get();
  const struct ublksrv_ctrl_dev_info *dinfo = &device_.GetInfo();
  unsigned dev_id = dinfo->dev_id;
  unsigned short q_id = queue_id_;
  const struct ublksrv_queue *q;

  sched_setscheduler(getpid(), SCHED_RR, NULL);

  q = ublksrv_queue_init(dev, q_id, NULL);
  if (!q) {
    fprintf(stderr, "ublk dev %d queue %d init queue failed\n",
        dinfo->dev_id, q_id);
    // TODO
    return absl::UnknownError("ublksrv_queue_init");
  }

  fprintf(stdout, "tid %d: ublk dev %d queue %d started\n",
      ublksrv_gettid(),
      dev_id, q->q_id);
  do {
    if (ublksrv_process_io(q) < 0)
      break;
  } while (1);

  fprintf(stdout, "ublk dev %d queue %d exited\n", dev_id, q->q_id);
  ublksrv_queue_deinit(q);
  return absl::OkStatus();
}

}  // namespace dfs
