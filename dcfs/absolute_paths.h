// SPDX-License-Identifier: MIT
#ifndef DCFS_ABSOLUTE_PATHS_H_
#define DCFS_ABSOLUTE_PATHS_H_

// The daemon changes its working directory to "/" when it detaches, so the
// paths the wrapper was given are made absolute before the fork.

#include "absl/status/status.h"
#include "dcfs/mount_dcfs.h"

namespace dcfs {

// Makes MOUNTPOINT, SOURCE, and dcfs.cache_db absolute.
// `args.spec`, SOURCE as written (decision 11), is untouched.
absl::Status MakePathsAbsolute(HelperArgs &args, HelperOptions &options);

}  // namespace dcfs

#endif  // DCFS_ABSOLUTE_PATHS_H_
