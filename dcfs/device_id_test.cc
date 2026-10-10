#include "dcfs/device_id.h"

#include <fcntl.h>

#include <array>
#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>

#include "absl/hash/hash.h"
#include "absl/status/status.h"
#include "absl/status/status_macros.h"
#include "absl/status/status_matchers.h"
#include "absl/status/statusor.h"
#include "dcfs/fd.h"
#include "dcfs/syscalls_backing.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"

namespace dcfs {
namespace {

using ::absl_testing::IsOk;
using ::absl_testing::StatusIs;
using ::testing::HasSubstr;

constexpr std::array<uint8_t, 16> kZeroUuid{};

TEST(DeviceIdTest, SerializeParseRoundTrip) {
  DeviceId id;
  id.uuid = {0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08,
             0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f, 0x10};
  id.subvol_id = 0x1122334455667788ULL;

  std::string serialized = id.Serialize();
  ASSERT_EQ(serialized.size(), 24u);

  absl::StatusOr<DeviceId> parsed = DeviceId::Parse(serialized);
  ASSERT_THAT(parsed, IsOk());
  EXPECT_EQ(*parsed, id);
}

TEST(DeviceIdTest, SerializeParseRoundTripZeroSubvol) {
  DeviceId id;
  id.uuid = {0xff, 0xee, 0xdd, 0xcc, 0xbb, 0xaa, 0x99, 0x88,
             0x77, 0x66, 0x55, 0x44, 0x33, 0x22, 0x11, 0x00};
  id.subvol_id = 0;

  absl::StatusOr<DeviceId> parsed = DeviceId::Parse(id.Serialize());
  ASSERT_THAT(parsed, IsOk());
  EXPECT_EQ(*parsed, id);
}

TEST(DeviceIdTest, ParseRejectsWrongLength) {
  EXPECT_THAT(DeviceId::Parse(""),
              StatusIs(absl::StatusCode::kInvalidArgument));
  EXPECT_THAT(DeviceId::Parse("short"),
              StatusIs(absl::StatusCode::kInvalidArgument));
  EXPECT_THAT(DeviceId::Parse(std::string(25, 'x')),
              StatusIs(absl::StatusCode::kInvalidArgument));
}

TEST(DeviceIdTest, EqualityAndHashing) {
  DeviceId a;
  a.uuid.fill(0x42);
  a.subvol_id = 7;

  DeviceId b = a;
  EXPECT_EQ(a, b);
  EXPECT_EQ(absl::HashOf(a), absl::HashOf(b));

  DeviceId different_subvol = a;
  different_subvol.subvol_id = 8;
  EXPECT_NE(a, different_subvol);

  DeviceId different_uuid = a;
  different_uuid.uuid[0] ^= 0xFF;
  EXPECT_NE(a, different_uuid);
}

TEST(DeviceIdTest, FstypeNameKnown) {
  EXPECT_EQ(FstypeName(0xEF53), "ext2/3/4");
  EXPECT_EQ(FstypeName(0x58465342), "xfs");
  EXPECT_EQ(FstypeName(0x9123683E), "btrfs");
  EXPECT_EQ(FstypeName(0x01021994), "tmpfs");
  EXPECT_EQ(FstypeName(0x2FC12FC1), "zfs");
  EXPECT_EQ(FstypeName(0x6969), "nfs");
  EXPECT_EQ(FstypeName(0x65735546), "fuse");
  EXPECT_EQ(FstypeName(0x794C7630), "overlayfs");
  EXPECT_EQ(FstypeName(0x9FA0), "proc");
  EXPECT_EQ(FstypeName(0x62656572), "sysfs");
  EXPECT_EQ(FstypeName(0x858458F6), "ramfs");
  EXPECT_EQ(FstypeName(0x4D44), "vfat");
  EXPECT_EQ(FstypeName(0x2011BAB0), "exfat");
  EXPECT_EQ(FstypeName(0xF2F52010), "f2fs");
  EXPECT_EQ(FstypeName(0xCA451A4E), "bcachefs");
}

TEST(DeviceIdTest, FstypeNameUnknown) {
  EXPECT_THAT(FstypeName(0x12345678), HasSubstr("0x12345678"));
}

// Opens `path` O_PATH and returns the DeviceId GetDeviceId() computes for
// it, or the failing status (including from open() itself).
absl::StatusOr<DeviceId> GetDeviceIdForPath(const char *path) {
  ASSIGN_OR_RETURN(FileDescriptor fd, syscalls::openat(AT_FDCWD, path, O_PATH));
  return GetDeviceId(*fd);
}

TEST(DeviceIdTest, GetDeviceIdRoot) {
  absl::StatusOr<DeviceId> id = GetDeviceIdForPath("/");
  if (!id.ok() && absl::IsUnimplemented(id.status())) {
    // Genuine filesystem-feature absence, not a bug: "/" in the test guest
    // is the initramfs rootfs, which (like tmpfs) has no UUID and so does
    // not support FS_IOC_GETFSUUID.
    GTEST_SKIP() << id.status();
  }
  ASSERT_THAT(id, IsOk());
  EXPECT_NE(id->uuid, kZeroUuid);

  // A second O_PATH fd on the same directory must yield an equal id.
  absl::StatusOr<DeviceId> id2 = GetDeviceIdForPath("/");
  ASSERT_THAT(id2, IsOk());
  EXPECT_EQ(*id, *id2);
}

TEST(DeviceIdTest, GetDeviceIdTestTmpDir) {
  const char *tmpdir = std::getenv("TEST_TMPDIR");
  ASSERT_NE(tmpdir, nullptr)
      << "TEST_TMPDIR must be set when running under bazel test";

  // TEST_TMPDIR is backed by a real ext4 disk (see the qemu_cc_test disks=
  // attribute on this target), which always supports FS_IOC_GETFSUUID on
  // the project's kernel; no skip needed.
  absl::StatusOr<DeviceId> id = GetDeviceIdForPath(tmpdir);
  ASSERT_THAT(id, IsOk());
  EXPECT_NE(id->uuid, kZeroUuid);
}

// Step 5.2: unlike ext4 and xfs (both call super_set_uuid(), verified in
// fs/ext4/super.c and fs/xfs/xfs_mount.c as of this writing), btrfs does
// not support FS_IOC_GETFSUUID at all -- no fs/btrfs/*.c file calls
// super_set_uuid(), so the kernel's generic ioctl_getfsuuid() (fs/ioctl.c)
// always returns ENOTTY for it, on every kernel version, not just this
// project's. GetDeviceId() falls back to BTRFS_IOC_FS_INFO's fsid field in
// that case (see device_id.cc) -- this is a dcfs fix (not a test-only
// workaround), since BTRFS_IOC_FS_INFO's fsid is the same stable
// filesystem UUID `btrfs filesystem show`/blkid report, just reached via a
// btrfs-specific ioctl instead of the generic one. Mounts vdc (this
// target's second disk, pre-formatted btrfs by run-qemu.sh) itself, since
// guest/init's disk0 handling only ever turns the *first* disks= entry
// into TEST_TMPDIR.
TEST(DeviceIdTest, GetDeviceIdBtrfsUsesFsInfoFallback) {
  const char *tmpdir = std::getenv("TEST_TMPDIR");
  ASSERT_NE(tmpdir, nullptr)
      << "TEST_TMPDIR must be set when running under bazel test";
  std::string mnt = std::string(tmpdir) + "/btrfs_mnt";
  ASSERT_THAT(syscalls::mkdirat(AT_FDCWD, mnt, 0755), IsOk());
  ASSERT_THAT(syscalls::mount("/dev/vdc", mnt, "btrfs", 0, nullptr), IsOk());

  absl::StatusOr<DeviceId> id = GetDeviceIdForPath(mnt.c_str());
  ASSERT_THAT(id, IsOk()) << id.status();
  EXPECT_NE(id->uuid, kZeroUuid);
  // The root subvolume's own tree id (BTRFS_FS_TREE_OBJECTID) is always 5,
  // on every btrfs filesystem.
  EXPECT_EQ(id->subvol_id, 5u);

  // A second O_PATH fd on the same mount must yield an equal id (same
  // consistency property GetDeviceIdRoot checks for a non-btrfs fs).
  absl::StatusOr<DeviceId> id2 = GetDeviceIdForPath(mnt.c_str());
  ASSERT_THAT(id2, IsOk());
  EXPECT_EQ(*id, *id2);

  EXPECT_THAT(syscalls::umount2(mnt, 0), IsOk());
}

// Asserts the *current* pre-FS_IOC_GETFSUUID-support OpenZFS behavior.
// Flip this to a success assertion once OpenZFS ships FS_IOC_GETFSUUID.
TEST(DeviceIdTest, GetDeviceIdZfsIsUnimplemented) {
  const char *zfs_path = std::getenv("DCFS_TEST_ZFS_PATH");
  if (zfs_path == nullptr) {
    GTEST_SKIP() << "DCFS_TEST_ZFS_PATH not set";
  }

  absl::StatusOr<DeviceId> id = GetDeviceIdForPath(zfs_path);
  EXPECT_THAT(id, StatusIs(absl::StatusCode::kUnimplemented, HasSubstr("zfs")));
}

}  // namespace
}  // namespace dcfs
