#ifndef DCFS_FILE_HANDLE_H_
#define DCFS_FILE_HANDLE_H_

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "absl/status/statusor.h"
#include "dcfs/device_id.h"
#include "dcfs/fd.h"
#include "dcfs/mount_fds.h"

namespace dcfs {

// A FileHandle is a durable, path-independent reference to a single inode
// (file, directory, symlink, ...) on some filesystem: a kernel file handle
// (struct file_handle, as produced by name_to_handle_at(2)) plus the
// DeviceId of the filesystem that issued it.
//
// dcfs never keeps paths around after startup, so a FileHandle -- together
// with a live O_PATH fd on its filesystem, looked up by DeviceId in a
// MountFds -- is the only durable reference the daemon has to a backing
// object; Open() reopens it via open_by_handle_at(2).
struct FileHandle {
  DeviceId device;
  int handle_type = 0;          // struct file_handle::handle_type
  std::vector<uint8_t> bytes;   // struct file_handle::f_handle, handle_bytes long

  friend bool operator==(const FileHandle &, const FileHandle &) = default;

  template <typename H>
  friend H AbslHashValue(H h, const FileHandle &fh) {
    return H::combine(std::move(h), fh.device, fh.handle_type, fh.bytes);
  }

  // Wire format: device.Serialize() (24 bytes) followed by handle_type as a
  // 4-byte little-endian int32, followed by bytes (the remainder). Intended
  // as a stable, compact on-disk cache key -- not human-readable.
  std::string Serialize() const;

  // Parses the format produced by Serialize(). Returns InvalidArgument if
  // `data` is shorter than 28 bytes (24-byte DeviceId + 4-byte handle_type).
  static absl::StatusOr<FileHandle> Parse(std::string_view data);

  // Renders as "<device>/<handle_type>:<hex bytes>". For log and error
  // messages only; not parseable by Parse().
  std::string ToString() const;

  // Builds a FileHandle for the object `fd` itself names, using `device` as
  // its already-known device identity instead of calling GetDeviceId(fd).
  // For callers that already know it -- e.g. the root fd, whose device is
  // established once at startup.
  static absl::StatusOr<FileHandle> FromFd(int fd, DeviceId device);

  // Builds a FileHandle for the object `fd` itself names, calling
  // GetDeviceId(fd) to establish its device. `fd` may be an O_PATH
  // descriptor (name_to_handle_at with AT_EMPTY_PATH operates on whatever
  // `fd` refers to, without following it further) -- except a symlink:
  // GetDeviceId()'s ioctl-based implementation cannot handle an O_PATH fd
  // for a symlink. ioctl() rejects O_PATH descriptors outright with EBADF,
  // so GetDeviceId falls back to reopening via /proc/self/fd/<fd>; when
  // <fd> is itself an O_PATH descriptor for a symlink, that reopen fails
  // ELOOP (the kernel refuses to transparently dereference a magic symlink
  // that itself names a symlink opened O_NOFOLLOW). Use FromDirEntry() for
  // a symlink instead -- it gets the device from the containing directory,
  // which never has this problem.
  static absl::StatusOr<FileHandle> FromFd(int fd);

  // Builds a FileHandle for the directory entry `name` inside `dirfd`. If
  // `name` names a symlink, the handle refers to the symlink itself, not
  // its target (name_to_handle_at is called without AT_SYMLINK_FOLLOW).
  //
  // Device identity is established without needing an fd on `name` itself
  // unless it turns out to be a mount point: only a directory can be a
  // mount point or (for Btrfs) a sub-volume boundary, so any non-directory
  // entry necessarily lives on the same filesystem as `dirfd`. This checks
  // that cheaply by comparing `dirfd`'s and the entry's statx mount ids,
  // and only opens the entry itself (O_PATH|O_DIRECTORY|O_NOFOLLOW, so it
  // must be a directory) to call GetDeviceId on it when the mount ids
  // actually differ.
  static absl::StatusOr<FileHandle> FromDirEntry(int dirfd,
                                                  std::string_view name);

  // Reopens the object this handle refers to: looks up an O_PATH fd for
  // `device` in `mounts` and calls open_by_handle_at(2) with `flags`.
  // Returns NotFound (propagated from MountFds::Get) if `device` is not
  // registered in `mounts`. An ESTALE status from the kernel -- meaning the
  // object is gone or the handle has expired -- is passed through
  // unchanged.
  absl::StatusOr<FileDescriptor> Open(const MountFds &mounts, int flags) const;
};

}  // namespace dcfs

#endif  // DCFS_FILE_HANDLE_H_
