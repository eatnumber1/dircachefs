#include "dcfs/absolute_paths.h"

#include <fcntl.h>

#include <string>

#include "absl/status/status.h"
#include "absl/status/status_macros.h"
#include "absl/strings/str_cat.h"
#include "dcfs/mount_dcfs.h"
#include "dcfs/syscalls.h"
#include "dcfs/syscalls_backing.h"

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
  const bool is_path = options.backing != HelperOptions::Backing::kNative ||
                       (!args.source.empty() && args.source[0] != '/' &&
                        syscalls::fstatat(AT_FDCWD, args.source).ok());
  if (is_path) args.source = absolute(args.source);
  return absl::OkStatus();
}

}  // namespace dcfs
