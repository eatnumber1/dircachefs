#include "dcfs/device_id.h"

#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/vfs.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>

// Pulls in the real FS_IOC_GETFSUUID definition when the host's UAPI
// headers are new enough (Linux 6.9+).
#include <linux/btrfs.h>
#include <linux/fs.h>
#include <linux/magic.h>
#include <linux/types.h>

#include "absl/status/status.h"
#include "absl/status/status_macros.h"
#include "absl/status/statusor.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/str_format.h"
#include "dcfs/status.h"

// This host's kernel headers (6.8) predate FS_IOC_GETFSUUID (added in
// 6.9). The definitions below are copied verbatim from the upstream UAPI
// header so callers get correct behavior on newer kernels without a
// rebuild, and correct behavior here without one.
#ifndef FS_IOC_GETFSUUID
struct fsuuid2 {
  __u8 len;
  __u8 uuid[16];
};
#define FS_IOC_GETFSUUID _IOR(0x15, 0, struct fsuuid2)
#endif  // FS_IOC_GETFSUUID

namespace dcfs {

std::string DeviceId::Serialize() const {
  std::string out;
  out.reserve(24);
  out.append(reinterpret_cast<const char *>(uuid.data()), uuid.size());
  for (int i = 0; i < 8; ++i) {
    out.push_back(static_cast<char>((subvol_id >> (8 * i)) & 0xFF));
  }
  return out;
}

absl::StatusOr<DeviceId> DeviceId::Parse(std::string_view data) {
  if (data.size() != 24) {
    return InvalidArgumentErrorBuilder()
           << "DeviceId::Parse: expected a 24-byte value, got " << data.size()
           << " bytes";
  }

  DeviceId id;
  std::copy(data.begin(), data.begin() + 16, id.uuid.begin());

  uint64_t subvol_id = 0;
  for (int i = 0; i < 8; ++i) {
    subvol_id |=
        static_cast<uint64_t>(static_cast<uint8_t>(data[16 + i])) << (8 * i);
  }
  id.subvol_id = subvol_id;
  return id;
}

std::string DeviceId::ToString() const {
  std::string s = absl::StrFormat(
      "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
      uuid[0], uuid[1], uuid[2], uuid[3], uuid[4], uuid[5], uuid[6], uuid[7],
      uuid[8], uuid[9], uuid[10], uuid[11], uuid[12], uuid[13], uuid[14],
      uuid[15]);
  if (subvol_id != 0) {
    absl::StrAppend(&s, "/subvol=", subvol_id);
  }
  return s;
}

std::string FstypeName(int64_t f_type) {
  switch (f_type) {
    case 0xEF53:
      return "ext2/3/4";
    case 0x58465342:
      return "xfs";
    case 0x9123683E:
      return "btrfs";
    case 0x01021994:
      return "tmpfs";
    case 0x2FC12FC1:
      return "zfs";
    case 0x6969:
      return "nfs";
    case 0x65735546:
      return "fuse";
    case 0x794C7630:
      return "overlayfs";
    case 0x9FA0:
      return "proc";
    case 0x62656572:
      return "sysfs";
    case 0x858458F6:
      return "ramfs";
    case 0x4D44:
      return "vfat";
    case 0x2011BAB0:
      return "exfat";
    case 0xF2F52010:
      return "f2fs";
    case 0xCA451A4E:
      return "bcachefs";
    default:
      return absl::StrFormat("fstype 0x%x", static_cast<uint64_t>(f_type));
  }
}

namespace {

// Returns the statfs(2) f_type of the filesystem containing `fd`.
absl::StatusOr<int64_t> GetFsType(int fd) {
  struct statfs sf;
  if (fstatfs(fd, &sf) != 0) {
    return ErrnoToStatus(errno, "fstatfs");
  }
  return static_cast<int64_t>(sf.f_type);
}

// ioctl(2) unconditionally refuses O_PATH descriptors with EBADF, even for
// ioctls such as FS_IOC_GETFSUUID that only need `fd` to identify a
// filesystem and would otherwise be safe to allow. Route around this by
// reopening the same file through /proc/self/fd/<fd>, a standard technique
// for performing fd-level operations on an O_PATH descriptor. This adds an
// extra open()+close() only on the O_PATH path; a directly-usable fd is
// unaffected.
int IoctlAllowingOPath(int fd, unsigned long request, void *arg) {
  if (ioctl(fd, request, arg) == 0) return 0;
  if (errno != EBADF) return -1;

  std::string proc_path = absl::StrCat("/proc/self/fd/", fd);
  int reopened = open(proc_path.c_str(),
                      O_RDONLY | O_CLOEXEC | O_NONBLOCK | O_NOCTTY);
  if (reopened < 0) return -1;

  int rc = ioctl(reopened, request, arg);
  int saved_errno = errno;
  close(reopened);
  errno = saved_errno;
  return rc;
}

}  // namespace

absl::StatusOr<DeviceId> GetDeviceId(int fd) {
  int64_t f_type;
  ABSL_ASSIGN_OR_RETURN(f_type, GetFsType(fd));

  // Step 5.2 finding: btrfs does not support FS_IOC_GETFSUUID at all, on
  // any kernel version -- unlike ext4 and xfs (fs/ext4/super.c,
  // fs/xfs/xfs_mount.c), no file under fs/btrfs/ calls super_set_uuid(),
  // so the kernel's generic handler (ioctl_getfsuuid() in fs/ioctl.c)
  // always returns ENOTTY for it: sb->s_uuid_len is simply never set.
  // BTRFS_IOC_FS_INFO's fsid field is the same stable filesystem UUID
  // (what `btrfs filesystem show`/blkid report), reached through a
  // btrfs-specific ioctl instead of the generic one, so it stands in for
  // FS_IOC_GETFSUUID here rather than this filesystem going unsupported.
  DeviceId id;
  if (f_type == BTRFS_SUPER_MAGIC) {
    struct btrfs_ioctl_fs_info_args fs_info;
    std::memset(&fs_info, 0, sizeof(fs_info));
    if (IoctlAllowingOPath(fd, BTRFS_IOC_FS_INFO, &fs_info) != 0) {
      return ErrnoToStatus(errno, "BTRFS_IOC_FS_INFO ioctl");
    }
    static_assert(sizeof(fs_info.fsid) == 16);
    std::copy(std::begin(fs_info.fsid), std::end(fs_info.fsid),
              id.uuid.begin());

    struct btrfs_ioctl_get_subvol_info_args args;
    std::memset(&args, 0, sizeof(args));
    if (IoctlAllowingOPath(fd, BTRFS_IOC_GET_SUBVOL_INFO, &args) != 0) {
      return ErrnoToStatus(errno, "BTRFS_IOC_GET_SUBVOL_INFO ioctl");
    }
    id.subvol_id = args.treeid;
    return id;
  }

  struct fsuuid2 fsuuid;
  std::memset(&fsuuid, 0, sizeof(fsuuid));

  if (IoctlAllowingOPath(fd, FS_IOC_GETFSUUID, &fsuuid) != 0) {
    int saved_errno = errno;
    if (saved_errno == ENOTTY || saved_errno == EOPNOTSUPP ||
        saved_errno == ENOSYS) {
      return UnimplementedErrorBuilder()
             << FstypeName(f_type) << " does not support FS_IOC_GETFSUUID";
    }
    return ErrnoToStatus(saved_errno, "FS_IOC_GETFSUUID ioctl");
  }

  if (fsuuid.len != 16) {
    return FailedPreconditionErrorBuilder()
           << "FS_IOC_GETFSUUID returned an unexpected uuid length: "
           << static_cast<int>(fsuuid.len);
  }

  std::copy(std::begin(fsuuid.uuid), std::end(fsuuid.uuid), id.uuid.begin());
  return id;
}

}  // namespace dcfs
