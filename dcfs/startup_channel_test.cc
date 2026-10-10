#include "dcfs/startup_channel.h"

#include <sys/socket.h>

#include <cstddef>
#include <string>
#include <utility>

#include "absl/status/status.h"
#include "absl/status/status_matchers.h"
#include "dcfs/mount_dcfs.h"
#include "dcfs/syscalls.h"
#include "dcfs/syscalls_backing.h"
#include "dcfs/testonly/assert_ok_and_assign.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"

namespace dcfs {
namespace {

using ::testing::HasSubstr;

// The wrapper's end of a channel: everything the reporter sent.
std::string ReadAll(int fd) {
  std::string bytes;
  char buf[256];
  while (true) {
    absl::StatusOr<size_t> n = syscalls::read(fd, buf, sizeof(buf));
    if (!n.ok() || *n == 0) return bytes;
    bytes.append(buf, *n);
  }
}

TEST(StartupReporterTest, ReadyIsReportedOnceAndClosesTheChannel) {
  ASSERT_OK_AND_ASSIGN(auto pair, syscalls::socketpair(AF_UNIX, SOCK_STREAM, 0));
  StartupReporter reporter(std::move(pair.first));
  EXPECT_TRUE(reporter.pending());
  reporter.Ready();
  EXPECT_FALSE(reporter.pending());
  reporter.Fail(absl::InternalError("too late"));  // no-op
  StartupReport report = DecodeStartupReport(ReadAll(*pair.second));
  EXPECT_TRUE(report.ready);
}

TEST(StartupReporterTest, FailureCarriesTheMessageAndExitStatus) {
  ASSERT_OK_AND_ASSIGN(auto pair, syscalls::socketpair(AF_UNIX, SOCK_STREAM, 0));
  StartupReporter reporter(std::move(pair.first));
  reporter.Fail(NativeMountError(32, "mount: wrong fs type"));
  EXPECT_FALSE(reporter.pending());
  reporter.Ready();  // no-op
  StartupReport report = DecodeStartupReport(ReadAll(*pair.second));
  EXPECT_FALSE(report.ready);
  EXPECT_EQ(report.exit_status, 32);
  EXPECT_THAT(report.message, HasSubstr("wrong fs type"));
}

TEST(StartupReporterTest, AnInertReporterReportsNothing) {
  StartupReporter reporter;
  EXPECT_FALSE(reporter.pending());
  reporter.Ready();
  reporter.Fail(absl::InternalError("nobody to tell"));
}

TEST(StartupReporterTest, AWrapperThatIsGoneIsNotFatal) {
  ASSERT_OK_AND_ASSIGN(auto pair, syscalls::socketpair(AF_UNIX, SOCK_STREAM, 0));
  StartupReporter reporter(std::move(pair.first));
  ASSERT_THAT(pair.second.Close(), absl_testing::IsOk());
  reporter.Ready();  // EPIPE, not SIGPIPE
  EXPECT_FALSE(reporter.pending());
}

}  // namespace
}  // namespace dcfs
