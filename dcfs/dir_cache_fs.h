#ifndef DCFS_DIR_CACHE_FS_H_
#define DCFS_DIR_CACHE_FS_H_

#include <cstdint>
#include <string_view>
#include <sys/types.h>

#include "absl/status/status.h"
#include "dcfs/attributes.h"
#include "dcfs/fd.h"
#include "dcfs/fuse.h"
#include "fuse_lowlevel.h"

namespace dcfs {

// DirCacheFS is a minimal, mechanical placeholder low-level FUSE filesystem.
// It serves a single, empty root directory backed directly by `source_fd_`;
// no metadata cache or database is wired up yet (that lands in later steps --
// see README.md for the overall design). Anything not implemented here is
// answered ENOSYS by libfuse.
class DirCacheFS {
 public:
  // `source_fd` must be open O_PATH | O_DIRECTORY on the directory this
  // filesystem is caching.
  explicit DirCacheFS(FileDescriptor source_fd);

  absl::Status Init(struct fuse_conn_info &conn);
  absl::Status Destroy();

  // Only fuse_ino_t{FUSE_ROOT_ID} exists; anything else is ENOENT.
  absl::Status Getattr(FuseRequest &req, fuse_ino_t ino, fuse_file_info *fi);

  // There are no entries under the root yet, so every lookup is a negative
  // (ENOENT) reply.
  absl::Status Lookup(
      FuseRequest &req, fuse_ino_t parent_ino, std::string_view name);

  absl::Status Opendir(FuseRequest &req, fuse_ino_t ino, fuse_file_info &fi);
  absl::Status Readdir(
      FuseRequest &req, fuse_ino_t ino, size_t size, off_t off,
      fuse_file_info &fi);
  absl::Status Releasedir(FuseRequest &req, fuse_ino_t ino, fuse_file_info &fi);

  absl::Status Statfs(FuseRequest &req, fuse_ino_t ino);

  // Access checks are not implemented yet; everything is allowed.
  absl::Status Access(FuseRequest &req, fuse_ino_t ino, int mask);

  // No inode table exists yet, so there is nothing to do on forget.
  void Forget(FuseRequest &req, fuse_ino_t ino, uint64_t nlookup);

 private:
  FileDescriptor source_fd_;
};

// Builds the fuse_lowlevel_ops table dispatching to a DirCacheFS instance
// passed as the `userdata`/`fuse_req_userdata` pointer.
fuse_lowlevel_ops MakeDirCacheFsOps();

}  // namespace dcfs

#endif  // DCFS_DIR_CACHE_FS_H_
