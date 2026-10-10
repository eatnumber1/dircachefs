#include "dcfs/file_handle.h"

#include <fcntl.h>
#include <linux/stat.h>
#include <sys/stat.h>

#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "absl/hash/hash.h"
#include "absl/status/status.h"
#include "absl/status/status_matchers.h"
#include "absl/status/statusor.h"
#include "absl/strings/str_cat.h"
#include "dcfs/device_id.h"
#include "dcfs/fd.h"
#include "dcfs/mount_fds.h"
#include "dcfs/status.h"
#include "dcfs/syscalls_backing.h"
#include "dcfs/testonly/assert_ok_and_assign.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"

namespace dcfs {
namespace {

using ::absl_testing::IsOk;
using ::absl_testing::IsOkAndHolds;
using ::absl_testing::StatusIs;
using ::testing::ContainsRegex;
using ::testing::EndsWith;
using ::testing::HasSubstr;
using ::testing::Not;
using ::testing::Optional;

// True if `status` reflects the backing filesystem lacking support this
// test needs, rather than a real bug: GetDeviceId() returns Unimplemented
// on filesystems without FS_IOC_GETFSUUID support (e.g. no UUID, like
// tmpfs or procfs; see device_id_test.cc), and name_to_handle_at(2) can
// fail EOPNOTSUPP/ENOTSUP on filesystems that don't export file handles.
bool IsUnsupported(const absl::Status &status) {
  if (absl::IsUnimplemented(status)) return true;
  absl::StatusOr<int> eno = GetErrnoFromStatus(status);
  return eno.ok() && (*eno == EOPNOTSUPP || *eno == ENOTSUP);
}

FileHandle MakeArbitraryHandle() {
  FileHandle fh;
  fh.device.uuid = {0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08,
                    0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f, 0x10};
  fh.device.subvol_id = 0x1122334455667788ULL;
  fh.handle_type = 7;
  fh.bytes = {0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09,
              0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f, 0x10, 0x11, 0x12, 0x13};
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

// Every length below the 28-byte minimum (24 of device, 4 of type) is
// refused, the error says how many bytes it got, and 28 exactly is a handle
// with no bytes.
TEST(FileHandleValueTest, ParseLengthBoundary) {
  for (size_t len :
       {size_t{0}, size_t{1}, size_t{23}, size_t{24}, size_t{27}}) {
    EXPECT_THAT(FileHandle::Parse(std::string(len, '\0')),
                StatusIs(absl::StatusCode::kInvalidArgument,
                         HasSubstr(absl::StrCat("got ", len, " bytes"))))
        << "length " << len;
  }
  absl::StatusOr<FileHandle> minimal = FileHandle::Parse(std::string(28, '\0'));
  ASSERT_THAT(minimal, IsOk());
  EXPECT_TRUE(minimal->bytes.empty());
  EXPECT_EQ(minimal->handle_type, 0);
}

// handle_type travels as four little-endian bytes of an int32: a negative
// type (the kernel's FILEID_* values are small positive ints, but this is a
// foreign value read back from a database) and the extremes survive.
TEST(FileHandleValueTest, HandleTypeSurvivesTheWireWhateverItsSign) {
  for (int32_t type : {int32_t{0}, int32_t{1}, int32_t{255}, int32_t{256},
                       int32_t{-1}, INT32_MIN, INT32_MAX}) {
    FileHandle fh = MakeArbitraryHandle();
    fh.handle_type = type;
    absl::StatusOr<FileHandle> parsed = FileHandle::Parse(fh.Serialize());
    ASSERT_THAT(parsed, IsOk());
    EXPECT_EQ(parsed->handle_type, type);
    EXPECT_EQ(*parsed, fh);
  }
}

TEST(FileHandleValueTest, TheTypeBytesAreLittleEndianAfterTheDevice) {
  FileHandle fh = MakeArbitraryHandle();
  fh.handle_type = 0x04030201;
  std::string wire = fh.Serialize();
  ASSERT_GE(wire.size(), 28u);
  EXPECT_EQ(wire.substr(24, 4), std::string("\x01\x02\x03\x04", 4));
}

// A longer handle than the kernel's MAX_HANDLE_SZ parses (Parse only splits
// the record); it is Open that the kernel refuses. Unlike a short one, it is
// not a decode error.
TEST(FileHandleValueTest, ParseAcceptsAnyHandleLength) {
  FileHandle fh = MakeArbitraryHandle();
  fh.bytes.assign(MAX_HANDLE_SZ + 1, 0x5a);
  absl::StatusOr<FileHandle> parsed = FileHandle::Parse(fh.Serialize());
  ASSERT_THAT(parsed, IsOk());
  EXPECT_EQ(parsed->bytes.size(), static_cast<size_t>(MAX_HANDLE_SZ + 1));
}

// ToString is for logs: it must carry each field a reader looks for, and is
// not compared whole (docs/style.md, tests).
TEST(FileHandleValueTest, ToStringHasEveryField) {
  FileHandle fh = MakeArbitraryHandle();
  std::string s = fh.ToString();
  EXPECT_THAT(s, HasSubstr("01020304-0506-0708-090a-0b0c0d0e0f10"))
      << "the filesystem uuid";
  EXPECT_THAT(s, HasSubstr(absl::StrCat("subvol=", fh.device.subvol_id)))
      << "the btrfs subvolume";
  EXPECT_THAT(s, ContainsRegex("/7:")) << "the handle type";
  EXPECT_THAT(s, HasSubstr("000102030405060708090a0b0c0d0e0f10111213"))
      << "the handle bytes, as hex";
}

TEST(FileHandleValueTest, ToStringOfAnEmptyHandleAndNoSubvolume) {
  FileHandle fh;
  fh.device.uuid.fill(0xAB);
  fh.handle_type = -3;
  std::string s = fh.ToString();
  EXPECT_THAT(s, HasSubstr("abababab-abab-abab-abab-abababababab"));
  EXPECT_THAT(s, Not(HasSubstr("subvol"))) << "no subvolume when it is 0";
  EXPECT_THAT(s, HasSubstr("/-3:")) << "the handle type, signed";
  EXPECT_THAT(s, EndsWith(":")) << "no handle bytes";
}

TEST(MountIdFromStatxTest, PrefersTheUniqueIdThenThePlainOneThenNothing) {
  struct statx stx = {};
  stx.stx_mnt_id = 42;

  stx.stx_mask = STATX_MNT_ID_UNIQUE | STATX_MNT_ID;
  EXPECT_THAT(MountIdFromStatx(stx), Optional(42u));
  stx.stx_mask = STATX_MNT_ID_UNIQUE;
  EXPECT_THAT(MountIdFromStatx(stx), Optional(42u));
  stx.stx_mask = STATX_MNT_ID;
  EXPECT_THAT(MountIdFromStatx(stx), Optional(42u));
  stx.stx_mask = STATX_TYPE | STATX_MODE;  // the kernel understood neither
  EXPECT_EQ(MountIdFromStatx(stx), std::nullopt);
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
    ASSERT_THAT(syscalls::mkdirat(AT_FDCWD, dir_path_, 0755), IsOk());

    ASSERT_OK_AND_ASSIGN(
        dir_, syscalls::openat(AT_FDCWD, dir_path_, O_PATH | O_DIRECTORY));
    dir_fd_ = *dir_;

    ASSERT_OK_AND_ASSIGN(file_, syscalls::openat(dir_fd_, "regular_file",
                                                 O_CREAT | O_RDWR, 0600));
    file_fd_ = *file_;

    ASSERT_THAT(syscalls::mkdirat(dir_fd_, "a_dir", 0755), IsOk());
    ASSERT_THAT(syscalls::symlinkat("regular_file", dir_fd_, "a_symlink"),
                IsOk());
  }

  FileDescriptor dir_;
  FileDescriptor file_;

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
  ASSERT_OK_AND_ASSIGN(
      FileDescriptor subdir,
      syscalls::openat(dir_fd_, "a_dir", O_PATH | O_DIRECTORY));
  const int subdir_fd = *subdir;

  absl::StatusOr<FileHandle> from_fd = FileHandle::FromFd(subdir_fd);
  if (!from_fd.ok()) {
    ASSERT_TRUE(IsUnsupported(from_fd.status())) << from_fd.status();
    GTEST_SKIP() << from_fd.status();
  }
  EXPECT_FALSE(from_fd->bytes.empty());

  absl::StatusOr<FileHandle> from_entry =
      FileHandle::FromDirEntry(dir_fd_, "a_dir");
  ASSERT_THAT(from_entry, IsOk());
  EXPECT_EQ(*from_fd, *from_entry);
}

// FileHandle::FromFd(int) cannot be used on a symlink fd at all (see its
// header comment), so this only exercises FromDirEntry -- which sidesteps
// that limitation by getting the device from the containing directory,
// since a symlink can never itself be a mount point.
TEST_F(FileHandleTest, FromDirEntryOnSymlink) {
  absl::StatusOr<FileHandle> from_entry =
      FileHandle::FromDirEntry(dir_fd_, "a_symlink");
  if (!from_entry.ok()) {
    ASSERT_TRUE(IsUnsupported(from_entry.status())) << from_entry.status();
    GTEST_SKIP() << from_entry.status();
  }
  EXPECT_FALSE(from_entry->bytes.empty());
}

TEST_F(FileHandleTest, FromDirEntryMissingNameFails) {
  absl::StatusOr<FileHandle> fh =
      FileHandle::FromDirEntry(dir_fd_, "does_not_exist");
  ASSERT_FALSE(fh.ok());
  EXPECT_THAT(GetErrnoFromStatus(fh.status()), IsOkAndHolds(ENOENT));
}

// Exercises FromDirEntry's mount-id-differs branch (opening the entry
// itself to call GetDeviceId on it): /proc is always its own filesystem,
// so "/proc"'s mount id must differ from "/"'s. This doesn't assert
// success -- GetDeviceId is expected to be Unimplemented for procfs (no
// FS_IOC_GETFSUUID support) regardless of which fd it's called on -- just
// that the mount boundary is detected and the code path runs without
// crashing, and that if GetDeviceId ever does succeed here, procfs's
// device really does differ from the root filesystem's.
TEST(FileHandleValueTest, FromDirEntryAcrossMountBoundaryDoesNotCrash) {
  ASSERT_OK_AND_ASSIGN(FileDescriptor root,
                       syscalls::openat(AT_FDCWD, "/", O_PATH | O_DIRECTORY));
  const int root_fd = *root;

  absl::StatusOr<FileHandle> fh = FileHandle::FromDirEntry(root_fd, "proc");
  if (!fh.ok()) {
    EXPECT_TRUE(absl::IsUnimplemented(fh.status())) << fh.status();
  } else {
    absl::StatusOr<DeviceId> root_device = GetDeviceId(root_fd);
    ASSERT_THAT(root_device, IsOk());
    EXPECT_NE(fh->device, *root_device);
  }
}

TEST_F(FileHandleTest, FromFdWithKnownDeviceSkipsGetDeviceId) {
  DeviceId device;
  device.uuid.fill(0x99);
  device.subvol_id = 42;

  absl::StatusOr<FileHandle> fh = FileHandle::FromFd(file_fd_, device);
  if (!fh.ok()) {
    ASSERT_TRUE(IsUnsupported(fh.status())) << fh.status();
    GTEST_SKIP() << fh.status();
  }
  EXPECT_EQ(fh->device, device);
  EXPECT_FALSE(fh->bytes.empty());
}

TEST_F(FileHandleTest, HardLinkYieldsEqualHandle) {
  absl::StatusOr<FileHandle> original = FileHandle::FromFd(file_fd_);
  if (!original.ok()) {
    ASSERT_TRUE(IsUnsupported(original.status())) << original.status();
    GTEST_SKIP() << original.status();
  }

  ASSERT_THAT(
      syscalls::linkat(dir_fd_, "regular_file", dir_fd_, "hard_link", 0),
      IsOk());
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

  ASSERT_OK_AND_ASSIGN(
      FileDescriptor other_file,
      syscalls::openat(dir_fd_, "other_file", O_CREAT | O_RDWR, 0600));
  absl::StatusOr<FileHandle> other = FileHandle::FromFd(*other_file);
  ASSERT_THAT(other, IsOk());
  EXPECT_NE(*original, *other);
}

TEST_F(FileHandleTest, OpenReopensFileViaMountFds) {
  absl::StatusOr<FileHandle> fh = FileHandle::FromFd(file_fd_);
  if (!fh.ok()) {
    ASSERT_TRUE(IsUnsupported(fh.status())) << fh.status();
    GTEST_SKIP() << fh.status();
  }

  // Open() requires a real (non-O_PATH) mount fd -- open_by_handle_at
  // rejects O_PATH (fs/fhandle.c get_path_from_fd() uses the non-raw fd
  // class).
  ASSERT_OK_AND_ASSIGN(
      FileDescriptor mount_fd,
      syscalls::openat(AT_FDCWD, dir_path_, O_RDONLY | O_DIRECTORY));
  MountFds mounts;
  ASSERT_THAT(mounts.Insert(fh->device, std::move(mount_fd)), IsOk());

  absl::StatusOr<FileDescriptor> opened = fh->Open(mounts, O_RDONLY);
  ASSERT_THAT(opened, IsOk());

  ASSERT_OK_AND_ASSIGN(struct stat original_st, syscalls::fstat(file_fd_));
  ASSERT_OK_AND_ASSIGN(struct stat opened_st, syscalls::fstat(**opened));
  EXPECT_EQ(original_st.st_ino, opened_st.st_ino);
}

TEST_F(FileHandleTest, OpenWithUnknownDeviceReturnsNotFound) {
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
  absl::StatusOr<FileHandle> fh = FileHandle::FromFd(file_fd_);
  if (!fh.ok()) {
    ASSERT_TRUE(IsUnsupported(fh.status())) << fh.status();
    GTEST_SKIP() << fh.status();
  }
  ASSERT_FALSE(fh->bytes.empty());
  fh->bytes[0] ^= 0xFF;

  ASSERT_OK_AND_ASSIGN(
      FileDescriptor mount_fd,
      syscalls::openat(AT_FDCWD, dir_path_, O_RDONLY | O_DIRECTORY));
  MountFds mounts;
  ASSERT_THAT(mounts.Insert(fh->device, std::move(mount_fd)), IsOk());

  // The corrupted handle should fail one way or another (typically ESTALE,
  // sometimes EBADF/EINVAL depending on how the bit flip lands); any
  // failure is acceptable here.
  EXPECT_FALSE(fh->Open(mounts, O_RDONLY).ok());
}

}  // namespace
}  // namespace dcfs
