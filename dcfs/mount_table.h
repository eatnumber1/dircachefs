#ifndef DCFS_MOUNT_TABLE_H_
#define DCFS_MOUNT_TABLE_H_

#include <memory>

#include "absl/status/statusor.h"
#include "libmount.h"

namespace dcfs {

// The parse /proc/self/mountinfo. If the mount table is a mountinfo file then
// /run/mount/utabs is parsed too and both files are merged to the one
// libmnt_table.
//
// If libmount is compiled with classic mtab file support, and the /etc/mtab
// is a regular file then this file is parsed.
absl::StatusOr<std::unique_ptr<libmnt_table, decltype(&mnt_unref_table)>>
  CreateMountTableFromMtab();

}  // namespace dcfs

#endif  // DCFS_MOUNT_TABLE_H_
