// SPDX-License-Identifier: MIT
#ifndef DCFS_FUSE_OPS_H_
#define DCFS_FUSE_OPS_H_

#include <string>
#include <string_view>

#include "dcfs/fuse_request.h"
#include "dcfs/protocol_events.h"

namespace dcfs {

// Builds the fuse_lowlevel_ops table dispatching every op to a DirCacheFS
// instance passed as the `userdata`/`fuse_req_userdata` pointer.
fuse_lowlevel_ops MakeFuseOps();

std::string_view OpName(events::Op op);
std::string DescribeRequest(const events::Request &request);

}  // namespace dcfs

#endif  // DCFS_FUSE_OPS_H_
