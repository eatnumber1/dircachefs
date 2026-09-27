#ifndef DCFS_FUSE_OPS_H_
#define DCFS_FUSE_OPS_H_

#include "dcfs/fuse_request.h"

namespace dcfs {

// Builds the fuse_lowlevel_ops table dispatching every op to a DirCacheFS
// instance passed as the `userdata`/`fuse_req_userdata` pointer.
fuse_lowlevel_ops MakeFuseOps();

}  // namespace dcfs

#endif  // DCFS_FUSE_OPS_H_
