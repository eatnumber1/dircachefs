#ifndef DCFS_IO_WORKER_H_
#define DCFS_IO_WORKER_H_

#include <future>
#include <thread>

#include "dcfs/status.h"
#include "absl/status/statusor.h"
#include "absl/status/status.h"
#include "absl/log/log.h"
#include "dcfs/ublk.h"

namespace dcfs {

// A UserIoWorkerThread handles ublk io events from the end-user.
class UserIoWorkerThread {
 public:
  ~UserIoWorkerThread();
  UserIoWorkerThread(const UblkDevice &device, int queue_id);

  // *this is captured by the running thread, so no moves or copies.
  UserIoWorkerThread(UserIoWorkerThread &&) = delete;
  UserIoWorkerThread(const UserIoWorkerThread &) = delete;
  UserIoWorkerThread &operator=(UserIoWorkerThread &&) = delete;
  UserIoWorkerThread &operator=(const UserIoWorkerThread &) = delete;

  absl::Status Join();

 private:
  UserIoWorkerThread(
      const UblkDevice &device, int queue_id,
      std::promise<absl::Status> result_promise);

  void ThreadMain(int queue_id, std::promise<absl::Status> result);
  absl::Status Run(int queue_id);

  pthread_t GetThreadHandle();

  const UblkDevice &device_;
  std::future<absl::Status> result_;
  std::thread thread_;
};

}  // namespace dcfs

#endif  // DCFS_IO_WORKER_H_
