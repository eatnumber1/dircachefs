#ifndef DCFS_ABSOLUTE_PATHS_H_
#define DCFS_ABSOLUTE_PATHS_H_

// The daemon changes its working directory to "/" when it detaches, so the
// paths the wrapper was given are made absolute before the fork.

#include "absl/status/status.h"
#include "dcfs/mount_dcfs.h"

namespace dcfs {

// Makes MOUNTPOINT and dcfs.cache_db absolute, and SOURCE where it is a path:
// always for `none` and `bind`; for a native mount only if it names
// something that exists relative to the working directory (a ZFS dataset
// `pool/fs`, a virtiofs or 9p tag, `tmpfs`, UUID=..., host:/export are not
// paths of it). `args.spec`, SOURCE as written (decision 11), is untouched.
[[nodiscard]] absl::Status MakePathsAbsolute(HelperArgs &args,
                                             HelperOptions &options);

}  // namespace dcfs

#endif  // DCFS_ABSOLUTE_PATHS_H_
