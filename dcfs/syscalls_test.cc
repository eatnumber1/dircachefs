#include "dcfs/syscalls.h"
#include "dcfs/syscalls_backing.h"

#include <fcntl.h>
#include <linux/fs.h>
#include <sys/file.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "absl/status/status_matchers.h"
#include "dcfs/fd.h"
#include "dcfs/status.h"
#include "dcfs/testonly/assert_ok_and_assign.h"
#include "gtest/gtest.h"

namespace dcfs {
namespace {

using ::absl_testing::IsOk;
using ::absl_testing::IsOkAndHolds;
using ::absl_testing::StatusIs;

class SyscallsTest : public ::testing::Test {
 protected:
  void SetUp() override {
    const char *tmpdir = std::getenv("TEST_TMPDIR");
    ASSERT_NE(tmpdir, nullptr);
    ASSERT_OK_AND_ASSIGN(
        tmpdir_, syscalls::openat(AT_FDCWD, tmpdir, O_PATH | O_DIRECTORY));
    tmpdir_fd_ = *tmpdir_;

    // Create a test file
    ASSERT_OK_AND_ASSIGN(
        file_, syscalls::openat(tmpdir_fd_, "test_file", O_CREAT | O_RDWR,
                                0600));
    file_fd_ = *file_;
  }

  FileDescriptor tmpdir_;
  FileDescriptor file_;
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

  char buf[64];
  auto n = syscalls::readlinkat(tmpdir_fd_, "test_link", buf, sizeof(buf));
  ASSERT_THAT(n, IsOk());
  EXPECT_EQ(std::string(buf, *n), target);
  // A full buffer is the caller's cue that the target may be longer.
  EXPECT_THAT(syscalls::readlinkat(tmpdir_fd_, "test_link", buf, 4),
              IsOkAndHolds(4u));
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
  ASSERT_THAT(syscalls::openat(tmpdir_fd_, "test_file2", O_CREAT | O_RDWR,
                               0600),
              IsOk());  // the descriptor closes at once

  absl::Status status = syscalls::renameat2(
      tmpdir_fd_, "test_file", tmpdir_fd_, "test_file2", RENAME_NOREPLACE);
  EXPECT_FALSE(status.ok());
  auto errno_val = GetErrnoFromStatus(status);
  EXPECT_TRUE(errno_val.ok());
  EXPECT_EQ(*errno_val, EEXIST);

  EXPECT_THAT(syscalls::unlinkat(tmpdir_fd_, "test_file2", 0), IsOk());
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

  // A null buffer asks for the size; then read it.
  EXPECT_THAT(syscalls::fgetxattr(file_fd_, attr_name, nullptr, 0),
              IsOkAndHolds(attr_value.size()));
  std::string retrieved(attr_value.size(), '\0');
  EXPECT_THAT(syscalls::fgetxattr(file_fd_, attr_name, retrieved.data(),
                                  retrieved.size()),
              IsOkAndHolds(attr_value.size()));
  EXPECT_EQ(retrieved, attr_value);
  // A buffer that is too small is ERANGE (the caller's loop retries).
  char tiny[1];
  auto too_small = syscalls::fgetxattr(file_fd_, attr_name, tiny, 1);
  ASSERT_FALSE(too_small.ok());
  EXPECT_EQ(GetErrnoFromStatus(too_small.status()).value_or(0), ERANGE);

  EXPECT_THAT(syscalls::fremovexattr(file_fd_, attr_name), IsOk());
}

TEST_F(SyscallsTest, FlistxattrReturnsTheNulSeparatedList) {
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

  auto size = syscalls::flistxattr(file_fd_, nullptr, 0);
  ASSERT_THAT(size, IsOk());
  std::string buf(*size, '\0');
  EXPECT_THAT(syscalls::flistxattr(file_fd_, buf.data(), buf.size()),
              IsOkAndHolds(*size));
  // The raw result: names, each followed by a NUL.
  const std::string_view list(buf);
  EXPECT_NE(list.find(attr_name1 + '\0'), std::string_view::npos);
  EXPECT_NE(list.find(attr_name2 + '\0'), std::string_view::npos);
}

TEST_F(SyscallsTest, FstatfsReturnsNonzeroFsize) {
  auto buf_result = syscalls::fstatfs(file_fd_);
  ASSERT_THAT(buf_result, IsOk());
  EXPECT_NE(buf_result->f_bsize, 0);
}

TEST_F(SyscallsTest, Getdents64ListsCreatedNames) {
  ASSERT_THAT(syscalls::openat(tmpdir_fd_, "file1", O_CREAT, 0600), IsOk());
  ASSERT_THAT(syscalls::openat(tmpdir_fd_, "file2", O_CREAT, 0600), IsOk());

  ASSERT_OK_AND_ASSIGN(
      FileDescriptor dir,
      syscalls::openat(tmpdir_fd_, ".", O_DIRECTORY | O_RDONLY));
  const int dir_fd = *dir;

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

// Regression test for the bug where syscalls::ioctl() built its failure via
// absl::ErrnoToStatus() (no errno payload) instead of dcfs::ErrnoToStatus():
// without the payload, GetErrnoFromStatus() cannot recover ENOTTY here, and
// callers like backing::ReadGeneration() that switch on the errno cannot
// tell "unsupported" apart from a real error. TEST_TMPDIR may be backed by
// a real disk (see test/qemu/guest/init), so this uses /tmp directly, which
// is always tmpfs in the QEMU guest and does not implement
// FS_IOC_GETVERSION.
TEST(SyscallsTmpfsTest, IoctlOnTmpfsIsEnottyWithTheErrnoPayload) {
  std::string path = "/tmp/dcfs_syscalls_test_tmpfs_XXXXXX";
  ASSERT_OK_AND_ASSIGN(FileDescriptor file, syscalls::mkstemp(path));
  const int fd = *file;
  ASSERT_THAT(syscalls::unlinkat(AT_FDCWD, path, 0), IsOk());

  uint32_t generation = 0;
  auto gen = syscalls::ioctl(fd, FS_IOC_GETVERSION, &generation);
  ASSERT_FALSE(gen.ok());
  auto errno_val = GetErrnoFromStatus(gen.status());
  ASSERT_THAT(errno_val, IsOk());
  EXPECT_EQ(*errno_val, ENOTTY);
}

TEST_F(SyscallsTest, ErrorPathOpenatMissing) {
  auto status = syscalls::openat(tmpdir_fd_, "missing_file", O_RDONLY);
  EXPECT_FALSE(status.ok());
  auto errno_val = GetErrnoFromStatus(status.status());
  EXPECT_TRUE(errno_val.ok());
  EXPECT_EQ(*errno_val, ENOENT);
}

TEST_F(SyscallsTest, NameToHandleAtRoundTrip) {
  // struct file_handle is variable-length (a handle_bytes-sized flexible
  // array member beyond the fixed header); a bare stack `file_handle`
  // provides storage for the header only, and its handle_bytes starts
  // uninitialized. Both bugs together used to make this call fail with
  // EINVAL (handle_bytes read as garbage > MAX_HANDLE_SZ) or, worse,
  // overflow the stack variable when it didn't -- undetected until this
  // test actually ran (it used to be skipped unconditionally on an
  // unprivileged host). Allocate real storage and set handle_bytes first,
  // as dcfs/file_handle.cc itself does.
  std::vector<uint8_t> handle_buf(sizeof(file_handle) + MAX_HANDLE_SZ);
  auto *handle = reinterpret_cast<file_handle *>(handle_buf.data());
  handle->handle_bytes = MAX_HANDLE_SZ;
  int mount_id;
  ASSERT_THAT(syscalls::name_to_handle_at(tmpdir_fd_, "test_file", *handle,
                                          mount_id, 0),
              IsOk());

  // open_by_handle_at's mount_fd argument is resolved through the
  // kernel's non-raw fd class (fs/fhandle.c get_path_from_fd()), which
  // rejects O_PATH descriptors with EBADF -- tmpdir_fd_ is O_PATH (see
  // SetUp), so a separate real fd is needed here.
  auto real_tmpdir_fd = syscalls::openat(
      AT_FDCWD, "/proc/self/fd/" + std::to_string(tmpdir_fd_),
      O_RDONLY | O_DIRECTORY);
  ASSERT_THAT(real_tmpdir_fd, IsOk());
  auto reopened_fd =
      syscalls::open_by_handle_at(**real_tmpdir_fd, *handle, O_RDONLY);
  ASSERT_THAT(reopened_fd, IsOk());

  auto st_original = syscalls::fstat(file_fd_);
  ASSERT_THAT(st_original, IsOk());

  auto st_reopened = syscalls::fstat(**reopened_fd);
  ASSERT_THAT(st_reopened, IsOk());

  EXPECT_EQ(st_original->st_ino, st_reopened->st_ino);
}

// The path-following variants, on the "/proc/self/fd/<fd>" path of an
// O_PATH descriptor (how backing.cc reaches objects it cannot open).
TEST_F(SyscallsTest, PathXattrCallsOnAProcFdPath) {
  const std::string value = "opath";
  if (absl::Status set = syscalls::fsetxattr(
          file_fd_, "user.dcfs_opath",
          std::span<const uint8_t>(
              reinterpret_cast<const uint8_t *>(value.data()), value.size()),
          0);
      !set.ok()) {
    GTEST_SKIP() << "user xattrs unsupported here: " << set;
  }
  const std::string path = "/proc/self/fd/" + std::to_string(file_fd_);

  EXPECT_THAT(syscalls::getxattr(path, "user.dcfs_opath", nullptr, 0),
              IsOkAndHolds(value.size()));
  std::string got(value.size(), '\0');
  EXPECT_THAT(
      syscalls::getxattr(path, "user.dcfs_opath", got.data(), got.size()),
      IsOkAndHolds(value.size()));
  EXPECT_EQ(got, value);
  auto absent = syscalls::getxattr(path, "user.dcfs_absent", nullptr, 0);
  ASSERT_FALSE(absent.ok());
  EXPECT_EQ(GetErrnoFromStatus(absent.status()).value_or(0), ENODATA);

  auto size = syscalls::listxattr(path, nullptr, 0);
  ASSERT_THAT(size, IsOk());
  std::string list(*size, '\0');
  EXPECT_THAT(syscalls::listxattr(path, list.data(), list.size()),
              IsOkAndHolds(*size));
  EXPECT_NE(list.find(std::string("user.dcfs_opath") + '\0'),
            std::string::npos);

  const std::string more = "more";
  EXPECT_THAT(
      syscalls::setxattr(
          path, "user.dcfs_set",
          std::span<const uint8_t>(
              reinterpret_cast<const uint8_t *>(more.data()), more.size()),
          0),
      IsOk());
  EXPECT_THAT(syscalls::getxattr(path, "user.dcfs_set", nullptr, 0),
              IsOkAndHolds(more.size()));
  EXPECT_THAT(syscalls::removexattr(path, "user.dcfs_set"), IsOk());
  auto removed = syscalls::getxattr(path, "user.dcfs_set", nullptr, 0);
  ASSERT_FALSE(removed.ok());
  EXPECT_EQ(GetErrnoFromStatus(removed.status()).value_or(0), ENODATA);
}

TEST_F(SyscallsTest, FchmodatAndUtimensatOnAProcFdPath) {
  const std::string path = "/proc/self/fd/" + std::to_string(file_fd_);
  ASSERT_THAT(syscalls::fchmodat(AT_FDCWD, path, 0640, 0), IsOk());
  auto st = syscalls::fstat(file_fd_);
  ASSERT_THAT(st, IsOk());
  EXPECT_EQ(st->st_mode & 07777, 0640u);

  const struct timespec times[2] = {{1000, 0}, {2000, 0}};
  ASSERT_THAT(syscalls::utimensat(AT_FDCWD, path, times, 0), IsOk());
  st = syscalls::fstat(file_fd_);
  ASSERT_THAT(st, IsOk());
  EXPECT_EQ(st->st_atim.tv_sec, 1000);
  EXPECT_EQ(st->st_mtim.tv_sec, 2000);

  auto missing = syscalls::fchmodat(tmpdir_fd_, "no_such_file", 0600, 0);
  ASSERT_FALSE(missing.ok());
  EXPECT_EQ(GetErrnoFromStatus(missing).value_or(0), ENOENT);
}

TEST_F(SyscallsTest, RealpathResolvesAndReportsMissing) {
  ASSERT_THAT(syscalls::symlinkat("test_file", tmpdir_fd_, "real_link"),
              IsOk());
  const std::string dir = "/proc/self/fd/" + std::to_string(tmpdir_fd_);
  ASSERT_OK_AND_ASSIGN(std::string base, syscalls::realpath(dir));
  EXPECT_THAT(syscalls::realpath(dir + "/real_link"),
              IsOkAndHolds(base + "/test_file"));
  auto missing = syscalls::realpath(dir + "/no_such_file");
  ASSERT_FALSE(missing.ok());
  EXPECT_EQ(StatusToErrno(missing.status()), ENOENT);
}

TEST_F(SyscallsTest, MkdtempAndMkstempMakeFreshNames) {
  const std::string dir = "/proc/self/fd/" + std::to_string(tmpdir_fd_);
  ASSERT_OK_AND_ASSIGN(std::string made,
                       syscalls::mkdtemp(dir + "/dtemp_XXXXXX"));
  EXPECT_NE(made, dir + "/dtemp_XXXXXX");
  EXPECT_THAT(syscalls::fstatat(AT_FDCWD, made), IsOk());
  std::string pattern = dir + "/ftemp_XXXXXX";
  ASSERT_OK_AND_ASSIGN(FileDescriptor fd, syscalls::mkstemp(pattern));
  EXPECT_NE(pattern, dir + "/ftemp_XXXXXX");
  ASSERT_OK_AND_ASSIGN(int flags, syscalls::fcntl(*fd, F_GETFD));
  EXPECT_NE(flags & FD_CLOEXEC, 0);
  std::string bad = "no_x_here";
  EXPECT_FALSE(syscalls::mkstemp(bad).ok());
  EXPECT_FALSE(syscalls::mkdtemp("no_x_here").ok());
}

TEST_F(SyscallsTest, RlimitFlockClockSleepAndPid) {
  ASSERT_OK_AND_ASSIGN(struct rlimit limit, syscalls::getrlimit(RLIMIT_NOFILE));
  EXPECT_GT(limit.rlim_cur, 0u);
  EXPECT_THAT(syscalls::setrlimit(RLIMIT_NOFILE, limit), IsOk());
  EXPECT_FALSE(syscalls::getrlimit(-1).ok());

  EXPECT_THAT(syscalls::flock(file_fd_, LOCK_EX | LOCK_NB), IsOk());
  EXPECT_THAT(syscalls::flock(file_fd_, LOCK_UN), IsOk());
  EXPECT_FALSE(syscalls::flock(-1, LOCK_EX).ok());

  ASSERT_OK_AND_ASSIGN(struct timespec before,
                       syscalls::clock_gettime(CLOCK_MONOTONIC));
  const struct timespec nap = {.tv_sec = 0, .tv_nsec = 5'000'000};
  ASSERT_THAT(syscalls::nanosleep(nap), IsOk());
  ASSERT_OK_AND_ASSIGN(struct timespec after,
                       syscalls::clock_gettime(CLOCK_MONOTONIC));
  EXPECT_GE((after.tv_sec - before.tv_sec) * 1'000'000'000L +
                (after.tv_nsec - before.tv_nsec),
            5'000'000L);
  EXPECT_FALSE(syscalls::clock_gettime(static_cast<clockid_t>(-5)).ok());
  EXPECT_GT(syscalls::getpid(), 0);
}

TEST_F(SyscallsTest, MountAndUmountReportTheirErrors) {
  // Nothing is mounted on the test file, which is not a directory.
  auto not_mounted = syscalls::umount2("/proc/self/fd/" +
                                           std::to_string(file_fd_), 0);
  EXPECT_FALSE(not_mounted.ok());
  EXPECT_FALSE(syscalls::mount("tmpfs", "/no/such/dir", "tmpfs", 0, nullptr)
                   .ok());
}

TEST_F(SyscallsTest, DupIsCloexecAndSameFile) {
  auto duped = syscalls::dup(file_fd_);
  ASSERT_THAT(duped, IsOk());
  EXPECT_NE(**duped, file_fd_);
  ASSERT_OK_AND_ASSIGN(int fd_flags, syscalls::fcntl(**duped, F_GETFD));
  EXPECT_NE(fd_flags & FD_CLOEXEC, 0);
  auto a = syscalls::fstat(file_fd_);
  auto b = syscalls::fstat(**duped);
  ASSERT_THAT(a, IsOk());
  ASSERT_THAT(b, IsOk());
  EXPECT_EQ(a->st_ino, b->st_ino);
}

TEST(SyscallsCredentialsTest, SetfsuidReturnsPreviousAndReadsBack) {
  constexpr uid_t kRead = static_cast<uid_t>(-1);  // changes nothing
  ASSERT_EQ(syscalls::setfsuid(kRead), 0u);
  EXPECT_EQ(syscalls::setfsuid(1000), 0u);
  EXPECT_EQ(syscalls::setfsuid(kRead), 1000u);
  EXPECT_EQ(syscalls::setfsuid(0), 1000u);
  EXPECT_EQ(syscalls::setfsuid(kRead), 0u);
  ASSERT_EQ(syscalls::setfsgid(kRead), 0u);
  EXPECT_EQ(syscalls::setfsgid(1000), 0u);
  EXPECT_EQ(syscalls::setfsgid(kRead), 1000u);
  EXPECT_EQ(syscalls::setfsgid(0), 1000u);
  EXPECT_EQ(syscalls::setfsgid(kRead), 0u);
}

}  // namespace
}  // namespace dcfs
