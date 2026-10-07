// Fault-injection tests for the xattr retry loops in dcfs/backing.cc: the
// value or the list of xattrs can grow between the size query and the read
// that follows, and no real filesystem can be made to do that
// deterministically. A separate binary (see BUILD.bazel) because its
// link-time symbol wrapping (-Wl,--wrap=...) rewrites every reference to
// the wrapped symbols in the whole binary it links into.
//
// Each wrapped libc function (__wrap_X below) is a thin, test-local fake:
// by default it forwards straight to the real implementation (__real_X),
// and only deviates when a test has armed FaultState for the specific path
// it cares about. No production code is touched by any of this.

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <string>
#include <sys/stat.h>
#include <sys/xattr.h>
#include <unistd.h>
#include <utility>
#include <vector>

#include "absl/status/status_matchers.h"
#include "dcfs/backing.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"

namespace dcfs {
namespace {

using ::absl_testing::IsOk;
using ::testing::Contains;
using ::testing::Pair;

// Shared, test-local state the __wrap_* functions below consult. Reset by
// BackingFaultTest::TearDown() after every test, so tests never leak their
// fault into one another.
struct FaultState {
  // getxattr: once this path/name pair is seen on a size-query call
  // (value == nullptr, size == 0), setxattr()s `grow_value` onto it for
  // real -- between the query and the read that immediately follows -- so
  // the subsequent read call gets a genuine ERANGE from the kernel.
  std::string getxattr_path;
  std::string getxattr_name;
  std::string grow_value;
  bool grown = false;

  // listxattr: once this path is seen on a size-query call, setxattr()s a
  // brand new xattr (name `grow_new_name`) onto it for real, growing the
  // list between the query and the read that immediately follows.
  std::string listxattr_path;
  std::string grow_new_name;
};

FaultState &GetFaultState() {
  static FaultState *state = new FaultState();
  return *state;
}

}  // namespace
}  // namespace dcfs

extern "C" {

ssize_t __real_getxattr(const char *path, const char *name, void *value,
                        size_t size);
ssize_t __real_listxattr(const char *path, char *list, size_t size);

ssize_t __wrap_getxattr(const char *path, const char *name, void *value,
                        size_t size) {
  dcfs::FaultState &st = dcfs::GetFaultState();
  ssize_t rc = __real_getxattr(path, name, value, size);
  // Only the size-query shape triggers the fault, and only once: the retry
  // re-query that follows a real ERANGE must see the real, already-grown
  // size.
  if (!st.grown && value == nullptr && size == 0 && path != nullptr &&
      st.getxattr_path == path && name != nullptr &&
      st.getxattr_name == name) {
    st.grown = true;
    if (::setxattr(path, name, st.grow_value.data(), st.grow_value.size(),
                   0) != 0) {
      ADD_FAILURE() << "test setup: setxattr to grow " << name
                    << " failed: " << std::strerror(errno);
    }
  }
  return rc;
}

ssize_t __wrap_listxattr(const char *path, char *list, size_t size) {
  dcfs::FaultState &st = dcfs::GetFaultState();
  ssize_t rc = __real_listxattr(path, list, size);
  if (!st.grown && list == nullptr && size == 0 && path != nullptr &&
      st.listxattr_path == path) {
    st.grown = true;
    const std::string value = "v";
    if (::setxattr(path, st.grow_new_name.c_str(), value.data(),
                   value.size(), 0) != 0) {
      ADD_FAILURE() << "test setup: setxattr to grow the list failed: "
                    << std::strerror(errno);
    }
  }
  return rc;
}

}  // extern "C"

namespace dcfs {
namespace {

class BackingFaultTest : public ::testing::Test {
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

  std::string ProcPath() const {
    return "/proc/self/fd/" + std::to_string(file_fd_);
  }

  int tmpdir_fd_ = -1;
  int file_fd_ = -1;
};

// Regression test for bdfd61c: the size-query-then-read of an xattr value
// is inherently racy against a concurrent writer of the same xattr, and
// the bug was that ERANGE from the *read* call (the value grew after the
// query) was not retried at all -- only a (never actually reachable)
// ERANGE from the query call was. __wrap_getxattr (above) grows the value
// for real in that exact window, so the read that follows gets a genuine
// ERANGE from the kernel -- no timing dependency at all.
TEST_F(BackingFaultTest, XattrValueRetriesOnErangeFromReadNotQuery) {
  const std::string small_value = "x";
  const std::string grown_value(256, 'y');
  if (::fsetxattr(file_fd_, "user.dcfs_erange", small_value.data(),
                  small_value.size(), 0) != 0) {
    GTEST_SKIP() << "user xattrs unsupported here: " << std::strerror(errno);
  }
  FaultState &st = GetFaultState();
  st.getxattr_path = ProcPath();
  st.getxattr_name = "user.dcfs_erange";
  st.grow_value = grown_value;

  auto xattrs = backing::ReadXattrsFd(file_fd_);
  ASSERT_THAT(xattrs, IsOk());
  EXPECT_THAT(*xattrs, Contains(Pair("user.dcfs_erange", grown_value)));
}

// As XattrValueRetriesOnErangeFromReadNotQuery, for the list: a new xattr
// appears between the size query and the read.
TEST_F(BackingFaultTest, XattrListRetriesOnErangeFromReadNotQuery) {
  const std::string value = "v";
  if (::fsetxattr(file_fd_, "user.dcfs_list_a", value.data(), value.size(),
                  0) != 0) {
    GTEST_SKIP() << "user xattrs unsupported here: " << std::strerror(errno);
  }
  FaultState &st = GetFaultState();
  st.listxattr_path = ProcPath();
  st.grow_new_name = "user.dcfs_list_b";

  auto xattrs = backing::ReadXattrsFd(file_fd_);
  ASSERT_THAT(xattrs, IsOk());
  EXPECT_THAT(*xattrs, Contains(Pair("user.dcfs_list_a", value)));
  EXPECT_THAT(*xattrs, Contains(Pair("user.dcfs_list_b", value)));
}

}  // namespace
}  // namespace dcfs
