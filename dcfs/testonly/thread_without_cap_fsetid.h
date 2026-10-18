// SPDX-License-Identifier: MIT
#ifndef DCFS_TESTONLY_THREAD_WITHOUT_CAP_FSETID_H_
#define DCFS_TESTONLY_THREAD_WITHOUT_CAP_FSETID_H_

#include <sys/types.h>

#include <thread>

#include "absl/log/check.h"
#include "absl/synchronization/notification.h"
#include "dcfs/syscalls.h"

namespace dcfs::testonly {

// A thread of this process whose /proc entry (/proc/<tid>/task/<tid>)
// shows filesystem uid 1000 and no CAP_FSETID: setting a thread's
// filesystem uid away from 0 drops the filesystem capabilities, CAP_FSETID
// among them, from its effective set (cap_task_fix_setuid). It waits until
// this is destroyed.
class ThreadWithoutCapFsetid {
 public:
  ThreadWithoutCapFsetid()
      : thread_([this] {
          tid_ = syscalls::gettid();
          CHECK_OK(syscalls::setfsuid(1000));
          ready_.Notify();
          done_.WaitForNotification();
        }) {
    ready_.WaitForNotification();
  }
  ThreadWithoutCapFsetid(const ThreadWithoutCapFsetid &) = delete;
  ThreadWithoutCapFsetid &operator=(const ThreadWithoutCapFsetid &) = delete;
  ~ThreadWithoutCapFsetid() {
    done_.Notify();
    thread_.join();
  }

  pid_t tid() const { return tid_; }

 private:
  absl::Notification ready_;
  absl::Notification done_;
  pid_t tid_ = 0;
  // Last: it starts in the constructor and uses the members above.
  std::thread thread_;
};

}  // namespace dcfs::testonly

#endif  // DCFS_TESTONLY_THREAD_WITHOUT_CAP_FSETID_H_
