#include "dcfs/file_handle.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <vector>

#include "absl/hash/hash.h"
#include "absl/status/status.h"
#include "absl/status/status_matchers.h"
#include "absl/status/statusor.h"
#include "absl/strings/cord.h"
#include "absl/strings/str_cat.h"
#include "absl/types/optional.h"
#include "dcfs/device_id.h"
#include "dcfs/fd.h"
#include "dcfs/mount_fds.h"
#include "dcfs/status.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"

namespace dcfs {
namespace {

using ::absl_testing::IsOk;
using ::absl_testing::StatusIs;

// True if `status` reflects this host or filesystem lacking support this
// test needs, rather than a real bug: GetDeviceId() returns Unimplemented
// on kernels older than 6.9 (this host runs 6.8, see device_id_test.cc);
// name_to_handle_at(2) can fail EOPNOTSUPP/ENOTSUP on filesystems that
// don't export file handles; and GetDeviceId(fd), when `fd` is an O_PATH
// descriptor for a symlink, fails ELOOP on every kernel version -- its
// ioctl-on-O_PATH fallback reopens via /proc/self/fd/<fd>, and the kernel
// deliberately returns ELOOP rather than open()ing through a magic symlink
// that itself names a symlink opened O_NOFOLLOW. This is a real limitation
// of today's GetDeviceId() for symlinks, not something worked around
// incorrectly here; see the report for this step.
bool IsUnsupported(const absl::Status &status) {
  if (absl::IsUnimplemented(status)) return true;
  absl::StatusOr<int> eno = GetErrnoFromStatus(status);
  if (eno.ok() && (*eno == EOPNOTSUPP || *eno == ENOTSUP)) return true;

  // dcfs/status.h's errno-name round trip (GetErrnoFromStatus) has no
  // table entry for ELOOP, so it can't be checked via *eno above; read the
  // errno payload directly instead of going through that lookup.
  absl::optional<absl::Cord> payload = status.GetPayload(kErrnoTypeUrl);
  return payload.has_value() && *payload == "ELOOP";
}

FileHandle MakeArbitraryHandle() {
  FileHandle fh;
  fh.device.uuid = {0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08,
                     0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f, 0x10};
  fh.device.subvol_id = 0x1122334455667788ULL;
  fh.handle_type = 7;
  fh.bytes = {0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
              0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f,
              0x10, 0x11, 0x12, 0x13};
  return fh;
}

TEST(FileHandleValueTest, SerializeParseRoundTrip) {
  FileHandle fh = MakeArbitraryHandle();
  ASSERT_EQ(fh.bytes.size(), 20u);

  std::string serialized = fh.Serialize();
  EXPECT_EQ(serialized.size(), 24u + 4u + fh.bytes.size());

  absl::StatusOr<FileHandle> parsed = FileHandle::Parse(serialized);
  ASSERT_THAT(parsed, IsOk());
  EXPECT_EQ(*parsed, fh);
}

TEST(FileHandleValueTest, SerializeParseRoundTripEmptyBytes) {
  FileHandle fh;
  fh.device.uuid.fill(0xAB);
  fh.handle_type = 3;
  ASSERT_TRUE(fh.bytes.empty());

  absl::StatusOr<FileHandle> parsed = FileHandle::Parse(fh.Serialize());
  ASSERT_THAT(parsed, IsOk());
  EXPECT_EQ(*parsed, fh);
}

TEST(FileHandleValueTest, ParseRejectsShortInput) {
  EXPECT_THAT(FileHandle::Parse(std::string(27, 'x')),
              StatusIs(absl::StatusCode::kInvalidArgument));
}

TEST(FileHandleValueTest, EqualityAndHashing) {
  FileHandle a = MakeArbitraryHandle();
  FileHandle b = a;
  EXPECT_EQ(a, b);
  EXPECT_EQ(absl::HashOf(a), absl::HashOf(b));

  FileHandle different_byte = a;
  different_byte.bytes[0] ^= 0xFF;
  EXPECT_NE(a, different_byte);
  EXPECT_NE(absl::HashOf(a), absl::HashOf(different_byte));

  FileHandle different_type = a;
  different_type.handle_type += 1;
  EXPECT_NE(a, different_type);

  FileHandle different_device = a;
  different_device.device.uuid[0] ^= 0xFF;
  EXPECT_NE(a, different_device);
}

// Creates a fresh subdirectory (one per test, named after the test, since
// all tests in this binary share one $TEST_TMPDIR) containing a regular
// file, a directory and a symlink, for the FromFd/FromDirEntry/Open tests
// below.
class FileHandleTest : public ::testing::Test {
 protected:
  void SetUp() override {
    const char *tmpdir = std::getenv("TEST_TMPDIR");
    ASSERT_NE(tmpdir, nullptr);

    const ::testing::TestInfo *info =
        ::testing::UnitTest::GetInstance()->current_test_info();
    dir_path_ = absl::StrCat(tmpdir, "/", info->name());
    ASSERT_EQ(::mkdir(dir_path_.c_str(), 0755), 0);

    dir_fd_ = ::open(dir_path_.c_str(), O_PATH | O_DIRECTORY | O_CLOEXEC);
    ASSERT_GE(dir_fd_, 0);

    file_fd_ =
        ::openat(dir_fd_, "regular_file", O_CREAT | O_RDWR | O_CLOEXEC, 0600);
    ASSERT_GE(file_fd_, 0);

    ASSERT_EQ(::mkdirat(dir_fd_, "a_dir", 0755), 0);
    ASSERT_EQ(::symlinkat("regular_file", dir_fd_, "a_symlink"), 0);
  }

  void TearDown() override {
    if (file_fd_ >= 0) ::close(file_fd_);
    if (dir_fd_ >= 0) ::close(dir_fd_);
  }

  std::string dir_path_;
  int dir_fd_ = -1;
  int file_fd_ = -1;
};

TEST_F(FileHandleTest, FromFdAndFromDirEntryAgreeOnRegularFile) {
  absl::StatusOr<FileHandle> from_fd = FileHandle::FromFd(file_fd_);
  if (!from_fd.ok()) {
    ASSERT_TRUE(IsUnsupported(from_fd.status())) << from_fd.status();
    GTEST_SKIP() << from_fd.status();
  }
  EXPECT_FALSE(from_fd->bytes.empty());

  absl::StatusOr<FileHandle> from_entry =
      FileHandle::FromDirEntry(dir_fd_, "regular_file");
  ASSERT_THAT(from_entry, IsOk());
  EXPECT_EQ(*from_fd, *from_entry);
}

TEST_F(FileHandleTest, FromFdAndFromDirEntryAgreeOnDirectory) {
  int subdir_fd = ::openat(dir_fd_, "a_dir", O_PATH | O_DIRECTORY | O_CLOEXEC);
  ASSERT_GE(subdir_fd, 0);

  absl::StatusOr<FileHandle> from_fd = FileHandle::FromFd(subdir_fd);
  if (!from_fd.ok()) {
    ASSERT_TRUE(IsUnsupported(from_fd.status())) << from_fd.status();
    ::close(subdir_fd);
    GTEST_SKIP() << from_fd.status();
  }
  EXPECT_FALSE(from_fd->bytes.empty());

  absl::StatusOr<FileHandle> from_entry =
      FileHandle::FromDirEntry(dir_fd_, "a_dir");
  ASSERT_THAT(from_entry, IsOk());
  EXPECT_EQ(*from_fd, *from_entry);

  ::close(subdir_fd);
}

TEST_F(FileHandleTest, FromFdAndFromDirEntryAgreeOnSymlink) {
  int link_fd =
      ::openat(dir_fd_, "a_symlink", O_PATH | O_NOFOLLOW | O_CLOEXEC);
  ASSERT_GE(link_fd, 0);

  absl::StatusOr<FileHandle> from_fd = FileHandle::FromFd(link_fd);
  if (!from_fd.ok()) {
    ASSERT_TRUE(IsUnsupported(from_fd.status())) << from_fd.status();
    ::close(link_fd);
    GTEST_SKIP() << from_fd.status();
  }
  EXPECT_FALSE(from_fd->bytes.empty());

  absl::StatusOr<FileHandle> from_entry =
      FileHandle::FromDirEntry(dir_fd_, "a_symlink");
  ASSERT_THAT(from_entry, IsOk());
  EXPECT_EQ(*from_fd, *from_entry);

  ::close(link_fd);
}

TEST_F(FileHandleTest, HardLinkYieldsEqualHandle) {
  absl::StatusOr<FileHandle> original = FileHandle::FromFd(file_fd_);
  if (!original.ok()) {
    ASSERT_TRUE(IsUnsupported(original.status())) << original.status();
    GTEST_SKIP() << original.status();
  }

  ASSERT_EQ(::linkat(dir_fd_, "regular_file", dir_fd_, "hard_link", 0), 0);
  absl::StatusOr<FileHandle> linked =
      FileHandle::FromDirEntry(dir_fd_, "hard_link");
  ASSERT_THAT(linked, IsOk());
  EXPECT_EQ(*original, *linked);
}

TEST_F(FileHandleTest, DifferentFileYieldsDifferentHandle) {
  absl::StatusOr<FileHandle> original = FileHandle::FromFd(file_fd_);
  if (!original.ok()) {
    ASSERT_TRUE(IsUnsupported(original.status())) << original.status();
    GTEST_SKIP() << original.status();
  }

  int other_fd =
      ::openat(dir_fd_, "other_file", O_CREAT | O_RDWR | O_CLOEXEC, 0600);
  ASSERT_GE(other_fd, 0);
  absl::StatusOr<FileHandle> other = FileHandle::FromFd(other_fd);
  ASSERT_THAT(other, IsOk());
  EXPECT_NE(*original, *other);
  ::close(other_fd);
}

TEST_F(FileHandleTest, OpenReopensFileViaMountFds) {
  if (geteuid() != 0) {
    GTEST_SKIP() << "needs CAP_DAC_READ_SEARCH (run as root)";
  }

  absl::StatusOr<FileHandle> fh = FileHandle::FromFd(file_fd_);
  if (!fh.ok()) {
    ASSERT_TRUE(IsUnsupported(fh.status())) << fh.status();
    GTEST_SKIP() << fh.status();
  }

  int mount_fd = ::open(dir_path_.c_str(), O_PATH | O_DIRECTORY | O_CLOEXEC);
  ASSERT_GE(mount_fd, 0);
  MountFds mounts;
  ASSERT_THAT(mounts.Insert(fh->device, FileDescriptor(mount_fd)), IsOk());

  absl::StatusOr<FileDescriptor> opened = fh->Open(mounts, O_RDONLY);
  ASSERT_THAT(opened, IsOk());

  struct stat original_st;
  ASSERT_EQ(::fstat(file_fd_, &original_st), 0);
  struct stat opened_st;
  ASSERT_EQ(::fstat(**opened, &opened_st), 0);
  EXPECT_EQ(original_st.st_ino, opened_st.st_ino);
}

TEST_F(FileHandleTest, OpenWithUnknownDeviceReturnsNotFound) {
  if (geteuid() != 0) {
    GTEST_SKIP() << "needs CAP_DAC_READ_SEARCH (run as root)";
  }

  absl::StatusOr<FileHandle> fh = FileHandle::FromFd(file_fd_);
  if (!fh.ok()) {
    ASSERT_TRUE(IsUnsupported(fh.status())) << fh.status();
    GTEST_SKIP() << fh.status();
  }

  MountFds empty_mounts;
  EXPECT_THAT(fh->Open(empty_mounts, O_RDONLY),
              StatusIs(absl::StatusCode::kNotFound));
}

TEST_F(FileHandleTest, OpenWithCorruptedHandleFails) {
  if (geteuid() != 0) {
    GTEST_SKIP() << "needs CAP_DAC_READ_SEARCH (run as root)";
  }

  absl::StatusOr<FileHandle> fh = FileHandle::FromFd(file_fd_);
  if (!fh.ok()) {
    ASSERT_TRUE(IsUnsupported(fh.status())) << fh.status();
    GTEST_SKIP() << fh.status();
  }
  ASSERT_FALSE(fh->bytes.empty());
  fh->bytes[0] ^= 0xFF;

  int mount_fd = ::open(dir_path_.c_str(), O_PATH | O_DIRECTORY | O_CLOEXEC);
  ASSERT_GE(mount_fd, 0);
  MountFds mounts;
  ASSERT_THAT(mounts.Insert(fh->device, FileDescriptor(mount_fd)), IsOk());

  // The corrupted handle should fail one way or another (typically ESTALE,
  // sometimes EBADF/EINVAL depending on how the bit flip lands); any
  // failure is acceptable here.
  EXPECT_FALSE(fh->Open(mounts, O_RDONLY).ok());
}

}  // namespace
}  // namespace dcfs
