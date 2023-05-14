#ifndef DFS_FUSE_H_
#define DFS_FUSE_H_

#include <string>

#include "dfs/mount.h"
#include "dfs/fd.h"
#include "absl/status/statusor.h"
#include "absl/container/flat_hash_set.h"

namespace dfs {

class FuseMount {
 public:
   struct Options {
     std::string fsname;
     std::string mount_source;
     absl::flat_hash_set<std::string> mount_options;
   };

   static absl::StatusOr<FuseMount> Create(
       std::string mountpoint, Options options);

 private:
   FuseMount(Mount mount, FileDescriptor fuse_fd);

   absl::Status Handshake();

   Mount mount_;
   FileDescriptor fuse_fd_;
};

}  // namespace dfs

#endif  // DFS_FUSE_H_
