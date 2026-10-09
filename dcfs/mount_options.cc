#include "dcfs/mount_options.h"

#include <span>
#include <string>
#include <string_view>

#include "absl/status/statusor.h"
#include "absl/strings/match.h"
#include "absl/strings/numbers.h"
#include "absl/strings/str_split.h"
#include "dcfs/status.h"

namespace dcfs {

absl::StatusOr<MountOptions> BuildMountOptions(
    bool allow_other, std::span<const std::string> fuse_opt) {
  MountOptions built;
  built.options.emplace_back(kDefaultPermissions);
  if (allow_other) built.options.emplace_back("allow_other");
  for (const std::string &opt : fuse_opt) {
    if (HasDefaultPermissions(std::span<const std::string>(&opt, 1))) {
      return InvalidArgumentErrorBuilder()
             << "dcfs.fuse_opt=" << opt
             << ": default_permissions is redundant, dcfs always mounts "
                "with it";
    }
    built.options.push_back(opt);
    // See DirCacheFS::Options::max_read: DirCacheFS::Init() needs this
    // value too, to satisfy libfuse's do_init() consistency check.
    if (unsigned int max_read;
        absl::StartsWith(opt, "max_read=") &&
        absl::SimpleAtoi(std::string_view(opt).substr(9), &max_read)) {
      built.max_read = max_read;
    }
  }
  return built;
}

bool HasDefaultPermissions(std::span<const std::string> options) {
  for (const std::string &element : options) {
    for (std::string_view opt : absl::StrSplit(element, ',')) {
      if (opt == kDefaultPermissions) return true;
    }
  }
  return false;
}

}  // namespace dcfs
