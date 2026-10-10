// Fault-injection tests for GetDeviceId (dcfs/device_id.cc): the ways its two
// ioctls (FS_IOC_GETFSUUID, and BTRFS_IOC_FS_INFO / BTRFS_IOC_GET_SUBVOL_INFO
// on btrfs) can fail or answer oddly, which no real filesystem of the test
// guest does on demand. A target of its own (BUILD.bazel) because the
// link-time wraps of ioctl and fstatfs64 (fstatfs under _FILE_OFFSET_BITS=64)
// rewrite every reference in the binary they are linked into; both fakes
// forward to the real call unless a test has armed Faults for it.

#include <fcntl.h>
#include <linux/btrfs.h>
#include <linux/fs.h>
#include <linux/magic.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/statfs.h>

#include <array>
#include <cerrno>
#include <cstdarg>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <map>
#include <optional>
#include <string>

#include "absl/algorithm/container.h"
#include "absl/status/status.h"
#include "absl/status/status_matchers.h"
#include "absl/status/statusor.h"
#include "dcfs/device_id.h"
#include "dcfs/fd.h"
#include "dcfs/status.h"
#include "dcfs/syscalls_backing.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"

namespace dcfs {
namespace {

using ::absl_testing::IsOk;
using ::absl_testing::StatusIs;
using ::testing::HasSubstr;

// What the fakes below do; reset after every test.
struct Faults {
  // fstatfs reports this f_type (for every descriptor) instead of the real one.
  std::optional<int64_t> fstatfs_type;
  // An ioctl request that fails with this errno.
  std::map<unsigned long, int> ioctl_errno;
  // FS_IOC_GETFSUUID succeeds, reporting this uuid length (the uuid is
  // kFakeUuid's bytes).
  std::optional<uint8_t> getfsuuid_len;
  // The btrfs ioctls succeed with kFakeUuid and this subvolume tree id.
  std::optional<uint64_t> btrfs_treeid;
};

Faults &GetFaults() {
  static Faults *faults = new Faults();
  return *faults;
}

constexpr std::array<uint8_t, 16> kFakeUuid = {
    0xa1, 0xa2, 0xa3, 0xa4, 0xa5, 0xa6, 0xa7, 0xa8,
    0xa9, 0xaa, 0xab, 0xac, 0xad, 0xae, 0xaf, 0xb0};

}  // namespace
}  // namespace dcfs

extern "C" {

int __real_ioctl(int fd, unsigned long request, ...);
int __real_fstatfs64(int fd, struct statfs *buf);

int __wrap_fstatfs64(int fd, struct statfs *buf) {
  int rc = __real_fstatfs64(fd, buf);
  const auto &type = dcfs::GetFaults().fstatfs_type;
  if (rc == 0 && type.has_value()) buf->f_type = *type;
  return rc;
}

int __wrap_ioctl(int fd, unsigned long request, ...) {
  va_list ap;
  va_start(ap, request);
  void *arg = va_arg(ap, void *);
  va_end(ap);

  dcfs::Faults &faults = dcfs::GetFaults();
  if (auto it = faults.ioctl_errno.find(request);
      it != faults.ioctl_errno.end()) {
    errno = it->second;
    return -1;
  }
  if (request == FS_IOC_GETFSUUID && faults.getfsuuid_len.has_value()) {
    auto *out = static_cast<struct fsuuid2 *>(arg);
    out->len = *faults.getfsuuid_len;
    absl::c_copy(dcfs::kFakeUuid, out->uuid);
    return 0;
  }
  if (request == BTRFS_IOC_FS_INFO && faults.btrfs_treeid.has_value()) {
    auto *out = static_cast<struct btrfs_ioctl_fs_info_args *>(arg);
    absl::c_copy(dcfs::kFakeUuid, out->fsid);
    return 0;
  }
  if (request == BTRFS_IOC_GET_SUBVOL_INFO && faults.btrfs_treeid.has_value()) {
    static_cast<struct btrfs_ioctl_get_subvol_info_args *>(arg)->treeid =
        *faults.btrfs_treeid;
    return 0;
  }
  return __real_ioctl(fd, request, arg);
}

}  // extern "C"

namespace dcfs {
namespace {

class DeviceIdFaultTest : public ::testing::Test {
 protected:
  void TearDown() override { GetFaults() = Faults(); }

  // The DeviceId GetDeviceId() computes for an O_PATH descriptor on `path`.
  static absl::StatusOr<DeviceId> IdOf(const char *path) {
    absl::StatusOr<FileDescriptor> fd =
        syscalls::openat(AT_FDCWD, path, O_PATH);
    if (!fd.ok()) return fd.status();
    return GetDeviceId(**fd);
  }
};

TEST_F(DeviceIdFaultTest, AnIoctlErrorOtherThanUnsupportedKeepsItsErrno) {
  GetFaults().ioctl_errno[FS_IOC_GETFSUUID] = EIO;
  absl::StatusOr<DeviceId> id = IdOf("/");
  ASSERT_FALSE(id.ok());
  EXPECT_EQ(StatusToErrno(id.status()), EIO);
  EXPECT_THAT(id.status().message(), HasSubstr("FS_IOC_GETFSUUID"));
}

TEST_F(DeviceIdFaultTest, EveryUnsupportedErrnoNamesTheFilesystem) {
  GetFaults().fstatfs_type = PROC_SUPER_MAGIC;
  for (int err : {ENOTTY, EOPNOTSUPP, ENOSYS}) {
    GetFaults().ioctl_errno[FS_IOC_GETFSUUID] = err;
    EXPECT_THAT(IdOf("/"), StatusIs(absl::StatusCode::kUnimplemented,
                                    AllOf(HasSubstr("proc"),
                                          HasSubstr("FS_IOC_GETFSUUID"))))
        << "errno " << err;
  }
}

TEST_F(DeviceIdFaultTest, AUuidOfTheWrongLengthIsRefused) {
  GetFaults().getfsuuid_len = 17;
  EXPECT_THAT(IdOf("/"),
              StatusIs(absl::StatusCode::kFailedPrecondition, HasSubstr("17")));
}

TEST_F(DeviceIdFaultTest, A16ByteUuidIsTakenAsIs) {
  GetFaults().getfsuuid_len = 16;
  absl::StatusOr<DeviceId> id = IdOf("/");
  ASSERT_THAT(id, IsOk());
  EXPECT_EQ(id->uuid, kFakeUuid);
  EXPECT_EQ(id->subvol_id, 0u);
}

TEST_F(DeviceIdFaultTest, BtrfsReportsItsFilesystemUuidAndSubvolume) {
  GetFaults().fstatfs_type = BTRFS_SUPER_MAGIC;
  GetFaults().btrfs_treeid = 257;
  absl::StatusOr<DeviceId> id = IdOf("/");
  ASSERT_THAT(id, IsOk());
  EXPECT_EQ(id->uuid, kFakeUuid);
  EXPECT_EQ(id->subvol_id, 257u);
}

TEST_F(DeviceIdFaultTest, BtrfsFsInfoFailureIsNamed) {
  GetFaults().fstatfs_type = BTRFS_SUPER_MAGIC;
  GetFaults().ioctl_errno[BTRFS_IOC_FS_INFO] = EPERM;
  absl::StatusOr<DeviceId> id = IdOf("/");
  ASSERT_FALSE(id.ok());
  EXPECT_EQ(StatusToErrno(id.status()), EPERM);
  EXPECT_THAT(id.status().message(), HasSubstr("BTRFS_IOC_FS_INFO"));
}

TEST_F(DeviceIdFaultTest, BtrfsSubvolumeInfoFailureIsNamed) {
  GetFaults().fstatfs_type = BTRFS_SUPER_MAGIC;
  GetFaults().btrfs_treeid = 5;  // FS_INFO answers; the next ioctl fails
  GetFaults().ioctl_errno[BTRFS_IOC_GET_SUBVOL_INFO] = EACCES;
  absl::StatusOr<DeviceId> id = IdOf("/");
  ASSERT_FALSE(id.ok());
  EXPECT_EQ(StatusToErrno(id.status()), EACCES);
  EXPECT_THAT(id.status().message(), HasSubstr("BTRFS_IOC_GET_SUBVOL_INFO"));
}

// ioctl refuses an O_PATH descriptor (EBADF); GetDeviceId reopens it through
// /proc/self/fd, which fails for a socket's inode (nothing can open it): that
// error, not the EBADF, is the answer. No fake: this is the kernel's.
TEST_F(DeviceIdFaultTest, AnObjectThatCannotBeReopenedReportsTheReopenError) {
  const char *tmpdir = std::getenv("TEST_TMPDIR");
  ASSERT_NE(tmpdir, nullptr);
  std::string sock = std::string(tmpdir) + "/sock";
  ASSERT_THAT(syscalls::mknodat(AT_FDCWD, sock, S_IFSOCK | 0600, 0), IsOk());
  absl::StatusOr<DeviceId> id = IdOf(sock.c_str());
  ASSERT_FALSE(id.ok());
  EXPECT_EQ(StatusToErrno(id.status()), ENXIO);
}

}  // namespace
}  // namespace dcfs
