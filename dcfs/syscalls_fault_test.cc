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
#include <sys/xattr.h>
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
  // fgetxattr: once this fd/name pair is seen on a size-query call
  // (value == nullptr, size == 0), fsetxattr()s `grow_value` onto it for
  // real -- between the query and the read that immediately follows --
  // so the subsequent read call gets a genuine ERANGE from the kernel.
  int fgetxattr_fd = -1;
  std::string fgetxattr_name;
  std::string grow_value;
  bool grown = false;

  // flistxattr: once this fd is seen on a size-query call, fsetxattr()s a
  // brand new xattr (name `grow_new_name`) onto it for real, growing the
  // list between the query and the read that immediately follows.
  int flistxattr_fd = -1;
  std::string grow_new_name;
};

FaultState &GetFaultState() {
  static FaultState *state = new FaultState();
  return *state;
}

}  // namespace
}  // namespace dcfs

extern "C" {

ssize_t __real_fgetxattr(int fd, const char *name, void *value, size_t size);
ssize_t __real_flistxattr(int fd, void *list, size_t size);

ssize_t __wrap_fgetxattr(int fd, const char *name, void *value, size_t size) {
  dcfs::FaultState &st = dcfs::GetFaultState();
  ssize_t rc = __real_fgetxattr(fd, name, value, size);
  // Only the size-query shape (as dcfs::syscalls::fgetxattr makes it)
  // triggers the fault, and only once: the retry re-query that follows a
  // real ERANGE must see the real, already-grown size.
  if (!st.grown && value == nullptr && size == 0 && fd == st.fgetxattr_fd &&
      name != nullptr && st.fgetxattr_name == name) {
    st.grown = true;
    if (::fsetxattr(fd, name, st.grow_value.data(), st.grow_value.size(),
                    0) != 0) {
      ADD_FAILURE() << "test setup: fsetxattr to grow " << name
                    << " failed: " << std::strerror(errno);
    }
  }
  return rc;
}

ssize_t __wrap_flistxattr(int fd, void *list, size_t size) {
  dcfs::FaultState &st = dcfs::GetFaultState();
  ssize_t rc = __real_flistxattr(fd, list, size);
  if (!st.grown && list == nullptr && size == 0 && fd == st.flistxattr_fd) {
    st.grown = true;
    const std::string value = "v";
    if (::fsetxattr(fd, st.grow_new_name.c_str(), value.data(), value.size(),
                    0) != 0) {
      ADD_FAILURE() << "test setup: fsetxattr to grow the list failed: "
                    << std::strerror(errno);
    }
  }
  return rc;
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

// Regression test for bdfd61c: fgetxattr()'s size-query-then-read is
// inherently racy against a concurrent writer of the same xattr, and the
// bug was that ERANGE from the *read* call (the value grew after the
// query) was not retried at all -- only a (never actually reachable)
// ERANGE from the query call was. __wrap_fgetxattr (above) grows the
// value for real in that exact window, so the read that follows gets a
// genuine ERANGE from the kernel -- no timing dependency at all.
TEST_F(SyscallsFaultTest, FgetxattrRetriesOnErangeFromReadNotQuery) {
  const std::string small_value = "x";
  const std::string grown_value(256, 'y');
  if (::fsetxattr(file_fd_, "user.dcfs_erange", small_value.data(),
                  small_value.size(), 0) != 0) {
    GTEST_SKIP() << "user xattrs unsupported here: " << std::strerror(errno);
  }
  FaultState &st = GetFaultState();
  st.fgetxattr_fd = file_fd_;
  st.fgetxattr_name = "user.dcfs_erange";
  st.grow_value = grown_value;

  auto value = syscalls::fgetxattr(file_fd_, "user.dcfs_erange");
  ASSERT_THAT(value, IsOk());
  EXPECT_EQ(*value, grown_value);
}

// As FgetxattrRetriesOnErangeFromReadNotQuery, for flistxattr(): the list
// grows (a new xattr appears) between the size query and the read.
TEST_F(SyscallsFaultTest, FlistxattrRetriesOnErangeFromReadNotQuery) {
  const std::string value = "v";
  if (::fsetxattr(file_fd_, "user.dcfs_list_a", value.data(), value.size(),
                  0) != 0) {
    GTEST_SKIP() << "user xattrs unsupported here: " << std::strerror(errno);
  }
  FaultState &st = GetFaultState();
  st.flistxattr_fd = file_fd_;
  st.grow_new_name = "user.dcfs_list_b";

  auto names = syscalls::flistxattr(file_fd_);
  ASSERT_THAT(names, IsOk());
  EXPECT_NE(std::find(names->begin(), names->end(), "user.dcfs_list_a"),
            names->end());
  EXPECT_NE(std::find(names->begin(), names->end(), "user.dcfs_list_b"),
            names->end());
}

}  // namespace
}  // namespace dcfs
