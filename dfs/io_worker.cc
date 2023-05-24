#include "dfs/io_worker.h"

#include <utility>

#include "dfs/syscalls.h"
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
    : device_(device),
      result_(result_promise.get_future()),
      // TODO thread creation can throw an exception. Convert to Status
      thread_(&UserIoWorkerThread::ThreadMain, this, queue_id, std::move(result_promise))
{
  // TODO make this configurable
  absl::Status st = syscalls::pthread_setschedparam(
      GetThreadHandle(), SCHED_RR, /*param=*/{
        .sched_priority = sched_get_priority_min(SCHED_RR),
      });
  LOG_IF(WARNING, !st.ok())
      << "Failed to set UserIoWorkerThread to SCHED_RR: " << st;
}

UserIoWorkerThread::UserIoWorkerThread(const UblkDevice &device, int queue_id)
    : UserIoWorkerThread(device, queue_id, /*result_promise=*/{}) {}

absl::Status UserIoWorkerThread::Join() {
  thread_.join();
  CHECK(result_.valid());
  return result_.get();
}

void UserIoWorkerThread::ThreadMain(int queue_id, std::promise<absl::Status> result) {
  result.set_value(Run(queue_id));
}

absl::Status UserIoWorkerThread::Run(int queue_id) {
  ASSIGN_OR_RETURN(auto queue, UblkQueue::Create(*device_, queue_id));

  LOG(INFO)
    << "Queue " << queue->q_id << " for " << device_ << " started on thread "
    << gettid();

  int num_events = 0;
  while (true) {
    absl::StatusOr<int> processed = queue.ProcessIo();
    if (!processed.ok()) {
      if (absl::StatusOr<int> eno = GetErrnoFromStatus(processed.status());
          eno.ok() && *eno == ENODEV) {
        // We're shutting down gracefully.
        break;
      }
      return std::move(processed).status();
    }
    num_events += *processed;
  }

  LOG(INFO)
    << "Queue " << queue->q_id << " for device "
    << device_.GetInfo().dev_id << " on thread " << gettid()
    << " processed " << num_events << " events and is shutting down.";
  return absl::OkStatus();
}

pthread_t UserIoWorkerThread::GetThreadHandle() {
  // libstdc++ uses pthreads for std::thread.
  // https://gcc.gnu.org/onlinedocs/libstdc++/manual/status.html#iso.2011.specific
  static_assert(std::is_same_v<std::thread::native_handle_type, pthread_t>);
  return thread_.native_handle();
}

}  // namespace dfs
