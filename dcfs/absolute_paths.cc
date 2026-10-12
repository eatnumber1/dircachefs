// SPDX-License-Identifier: MIT
#include "dcfs/absolute_paths.h"

#include <string>

#include "absl/status/status.h"
#include "absl/status/status_macros.h"
#include "absl/strings/str_cat.h"
#include "dcfs/mount_dcfs.h"
#include "dcfs/syscalls.h"

namespace dcfs {

absl::Status MakePathsAbsolute(HelperArgs &args, HelperOptions &options) {
  ASSIGN_OR_RETURN(std::string cwd, syscalls::realpath("."));
  auto absolute = [&cwd](const std::string &path) {
    if (path.empty() || path[0] == '/') return path;
    return cwd == "/" ? absl::StrCat("/", path) : absl::StrCat(cwd, "/", path);
  };
  args.mountpoint = absolute(args.mountpoint);
  if (options.cache_db.has_value()) {
    options.cache_db = absolute(*options.cache_db);
  }
  args.source = absolute(args.source);
  return absl::OkStatus();
}

}  // namespace dcfs
