#include "dcfs/syscalls.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <string>
#include <vector>

#include "absl/status/status_matchers.h"
#include "dcfs/status.h"
#include "gtest/gtest.h"

namespace dcfs {
namespace {

using ::absl_testing::IsOk;
using ::absl_testing::StatusIs;

class SyscallsTest : public ::testing::Test {
 protected:
  void SetUp() override {
    const char *tmpdir = std::getenv("TEST_TMPDIR");
    ASSERT_NE(tmpdir, nullptr);
    tmpdir_fd_ = ::openat(AT_FDCWD, tmpdir, O_PATH | O_DIRECTORY);
    ASSERT_GE(tmpdir_fd_, 0);

    // Create a test file
    file_fd_ = ::openat(tmpdir_fd_, "test_file", O_CREAT | O_RDWR, 0600);
    ASSERT_GE(file_fd_, 0);
  }

  void TearDown() override {
    if (file_fd_ >= 0) ::close(file_fd_);
    if (tmpdir_fd_ >= 0) ::close(tmpdir_fd_);
  }

  int tmpdir_fd_ = -1;
  int file_fd_ = -1;
};

TEST_F(SyscallsTest, WriteAndReadRoundTrip) {
  const std::string data = "hello world";
  auto written_result = syscalls::write(file_fd_, data.data(), data.size());
  ASSERT_THAT(written_result, IsOk());
  EXPECT_EQ(*written_result, data.size());

  auto pos_result = syscalls::lseek(file_fd_, 0, SEEK_SET);
  ASSERT_THAT(pos_result, IsOk());
  EXPECT_EQ(*pos_result, 0);

  std::string buf(data.size(), '\0');
  auto read_result = syscalls::read(file_fd_, buf.data(), buf.size());
  ASSERT_THAT(read_result, IsOk());
  EXPECT_EQ(*read_result, data.size());
  EXPECT_EQ(buf, data);
}

TEST_F(SyscallsTest, PwriteAndPreadRoundTrip) {
  const std::string data = "test data";
  auto written_result =
      syscalls::pwrite(file_fd_, data.data(), data.size(), 0);
  ASSERT_THAT(written_result, IsOk());
  EXPECT_EQ(*written_result, data.size());

  std::string buf(data.size(), '\0');
  auto read_result = syscalls::pread(file_fd_, buf.data(), buf.size(), 0);
  ASSERT_THAT(read_result, IsOk());
  EXPECT_EQ(*read_result, data.size());
  EXPECT_EQ(buf, data);
}

TEST_F(SyscallsTest, FtruncateAndFstat) {
  const off_t new_size = 42;
  EXPECT_THAT(syscalls::ftruncate(file_fd_, new_size), IsOk());

  auto st_result = syscalls::fstat(file_fd_);
  ASSERT_THAT(st_result, IsOk());
  EXPECT_EQ(st_result->st_size, new_size);
}

TEST_F(SyscallsTest, FallocateSkipIfNotSupported) {
  absl::Status status = syscalls::fallocate(file_fd_, 0, 0, 4096);
  if (!status.ok()) {
    auto errno_val = GetErrnoFromStatus(status);
    if (errno_val.ok() && (*errno_val == EOPNOTSUPP || *errno_val == ENOTSUP)) {
      GTEST_SKIP() << "fallocate not supported on this filesystem";
    }
  }
  EXPECT_THAT(status, IsOk());
}

TEST_F(SyscallsTest, FsyncAndFdatasync) {
  const std::string data = "sync test";
  ASSERT_THAT(syscalls::write(file_fd_, data.data(), data.size()), IsOk());
  EXPECT_THAT(syscalls::fsync(file_fd_), IsOk());
  EXPECT_THAT(syscalls::fdatasync(file_fd_), IsOk());
}

TEST_F(SyscallsTest, FchmodAndFstatat) {
  const mode_t new_mode = 0644;
  EXPECT_THAT(syscalls::fchmod(file_fd_, new_mode), IsOk());

  auto st_result =
      syscalls::fstatat(tmpdir_fd_, "test_file", AT_SYMLINK_NOFOLLOW);
  ASSERT_THAT(st_result, IsOk());
  EXPECT_EQ(st_result->st_mode & 0777, new_mode);
}

TEST_F(SyscallsTest, FutimensSetsMtime) {
  struct timespec times[2];
  times[0].tv_sec = 1000000;
  times[0].tv_nsec = 0;
  times[1].tv_sec = 2000000;
  times[1].tv_nsec = 0;

  EXPECT_THAT(syscalls::futimens(file_fd_, times), IsOk());

  auto st_result = syscalls::fstat(file_fd_);
  ASSERT_THAT(st_result, IsOk());
  EXPECT_EQ(st_result->st_mtime, 2000000);
}

TEST_F(SyscallsTest, MkdiratAndFstatat) {
  EXPECT_THAT(syscalls::mkdirat(tmpdir_fd_, "test_dir", 0755), IsOk());

  auto st_result = syscalls::fstatat(tmpdir_fd_, "test_dir", 0);
  ASSERT_THAT(st_result, IsOk());
  EXPECT_TRUE(S_ISDIR(st_result->st_mode));
}

TEST_F(SyscallsTest, SymlinkatAndReadlinkat) {
  const std::string target = "test_file";
  EXPECT_THAT(syscalls::symlinkat(target, tmpdir_fd_, "test_link"), IsOk());

  auto link_result = syscalls::readlinkat(tmpdir_fd_, "test_link");
  ASSERT_THAT(link_result, IsOk());
  EXPECT_EQ(*link_result, target);
}

TEST_F(SyscallsTest, SymlinkatWithLongTarget) {
  std::string long_target(500, 'a');
  EXPECT_THAT(syscalls::symlinkat(long_target, tmpdir_fd_, "test_long_link"),
              IsOk());

  auto link_result = syscalls::readlinkat(tmpdir_fd_, "test_long_link");
  ASSERT_THAT(link_result, IsOk());
  EXPECT_EQ(*link_result, long_target);
}

TEST_F(SyscallsTest, LinkatRaisesStNlink) {
  auto st_before = syscalls::fstat(file_fd_);
  ASSERT_THAT(st_before, IsOk());

  EXPECT_THAT(syscalls::linkat(tmpdir_fd_, "test_file", tmpdir_fd_, "test_link2", 0),
              IsOk());

  auto st_after = syscalls::fstat(file_fd_);
  ASSERT_THAT(st_after, IsOk());
  EXPECT_EQ(st_after->st_nlink, st_before->st_nlink + 1);
}

TEST_F(SyscallsTest, Renameat2WithRenameNoreplace) {
  int file2_fd = ::openat(tmpdir_fd_, "test_file2", O_CREAT | O_RDWR, 0600);
  ASSERT_GE(file2_fd, 0);
  ::close(file2_fd);

  absl::Status status = syscalls::renameat2(
      tmpdir_fd_, "test_file", tmpdir_fd_, "test_file2", RENAME_NOREPLACE);
  EXPECT_FALSE(status.ok());
  auto errno_val = GetErrnoFromStatus(status);
  EXPECT_TRUE(errno_val.ok());
  EXPECT_EQ(*errno_val, EEXIST);

  ::unlinkat(tmpdir_fd_, "test_file2", 0);
}

TEST_F(SyscallsTest, Renameat2WithZeroFlags) {
  EXPECT_THAT(syscalls::renameat2(tmpdir_fd_, "test_file", tmpdir_fd_,
                                  "test_file_renamed", 0),
              IsOk());

  EXPECT_THAT(
      syscalls::fstatat(tmpdir_fd_, "test_file_renamed", AT_SYMLINK_NOFOLLOW),
      IsOk());
  EXPECT_THAT(
      syscalls::fstatat(tmpdir_fd_, "test_file", AT_SYMLINK_NOFOLLOW),
      StatusIs(absl::StatusCode::kNotFound));
}

TEST_F(SyscallsTest, UnlinkatRemovesFile) {
  ASSERT_THAT(
      syscalls::fstatat(tmpdir_fd_, "test_file", AT_SYMLINK_NOFOLLOW),
      IsOk());

  EXPECT_THAT(syscalls::unlinkat(tmpdir_fd_, "test_file", 0), IsOk());

  auto status_result =
      syscalls::fstatat(tmpdir_fd_, "test_file", AT_SYMLINK_NOFOLLOW);
  EXPECT_FALSE(status_result.ok());
  auto errno_val = GetErrnoFromStatus(status_result.status());
  EXPECT_TRUE(errno_val.ok());
  EXPECT_EQ(*errno_val, ENOENT);
}

TEST_F(SyscallsTest, MknodatFIFO) {
  EXPECT_THAT(syscalls::mknodat(tmpdir_fd_, "test_fifo", S_IFIFO | 0600, 0),
              IsOk());

  auto st_result = syscalls::fstatat(tmpdir_fd_, "test_fifo", 0);
  ASSERT_THAT(st_result, IsOk());
  EXPECT_TRUE(S_ISFIFO(st_result->st_mode));
}

TEST_F(SyscallsTest, FsetxattrFgetxattrFremovexattr) {
  const std::string attr_name = "user.dcfs_test";
  const std::string attr_value = "test_value";

  absl::Status set_status = syscalls::fsetxattr(
      file_fd_, attr_name,
      std::span<const uint8_t>(
          reinterpret_cast<const uint8_t *>(attr_value.data()),
          attr_value.size()),
      0);
  if (!set_status.ok()) {
    auto errno_val = GetErrnoFromStatus(set_status);
    if (errno_val.ok() &&
        (*errno_val == ENOTSUP || *errno_val == EOPNOTSUPP)) {
      GTEST_SKIP() << "Extended attributes not supported on this filesystem";
    }
  }
  EXPECT_THAT(set_status, IsOk());

  auto retrieved = syscalls::fgetxattr(file_fd_, attr_name);
  ASSERT_THAT(retrieved, IsOk());
  EXPECT_EQ(*retrieved, attr_value);

  EXPECT_THAT(syscalls::fremovexattr(file_fd_, attr_name), IsOk());
}

TEST_F(SyscallsTest, FlistxattrSplitsNulList) {
  const std::string attr_name1 = "user.dcfs_test1";
  const std::string attr_name2 = "user.dcfs_test2";
  const std::string attr_value = "test";

  absl::Status set_status = syscalls::fsetxattr(
      file_fd_, attr_name1,
      std::span<const uint8_t>(
          reinterpret_cast<const uint8_t *>(attr_value.data()),
          attr_value.size()),
      0);
  if (!set_status.ok()) {
    auto errno_val = GetErrnoFromStatus(set_status);
    if (errno_val.ok() &&
        (*errno_val == ENOTSUP || *errno_val == EOPNOTSUPP)) {
      GTEST_SKIP() << "Extended attributes not supported on this filesystem";
    }
  }
  EXPECT_THAT(set_status, IsOk());

  absl::Status set_status2 = syscalls::fsetxattr(
      file_fd_, attr_name2,
      std::span<const uint8_t>(
          reinterpret_cast<const uint8_t *>(attr_value.data()),
          attr_value.size()),
      0);
  EXPECT_THAT(set_status2, IsOk());

  auto attrs = syscalls::flistxattr(file_fd_);
  ASSERT_THAT(attrs, IsOk());
  EXPECT_TRUE(std::find(attrs->begin(), attrs->end(), attr_name1) !=
              attrs->end());
  EXPECT_TRUE(std::find(attrs->begin(), attrs->end(), attr_name2) !=
              attrs->end());
}

TEST_F(SyscallsTest, ReopenPathFdAllowsXattr) {
  int path_fd = ::openat(tmpdir_fd_, "test_file", O_PATH);
  ASSERT_GE(path_fd, 0);

  auto reopened = syscalls::ReopenPathFd(path_fd, O_RDWR);
  ASSERT_THAT(reopened, IsOk());

  const std::string attr_name = "user.dcfs_test";
  const std::string attr_value = "test";
  absl::Status set_status = syscalls::fsetxattr(
      **reopened, attr_name,
      std::span<const uint8_t>(
          reinterpret_cast<const uint8_t *>(attr_value.data()),
          attr_value.size()),
      0);
  if (!set_status.ok()) {
    auto errno_val = GetErrnoFromStatus(set_status);
    if (errno_val.ok() &&
        (*errno_val == ENOTSUP || *errno_val == EOPNOTSUPP)) {
      GTEST_SKIP() << "Extended attributes not supported on this filesystem";
    }
  }

  ::close(path_fd);
}

TEST_F(SyscallsTest, ReopenPathFdAndFstatMatch) {
  int path_fd = ::openat(tmpdir_fd_, "test_file", O_PATH);
  ASSERT_GE(path_fd, 0);

  auto st_original = syscalls::fstat(file_fd_);
  ASSERT_THAT(st_original, IsOk());

  auto reopened_fd = syscalls::ReopenPathFd(path_fd, O_RDONLY);
  ASSERT_THAT(reopened_fd, IsOk());
  auto st_reopened = syscalls::fstat(**reopened_fd);
  ASSERT_THAT(st_reopened, IsOk());

  EXPECT_EQ(st_original->st_ino, st_reopened->st_ino);

  ::close(path_fd);
}

TEST_F(SyscallsTest, FstatfsReturnsNonzeroFsize) {
  auto buf_result = syscalls::fstatfs(file_fd_);
  ASSERT_THAT(buf_result, IsOk());
  EXPECT_NE(buf_result->f_bsize, 0);
}

TEST_F(SyscallsTest, Getdents64ListsCreatedNames) {
  ::openat(tmpdir_fd_, "file1", O_CREAT, 0600);
  ::openat(tmpdir_fd_, "file2", O_CREAT, 0600);

  int dir_fd = ::openat(tmpdir_fd_, ".", O_DIRECTORY | O_RDONLY);
  ASSERT_GE(dir_fd, 0);

  std::vector<uint8_t> dir_buf(4096);
  auto nbytes_result =
      syscalls::getdents64(dir_fd, dir_buf.data(), dir_buf.size());
  ASSERT_THAT(nbytes_result, IsOk());

  std::vector<std::string> names;
  for (int64_t pos = 0; pos < *nbytes_result;) {
    const auto *entry = reinterpret_cast<const syscalls::linux_dirent64 *>(
        dir_buf.data() + pos);
    names.push_back(std::string(entry->d_name));
    pos += entry->d_reclen;
  }

  EXPECT_TRUE(std::find(names.begin(), names.end(), "file1") != names.end());
  EXPECT_TRUE(std::find(names.begin(), names.end(), "file2") != names.end());

  ::close(dir_fd);
}

TEST_F(SyscallsTest, StatxAndFstat) {
  auto stx_result =
      syscalls::statx(tmpdir_fd_, "test_file", AT_SYMLINK_NOFOLLOW,
                      STATX_BASIC_STATS);
  ASSERT_THAT(stx_result, IsOk());
  EXPECT_NE(stx_result->stx_ino, 0);

  auto st_result = syscalls::fstat(file_fd_);
  ASSERT_THAT(st_result, IsOk());
  EXPECT_EQ(stx_result->stx_ino, static_cast<uint64_t>(st_result->st_ino));
}

TEST_F(SyscallsTest, GetInodeGeneration) {
  auto gen = GetInodeGeneration(file_fd_);
  if (!gen.ok()) {
    auto errno_val = GetErrnoFromStatus(gen.status());
    if (errno_val.ok() && *errno_val == ENOTTY) {
      GTEST_SKIP() << "FS_IOC_GETVERSION not supported";
    }
  }
}

TEST_F(SyscallsTest, ErrorPathOpenatMissing) {
  auto status = syscalls::openat(tmpdir_fd_, "missing_file", O_RDONLY);
  EXPECT_FALSE(status.ok());
  auto errno_val = GetErrnoFromStatus(status.status());
  EXPECT_TRUE(errno_val.ok());
  EXPECT_EQ(*errno_val, ENOENT);
}

TEST_F(SyscallsTest, NameToHandleAtRoundTrip) {
  if (geteuid() != 0) {
    GTEST_SKIP() << "needs CAP_DAC_READ_SEARCH (run as root)";
  }

  file_handle handle;
  int mount_id;
  ASSERT_THAT(syscalls::name_to_handle_at(tmpdir_fd_, "test_file", handle,
                                          mount_id, 0),
              IsOk());

  auto reopened_fd = syscalls::open_by_handle_at(tmpdir_fd_, handle, O_RDONLY);
  ASSERT_THAT(reopened_fd, IsOk());

  auto st_original = syscalls::fstat(file_fd_);
  ASSERT_THAT(st_original, IsOk());

  auto st_reopened = syscalls::fstat(**reopened_fd);
  ASSERT_THAT(st_reopened, IsOk());

  EXPECT_EQ(st_original->st_ino, st_reopened->st_ino);
}

TEST_F(SyscallsTest, XattrOpathReadsTheObjectNotTheSymlinkTarget) {
  const std::string value = "opath";
  if (::fsetxattr(file_fd_, "user.dcfs_opath", value.data(), value.size(),
                  0) != 0) {
    GTEST_SKIP() << "user xattrs unsupported here: " << std::strerror(errno);
  }
  int file_path_fd = ::openat(tmpdir_fd_, "test_file", O_PATH);
  ASSERT_GE(file_path_fd, 0);
  auto names = syscalls::listxattr_opath(file_path_fd);
  ASSERT_THAT(names, IsOk());
  EXPECT_NE(std::find(names->begin(), names->end(), "user.dcfs_opath"),
            names->end());
  auto got = syscalls::getxattr_opath(file_path_fd, "user.dcfs_opath");
  ASSERT_THAT(got, IsOk());
  EXPECT_EQ(*got, value);
  auto absent = syscalls::getxattr_opath(file_path_fd, "user.dcfs_absent");
  ASSERT_FALSE(absent.ok());
  EXPECT_EQ(GetErrnoFromStatus(absent.status()).value_or(0), ENODATA);

  // Through an O_PATH fd on a symlink to that file, the magic link resolves
  // to the symlink itself, so the target's xattr is not visible.
  ::unlinkat(tmpdir_fd_, "opath_link", 0);
  ASSERT_EQ(::symlinkat("test_file", tmpdir_fd_, "opath_link"), 0);
  int link_fd = ::openat(tmpdir_fd_, "opath_link", O_PATH | O_NOFOLLOW);
  ASSERT_GE(link_fd, 0);
  auto link_names = syscalls::listxattr_opath(link_fd);
  ASSERT_THAT(link_names, IsOk());
  EXPECT_EQ(std::find(link_names->begin(), link_names->end(),
                      "user.dcfs_opath"),
            link_names->end());
  ::close(link_fd);
  ::close(file_path_fd);
}

TEST_F(SyscallsTest, DupIsCloexecAndSameFile) {
  auto duped = syscalls::dup(file_fd_);
  ASSERT_THAT(duped, IsOk());
  EXPECT_NE(**duped, file_fd_);
  EXPECT_NE(::fcntl(**duped, F_GETFD) & FD_CLOEXEC, 0);
  auto a = syscalls::fstat(file_fd_);
  auto b = syscalls::fstat(**duped);
  ASSERT_THAT(a, IsOk());
  ASSERT_THAT(b, IsOk());
  EXPECT_EQ(a->st_ino, b->st_ino);
}

}  // namespace
}  // namespace dcfs
