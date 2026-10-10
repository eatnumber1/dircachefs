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

#include <fcntl.h>
#include <sys/types.h>

#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <span>
#include <string>
#include <string_view>

#include "absl/status/status.h"
#include "absl/status/status_matchers.h"
#include "dcfs/backing.h"
#include "dcfs/fd.h"
#include "dcfs/status.h"
#include "dcfs/syscalls_backing.h"
#include "dcfs/testonly/assert_ok_and_assign.h"
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

  // readlinkat: for this exact (dirfd, path) pair, always report "buffer
  // completely full" (never delegating to the real syscall) -- simulating a
  // symlink target longer than any real Linux filesystem can actually be
  // made to hold (every one caps a target at or below PATH_MAX at
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
    EXPECT_THAT(dcfs::syscalls::setxattr(
                    path, name,
                    std::span<const uint8_t>(
                        reinterpret_cast<const uint8_t *>(
                            st.grow_value.data()),
                        st.grow_value.size()),
                    0),
                ::absl_testing::IsOk())
        << "test setup: setxattr to grow " << name;
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
    EXPECT_THAT(dcfs::syscalls::setxattr(
                    path, st.grow_new_name,
                    std::span<const uint8_t>(
                        reinterpret_cast<const uint8_t *>(value.data()),
                        value.size()),
                    0),
                ::absl_testing::IsOk())
        << "test setup: setxattr to grow the list failed";
  }
  return rc;
}

ssize_t __real_readlinkat(int dirfd, const char *pathname, char *buf,
                          size_t bufsize);

ssize_t __wrap_readlinkat(int dirfd, const char *pathname, char *buf,
                          size_t bufsize) {
  dcfs::FaultState &st = dcfs::GetFaultState();
  if (dirfd == st.fake_readlinkat_dirfd && pathname != nullptr &&
      st.fake_readlinkat_path == pathname) {
    // Simulated backing readlinkat(2) that never fits, whatever the buffer
    // size: fill it and report it as fully used.
    std::memset(buf, 'a', bufsize);
    return static_cast<ssize_t>(bufsize);
  }
  return __real_readlinkat(dirfd, pathname, buf, bufsize);
}

}  // extern "C"

namespace dcfs {
namespace {

class BackingFaultTest : public ::testing::Test {
 protected:
  void SetUp() override {
    const char *tmpdir = std::getenv("TEST_TMPDIR");
    ASSERT_NE(tmpdir, nullptr);
    ASSERT_OK_AND_ASSIGN(
        tmpdir_fd_,
        syscalls::openat(AT_FDCWD, tmpdir, O_PATH | O_DIRECTORY));
    ASSERT_OK_AND_ASSIGN(
        file_fd_, syscalls::openat(*tmpdir_fd_, "test_file",
                                   O_CREAT | O_RDWR, 0600));
  }

  void TearDown() override {
    GetFaultState() = FaultState();
  }

  std::string ProcPath() const {
    return "/proc/self/fd/" + std::to_string(*file_fd_);
  }

  absl::Status SetXattr(std::string_view name, std::string_view value) {
    return syscalls::fsetxattr(
        *file_fd_, name,
        std::span<const uint8_t>(
            reinterpret_cast<const uint8_t *>(value.data()), value.size()),
        0);
  }

  FileDescriptor tmpdir_fd_;
  FileDescriptor file_fd_;
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
  if (absl::Status set = SetXattr("user.dcfs_erange", small_value);
      !set.ok()) {
    GTEST_SKIP() << "user xattrs unsupported here: " << set;
  }
  FaultState &st = GetFaultState();
  st.getxattr_path = ProcPath();
  st.getxattr_name = "user.dcfs_erange";
  st.grow_value = grown_value;

  auto xattrs = backing::ReadXattrsFd(*file_fd_);
  ASSERT_THAT(xattrs, IsOk());
  EXPECT_THAT(*xattrs, Contains(Pair("user.dcfs_erange", grown_value)));
}

// As XattrValueRetriesOnErangeFromReadNotQuery, for the list: a new xattr
// appears between the size query and the read.
TEST_F(BackingFaultTest, XattrListRetriesOnErangeFromReadNotQuery) {
  const std::string value = "v";
  if (absl::Status set = SetXattr("user.dcfs_list_a", value); !set.ok()) {
    GTEST_SKIP() << "user xattrs unsupported here: " << set;
  }
  FaultState &st = GetFaultState();
  st.listxattr_path = ProcPath();
  st.grow_new_name = "user.dcfs_list_b";

  auto xattrs = backing::ReadXattrsFd(*file_fd_);
  ASSERT_THAT(xattrs, IsOk());
  EXPECT_THAT(*xattrs, Contains(Pair("user.dcfs_list_a", value)));
  EXPECT_THAT(*xattrs, Contains(Pair("user.dcfs_list_b", value)));
}

// Regression test for bdfd61c: reading a symlink must report ENAMETOOLONG,
// not silently truncate, once the doubling buffer reaches the PATH_MAX*4
// cap and the target still doesn't fit. No real Linux filesystem lets a
// test create a symlink whose target is actually that long, so
// __wrap_readlinkat (above) simulates a backing readlinkat(2) that always
// reports a full buffer.
TEST_F(BackingFaultTest, SymlinkTargetIsEnametoolongAtPathMaxTimesFourCap) {
  FaultState &st = GetFaultState();
  st.fake_readlinkat_dirfd = *tmpdir_fd_;
  st.fake_readlinkat_path = "";

  auto target = backing::ReadSymlinkFd(*tmpdir_fd_);
  ASSERT_FALSE(target.ok());
  auto errno_val = GetErrnoFromStatus(target.status());
  ASSERT_THAT(errno_val, IsOk());
  EXPECT_EQ(*errno_val, ENAMETOOLONG);
}

}  // namespace
}  // namespace dcfs
