// Step 15.2: ForkDaemon's parent side and its failure paths: a daemon that
// reports ready, one that reports a failure, one that dies without a word,
// and one that cannot detach (a link-time wrap of setsid fails it). The
// forked child is the "daemon": it runs `body` and exits. A target of its own
// because of the wrap (BUILD.bazel).

#include <unistd.h>

#include <cerrno>
#include <functional>
#include <string>
#include <vector>

#include "absl/log/log_entry.h"
#include "absl/log/log_sink.h"
#include "absl/log/log_sink_registry.h"
#include "absl/status/status.h"
#include "dcfs/mount_dcfs.h"
#include "dcfs/startup_channel.h"
#include "dcfs/syscalls_process.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"

extern "C" pid_t __real_setsid();

namespace dcfs {
namespace {

using ::testing::ElementsAre;
using ::testing::HasSubstr;

bool g_setsid_fails = false;

class ErrorLines : public absl::LogSink {
 public:
  ErrorLines() { absl::AddLogSink(this); }
  ~ErrorLines() override { absl::RemoveLogSink(this); }
  void Send(const absl::LogEntry &entry) override {
    if (entry.log_severity() >= absl::LogSeverity::kError) {
      lines.emplace_back(entry.text_message());
    }
  }
  std::vector<std::string> lines;
};

// Forks a daemon running `body` with its reporter, which then exits with
// `child_exit`; returns the parent's exit status.
int RunDaemon(const std::function<void(StartupReporter &)> &body,
              int child_exit = 0) {
  absl::StatusOr<DaemonFork> forked = ForkDaemon();
  EXPECT_TRUE(forked.ok()) << forked.status();
  if (!forked->parent_exit_status.has_value()) {
    body(forked->reporter);
    syscalls::_exit(child_exit);
  }
  return *forked->parent_exit_status;
}

TEST(ForkDaemonTest, ReadyMakesTheWrapperExitZero) {
  ErrorLines errors;
  EXPECT_EQ(RunDaemon([](StartupReporter &r) { r.Ready(); }), 0);
  EXPECT_THAT(errors.lines, testing::IsEmpty());
}

TEST(ForkDaemonTest, AFailureIsPrintedAndItsStatusReturned) {
  ErrorLines errors;
  EXPECT_EQ(RunDaemon([](StartupReporter &r) {
              r.Fail(NativeMountError(32, "mount: nothing here"));
            }),
            32);
  EXPECT_THAT(errors.lines, ElementsAre(HasSubstr("mount: nothing here")));
}

TEST(ForkDaemonTest, ADaemonThatDiesSilentlyIsReportedWithHow) {
  ErrorLines errors;
  EXPECT_EQ(RunDaemon([](StartupReporter &) {}, /*child_exit=*/7), 32);
  EXPECT_THAT(errors.lines,
              ElementsAre(AllOf(HasSubstr("before it was ready"),
                                HasSubstr("exit status 7"))));
}

TEST(ForkDaemonTest, ADaemonThatCannotDetachReportsWhy) {
  ErrorLines errors;
  g_setsid_fails = true;
  const int status = RunDaemon([](StartupReporter &) { syscalls::_exit(99); });
  g_setsid_fails = false;
  EXPECT_NE(status, 0);
  EXPECT_THAT(errors.lines, ElementsAre(HasSubstr("setsid")));
}

}  // namespace
}  // namespace dcfs

extern "C" pid_t __wrap_setsid() {
  if (dcfs::g_setsid_fails) {
    errno = EPERM;
    return -1;
  }
  return __real_setsid();
}
