// Fault-injection tests for races/limits in dcfs/syscalls.cc that no real
// syscall on any backing filesystem available to the test suite can be
// made to hit deterministically. This is a separate binary/target from
// syscalls_test (see BUILD.bazel) specifically so its link-time symbol
// wrapping (-Wl,--wrap=...) is scoped to just this test: --wrap rewrites
// every undefined reference to the wrapped symbol in the whole binary it
// links into, so mixing it into the main syscalls_test target would also
// intercept every *other* test's calls to these functions.
//
// Each wrapped libc function (__wrap_X below) is a thin, test-local fake:
// by default it forwards straight to the real implementation
// (__real_X, which -Wl,--wrap resolves to libc's actual definition), and
// only deviates from that when a test has armed FaultState for the
// specific fd/name/path it cares about. No production code (dcfs/
// syscalls.h or .cc) is touched by any of this.

#include "dcfs/syscalls.h"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

#include "absl/status/status_matchers.h"
#include "dcfs/status.h"
#include "gtest/gtest.h"

namespace dcfs {
namespace {

using ::absl_testing::IsOk;

// Shared, test-local state the __wrap_* functions below consult. Reset by
// SyscallsFaultTest::TearDown() after every test, so tests never leak
// their fault into one another.
struct FaultState {
  // readlinkat: for this exact (dirfd, path) pair, always report "buffer
  // completely full" (never delegating to the real syscall) -- simulating
  // a symlink target longer than any real Linux filesystem can actually
  // be made to hold (every one caps a target at or below PATH_MAX at
  // symlink(2) time), so the doubling loop's PATH_MAX*4 cap is reached.
  int fake_readlinkat_dirfd = -1;
  std::string fake_readlinkat_path;
};

FaultState &GetFaultState() {
  static FaultState *state = new FaultState();
  return *state;
}

}  // namespace
}  // namespace dcfs

extern "C" {

ssize_t __real_readlinkat(int dirfd, const char *pathname, char *buf,
                          size_t bufsize);

ssize_t __wrap_readlinkat(int dirfd, const char *pathname, char *buf,
                          size_t bufsize) {
  dcfs::FaultState &st = dcfs::GetFaultState();
  if (dirfd == st.fake_readlinkat_dirfd && pathname != nullptr &&
      st.fake_readlinkat_path == pathname) {
    // Simulated backing readlinkat(2) that never fits, whatever the
    // buffer size: fill it and report it as fully used.
    std::memset(buf, 'a', bufsize);
    return static_cast<ssize_t>(bufsize);
  }
  return __real_readlinkat(dirfd, pathname, buf, bufsize);
}

}  // extern "C"

namespace dcfs {
namespace {

class SyscallsFaultTest : public ::testing::Test {
 protected:
  void SetUp() override {
    const char *tmpdir = std::getenv("TEST_TMPDIR");
    ASSERT_NE(tmpdir, nullptr);
    tmpdir_fd_ = ::openat(AT_FDCWD, tmpdir, O_PATH | O_DIRECTORY);
    ASSERT_GE(tmpdir_fd_, 0);
    file_fd_ = ::openat(tmpdir_fd_, "test_file", O_CREAT | O_RDWR, 0600);
    ASSERT_GE(file_fd_, 0);
  }

  void TearDown() override {
    GetFaultState() = FaultState();
    if (file_fd_ >= 0) ::close(file_fd_);
    if (tmpdir_fd_ >= 0) ::close(tmpdir_fd_);
  }

  int tmpdir_fd_ = -1;
  int file_fd_ = -1;
};

// Regression test for bdfd61c: readlinkat() must report ENAMETOOLONG, not
// silently truncate, once its doubling loop's buffer reaches the
// PATH_MAX*4 cap and the target still doesn't fit. No real Linux
// filesystem lets a test create a symlink whose target is actually that
// long, so __wrap_readlinkat (above) simulates a backing readlinkat(2)
// that always reports "buffer too small", whatever the buffer size.
TEST_F(SyscallsFaultTest, ReadlinkatReturnsEnametoolongAtPathMaxTimesFourCap) {
  FaultState &st = GetFaultState();
  st.fake_readlinkat_dirfd = tmpdir_fd_;
  st.fake_readlinkat_path = "irrelevant";

  auto target = syscalls::readlinkat(tmpdir_fd_, "irrelevant");
  ASSERT_FALSE(target.ok());
  auto errno_val = GetErrnoFromStatus(target.status());
  ASSERT_THAT(errno_val, IsOk());
  EXPECT_EQ(*errno_val, ENAMETOOLONG);
}

}  // namespace
}  // namespace dcfs
