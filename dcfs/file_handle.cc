#include "dcfs/file_handle.h"

#include <fcntl.h>
#include <sys/stat.h>

#include <algorithm>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "absl/container/fixed_array.h"
#include "absl/status/status.h"
#include "absl/status/status_macros.h"
#include "absl/status/statusor.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/str_format.h"
#include "absl/types/optional.h"
#include "dcfs/device_id.h"
#include "dcfs/fd.h"
#include "dcfs/mount_fds.h"
#include "dcfs/status.h"
#include "dcfs/syscalls_backing.h"

namespace dcfs {
namespace {

// The handle_type and f_handle bytes portion of a kernel file handle, once
// unpacked out of a `struct file_handle`.
struct RawHandle {
  int handle_type = 0;
  std::vector<uint8_t> bytes;
};

// Calls name_to_handle_at(dirfd, pathname, flags), providing MAX_HANDLE_SZ
// of room for the returned handle (the maximum any Linux filesystem can
// currently produce) and unpacking the result. The `mount_id` output is
// discarded: name_to_handle_at's mount ids can be reused across mounts, so
// device identity is established via GetDeviceId instead, never via a
// mount id.
absl::StatusOr<RawHandle> NameToHandle(int dirfd, std::string_view pathname,
                                       int flags) {
  absl::FixedArray<uint8_t> buf(sizeof(struct file_handle) + MAX_HANDLE_SZ);
  auto *handle = reinterpret_cast<struct file_handle *>(buf.data());
  handle->handle_bytes = MAX_HANDLE_SZ;

  int mount_id = 0;
  RETURN_IF_ERROR(
      syscalls::name_to_handle_at(dirfd, pathname, *handle, mount_id, flags));

  RawHandle raw;
  raw.handle_type = handle->handle_type;
  raw.bytes.assign(handle->f_handle, handle->f_handle + handle->handle_bytes);
  return raw;
}

}  // namespace

// Both STATX_MNT_ID and STATX_MNT_ID_UNIQUE report the mount id in the same
// stx_mnt_id field; STATX_MNT_ID_UNIQUE additionally promises the kernel
// won't reuse the value later, which STATX_MNT_ID alone does not. Prefer
// the unique id when the kernel set that bit, otherwise fall back to the
// plain one; return nullopt if the kernel set neither (statx didn't
// understand either request bit at all).
std::optional<uint64_t> MountIdFromStatx(const struct statx &stx) {
  if ((stx.stx_mask & STATX_MNT_ID_UNIQUE) != 0) return stx.stx_mnt_id;
  if ((stx.stx_mask & STATX_MNT_ID) != 0) return stx.stx_mnt_id;
  return std::nullopt;
}

std::string FileHandle::Serialize() const {
  std::string out = device.Serialize();
  out.reserve(out.size() + 4 + bytes.size());
  uint32_t type = static_cast<uint32_t>(handle_type);
  for (int i = 0; i < 4; ++i) {
    out.push_back(static_cast<char>((type >> (8 * i)) & 0xFF));
  }
  out.append(reinterpret_cast<const char *>(bytes.data()), bytes.size());
  return out;
}

absl::StatusOr<FileHandle> FileHandle::Parse(std::string_view data) {
  if (data.size() < 28) {
    return InvalidArgumentErrorBuilder()
           << "FileHandle::Parse: expected at least 28 bytes, got "
           << data.size() << " bytes";
  }

  ASSIGN_OR_RETURN(DeviceId device, DeviceId::Parse(data.substr(0, 24)));

  uint32_t type = 0;
  for (int i = 0; i < 4; ++i) {
    type |= static_cast<uint32_t>(static_cast<uint8_t>(data[24 + i]))
            << (8 * i);
  }

  FileHandle fh;
  fh.device = std::move(device);
  fh.handle_type = static_cast<int32_t>(type);
  std::string_view rest = data.substr(28);
  fh.bytes.assign(rest.begin(), rest.end());
  return fh;
}

std::string FileHandle::ToString() const {
  std::string hex;
  hex.reserve(bytes.size() * 2);
  for (uint8_t b : bytes) {
    absl::StrAppendFormat(&hex, "%02x", b);
  }
  return absl::StrCat(device.ToString(), "/", handle_type, ":", hex);
}

absl::StatusOr<FileHandle> FileHandle::FromFd(int fd, DeviceId device) {
  ASSIGN_OR_RETURN(RawHandle raw, NameToHandle(fd, "", AT_EMPTY_PATH));

  FileHandle fh;
  fh.device = std::move(device);
  fh.handle_type = raw.handle_type;
  fh.bytes = std::move(raw.bytes);
  return fh;
}

absl::StatusOr<FileHandle> FileHandle::FromFd(int fd) {
  ASSIGN_OR_RETURN(DeviceId device, GetDeviceId(fd));
  return FromFd(fd, std::move(device));
}

absl::StatusOr<FileHandle> FileHandle::FromDirEntry(int dirfd,
                                                     std::string_view name) {
  ASSIGN_OR_RETURN(RawHandle raw, NameToHandle(dirfd, name, 0));

  // Only a directory can be a mount point (or, for Btrfs, a sub-volume
  // boundary), so if `name` isn't one, it necessarily shares dirfd's
  // filesystem. Check cheaply via statx's mount id before ever opening
  // `name` itself.
  unsigned int want_mnt_id = STATX_MNT_ID_UNIQUE | STATX_MNT_ID;
  ASSIGN_OR_RETURN(
      struct statx entry_stx,
      syscalls::statx(dirfd, name, AT_SYMLINK_NOFOLLOW | AT_STATX_DONT_SYNC,
                      STATX_TYPE | want_mnt_id));
  ASSIGN_OR_RETURN(struct statx dir_stx,
                   syscalls::statx(dirfd, "", AT_EMPTY_PATH, want_mnt_id));

  std::optional<uint64_t> entry_mnt_id = MountIdFromStatx(entry_stx);
  std::optional<uint64_t> dir_mnt_id = MountIdFromStatx(dir_stx);

  DeviceId device;
  if (entry_mnt_id.has_value() && dir_mnt_id.has_value() &&
      *entry_mnt_id != *dir_mnt_id) {
    // A mount boundary -- `name` must be a directory to be one, so this is
    // safe to require via O_DIRECTORY. O_NOFOLLOW keeps a symlink named
    // `name` (which could never reach this branch, but just in case) from
    // being followed.
    ASSIGN_OR_RETURN(
        FileDescriptor entry_fd,
        syscalls::openat(dirfd, name, O_PATH | O_DIRECTORY | O_NOFOLLOW));
    ASSIGN_OR_RETURN(device, GetDeviceId(*entry_fd));
  } else {
    // Same filesystem as dirfd (or the kernel didn't report a mount id at
    // all, in which case assuming "same filesystem" is the best available
    // answer).
    ASSIGN_OR_RETURN(device, GetDeviceId(dirfd));
  }

  FileHandle fh;
  fh.device = std::move(device);
  fh.handle_type = raw.handle_type;
  fh.bytes = std::move(raw.bytes);
  return fh;
}

absl::StatusOr<FileDescriptor> FileHandle::Open(const MountFds &mounts,
                                                 int flags) const {
  ASSIGN_OR_RETURN(int mount_fd, mounts.Get(device));

  absl::FixedArray<uint8_t> buf(sizeof(struct file_handle) + bytes.size());
  auto *handle = reinterpret_cast<struct file_handle *>(buf.data());
  handle->handle_bytes = static_cast<unsigned int>(bytes.size());
  handle->handle_type = handle_type;
  std::copy(bytes.begin(), bytes.end(), handle->f_handle);

  return syscalls::open_by_handle_at(mount_fd, *handle, flags);
}

}  // namespace dcfs
