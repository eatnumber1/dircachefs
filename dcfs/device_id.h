#ifndef DCFS_DEVICE_ID_H_
#define DCFS_DEVICE_ID_H_

#include <array>
#include <cstdint>
#include <string>
#include <string_view>

#include "absl/status/statusor.h"

namespace dcfs {

// Identifies a filesystem instance: its on-disk UUID (as reported by
// FS_IOC_GETFSUUID on ext4/xfs, or BTRFS_IOC_FS_INFO's fsid field on
// btrfs, which does not support FS_IOC_GETFSUUID at all -- see
// GetDeviceId) plus, for filesystems with independently-snapshottable
// sub-volumes (currently only Btrfs), the sub-volume's tree id. Two O_PATH
// fds referring to the same mounted filesystem (and sub-volume, if any)
// compare equal.
struct DeviceId {
  std::array<uint8_t, 16> uuid{};
  uint64_t subvol_id = 0;

  friend bool operator==(const DeviceId &a, const DeviceId &b) {
    return a.uuid == b.uuid && a.subvol_id == b.subvol_id;
  }
  friend bool operator!=(const DeviceId &a, const DeviceId &b) {
    return !(a == b);
  }

  template <typename H>
  friend H AbslHashValue(H h, const DeviceId &id) {
    return H::combine(std::move(h), id.uuid, id.subvol_id);
  }

  // Serializes to a fixed 24-byte string: the 16 uuid bytes followed by
  // subvol_id encoded as little-endian uint64. Intended as a stable,
  // compact on-disk cache key -- not human-readable.
  std::string Serialize() const;

  // Parses the format produced by Serialize(). Returns InvalidArgument if
  // `data` is not exactly 24 bytes.
  static absl::StatusOr<DeviceId> Parse(std::string_view data);

  // Renders the uuid in canonical 8-4-4-4-12 hex, with "/subvol=<N>"
  // appended when subvol_id is nonzero. For log and error messages only;
  // not parseable by Parse().
  std::string ToString() const;
};

// Returns the DeviceId of the filesystem containing `fd`, which may be an
// O_PATH descriptor. On btrfs this always goes through BTRFS_IOC_FS_INFO/
// BTRFS_IOC_GET_SUBVOL_INFO instead of FS_IOC_GETFSUUID (see the .cc file
// for why). Otherwise, returns absl::UnimplementedError if the underlying
// filesystem does not support FS_IOC_GETFSUUID: that ioctl was only added
// in Linux 6.9, and as of this writing OpenZFS does not implement it at
// all.
absl::StatusOr<DeviceId> GetDeviceId(int fd);

// Returns a human-readable name for a statfs(2) f_type magic number, e.g.
// "ext4", falling back to "fstype 0x<hex>" for magics not in the table.
// For log and error messages only.
std::string FstypeName(int64_t f_type);

}  // namespace dcfs

#endif  // DCFS_DEVICE_ID_H_
