// Step 15.2: ForkDaemon's parent side and its failure paths: a daemon that
// reports ready, one that reports a failure, one that dies without a word,
// and one that cannot detach (a link-time wrap of setsid fails it). The
// forked child is the "daemon": it runs `body` and exits. The wrapper is a
// process of its own, since ForkDaemon exits in it (step 26.14f). A target of
// its own because of the wrap (BUILD.bazel).

#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <string>
#include <vector>

#include "absl/functional/function_ref.h"
#include "absl/log/log_entry.h"
#include "absl/log/log_sink.h"
#include "absl/log/log_sink_registry.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/str_split.h"
#include "dcfs/fd.h"
#include "dcfs/mount_dcfs.h"
#include "dcfs/startup_channel.h"
#include "dcfs/syscalls.h"
#include "dcfs/syscalls_backing.h"
#include "dcfs/syscalls_process.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"

extern "C" pid_t __real_setsid();

namespace dcfs {
namespace {

using ::testing::ElementsAre;
using ::testing::HasSubstr;

bool g_setsid_fails = false;

// Sends each ERROR line to a descriptor (the test's end of a socket pair):
// the wrapper's process exits inside ForkDaemon, so nothing in it can hand
// the lines back afterwards.
class ErrorLinesTo : public absl::LogSink {
 public:
  explicit ErrorLinesTo(int fd) : fd_(fd) { absl::AddLogSink(this); }
  ~ErrorLinesTo() override { absl::RemoveLogSink(this); }
  void Send(const absl::LogEntry &entry) override {
    if (entry.log_severity() < absl::LogSeverity::kError) return;
    const std::string line = absl::StrCat(entry.text_message(), "\n");
    syscalls::write(fd_, line.data(), line.size()).status().IgnoreError();
  }

 private:
  int fd_;
};

struct WrapperRun {
  int exit_status = -1;
  std::vector<std::string> errors;
};

// Runs the wrapper, a process of its own because ForkDaemon exits in it: it
// forks a daemon running `body` with its reporter, which then exits with
// `child_exit`. Returns the wrapper's exit status and the ERROR lines it
// logged.
WrapperRun RunWrapper(absl::FunctionRef<void(StartupReporter &)> body,
                      int child_exit = 0) {
  auto channel = syscalls::socketpair(AF_UNIX, SOCK_STREAM, 0);
  EXPECT_TRUE(channel.ok()) << channel.status();
  auto &[read_end, write_end] = *channel;
  absl::StatusOr<pid_t> wrapper = syscalls::fork();
  EXPECT_TRUE(wrapper.ok()) << wrapper.status();
  if (*wrapper == 0) {
    read_end.Close().IgnoreError();
    ErrorLinesTo errors(*write_end);
    // Returns in the daemon only.
    absl::StatusOr<StartupReporter> forked = ForkDaemon();
    if (!forked.ok()) syscalls::_exit(111);
    body(*forked);
    syscalls::_exit(child_exit);
  }
  write_end.Close().IgnoreError();
  WrapperRun run;
  std::string text;
  char buf[512];
  while (true) {
    absl::StatusOr<size_t> n = syscalls::read(*read_end, buf, sizeof(buf));
    if (!n.ok() || *n == 0) break;
    text.append(buf, *n);
  }
  int wait_status = 0;
  EXPECT_TRUE(syscalls::waitpid(*wrapper, &wait_status, 0).ok());
  EXPECT_TRUE(WIFEXITED(wait_status)) << "wait status " << wait_status;
  run.exit_status = WEXITSTATUS(wait_status);
  run.errors = absl::StrSplit(text, '\n', absl::SkipEmpty());
  return run;
}

TEST(ForkDaemonTest, ReadyMakesTheWrapperExitZero) {
  const WrapperRun run = RunWrapper([](StartupReporter &r) { r.Ready(); });
  EXPECT_EQ(run.exit_status, 0);
  EXPECT_THAT(run.errors, testing::IsEmpty());
}

TEST(ForkDaemonTest, AFailureIsPrintedAndItsStatusReturned) {
  const WrapperRun run = RunWrapper([](StartupReporter &r) {
    r.Fail(NativeMountError(32, "mount: nothing here"));
  });
  EXPECT_EQ(run.exit_status, 32);
  EXPECT_THAT(run.errors, ElementsAre(HasSubstr("mount: nothing here")));
}

TEST(ForkDaemonTest, ADaemonThatDiesSilentlyIsReportedWithHow) {
  const WrapperRun run = RunWrapper([](StartupReporter &) {}, /*child_exit=*/7);
  EXPECT_EQ(run.exit_status, 32);
  EXPECT_THAT(run.errors,
              ElementsAre(AllOf(HasSubstr("before it was ready"),
                                HasSubstr("exit status 7"))));
}

TEST(ForkDaemonTest, ADaemonThatCannotDetachReportsWhy) {
  g_setsid_fails = true;
  const WrapperRun run =
      RunWrapper([](StartupReporter &) { syscalls::_exit(99); });
  g_setsid_fails = false;
  EXPECT_NE(run.exit_status, 0);
  EXPECT_THAT(run.errors, ElementsAre(HasSubstr("setsid")));
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
