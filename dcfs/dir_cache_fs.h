#ifndef DCFS_DIR_CACHE_FS_H_
#define DCFS_DIR_CACHE_FS_H_

#include <stddef.h>
#include <sys/types.h>

#include "absl/status/status.h"
#include "absl/time/time.h"
#include "dcfs/attributes.h"
#include "dcfs/file_handle.h"
#include "dcfs/fs_db.h"
#include "dcfs/fuse.h"
#include "fuse_lowlevel.h"

namespace dcfs {

class DirCacheFS {
 public:
  struct Options {
    absl::Duration kernel_inode_attribute_timeout = absl::ZeroDuration();
    absl::Duration kernel_directory_entry_timeout = absl::ZeroDuration();
  };

  using InodeID = ::dcfs::FileSystemDatabase::InodeID;

  DirCacheFS(
      FileHandle::Builder *absl_nonnull handle_builder,
      FileSystemDatabase *absl_nonnull fs_db,
      InodeID root_inode,
      Options opts);

  absl::Status Init(struct fuse_conn_info &conn);
  static_assert(FuseInitOp<DirCacheFS>);

  absl::Status Destroy();
  static_assert(FuseDestroyOp<DirCacheFS>);

  absl::Status Getattr(FuseRequest &req, fuse_ino_t ino, fuse_file_info *fi);
  static_assert(FuseGetattrOp<DirCacheFS>);

  absl::Status Opendir(FuseRequest &req, fuse_ino_t ino, fuse_file_info &fi);
  static_assert(FuseGetattrOp<DirCacheFS>);

  absl::Status Releasedir(FuseRequest &req, fuse_ino_t ino, fuse_file_info &fi);
  static_assert(FuseReleasedirOp<DirCacheFS>);

  absl::Status Readdir(
      FuseRequest &req, fuse_ino_t ino, size_t size, off_t off,
      fuse_file_info &fi);
  static_assert(FuseReaddirOp<DirCacheFS>);

#if 0
  absl::Status Readdirplus(
      FuseRequest &req, fuse_ino_t ino, size_t size, off_t off,
      fuse_file_info &fi);
  static_assert(FuseReaddirplusOp<DirCacheFS>);
#endif

  absl::Status Lookup(
      FuseRequest &req, fuse_ino_t parent_ino, std::string_view name);
  static_assert(FuseLookupOp<DirCacheFS>);

 private:
  absl::StatusOr<InodeID> FuseToInode(fuse_ino_t ino) const;
  fuse_ino_t InodeToFuse(InodeID inode) const;

  FileSystemDatabase &fs_db_;
  FileHandle::Builder &handle_builder_;
  InodeID root_inode_;
  const Options opts_;
};

}  // namespace dcfs

#endif  // DCFS_DIR_CACHE_FS_H_
