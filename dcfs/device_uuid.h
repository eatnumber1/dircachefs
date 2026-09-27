#ifndef DCFS_DEVICE_UUID_H_
#define DCFS_DEVICE_UUID_H_

#include <string>

namespace dcfs {

struct DeviceUUID {
  std::string value;

  template <typename H>
  friend H AbslHashValue(H h, const DeviceUUID& uuid) {
    return H::combine(std::move(h), uuid.value);
  }

  friend bool operator==(const DeviceUUID &a, const DeviceUUID &b) {
    return a.value == b.value;
  }

  template <typename Sink>
  friend void AbslStringify(Sink& sink, const DeviceUUID& uuid) {
    absl::Format(&sink, "%s", uuid.value);
  }
};

}  // namespace dcfs

#endif  // DCFS_DEVICE_UUID_H_
