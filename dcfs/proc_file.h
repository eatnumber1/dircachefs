// SPDX-License-Identifier: MIT
#ifndef DCFS_PROC_FILE_H_
#define DCFS_PROC_FILE_H_

#include <string>
#include <string_view>

#include "absl/status/statusor.h"

namespace dcfs {

// The whole text of the procfs file at `path` (/proc/self/mountinfo,
// /proc/<pid>/task/<tid>/status), read to its end. procfs makes each one
// as it is read; nothing on the backing filesystem is touched.
absl::StatusOr<std::string> ReadProcFile(std::string_view path);

// The text of /proc/self/mountinfo.
absl::StatusOr<std::string> ReadMountinfo();

}  // namespace dcfs

#endif  // DCFS_PROC_FILE_H_
