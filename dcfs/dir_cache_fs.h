#ifndef DCFS_DIR_CACHE_FS_H_
#define DCFS_DIR_CACHE_FS_H_

#include <cstdint>
#include <span>
#include <string_view>
#include <sys/types.h>

#include "absl/container/flat_hash_map.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/time/time.h"
#include "dcfs/context.h"
#include "dcfs/fd.h"
#include "dcfs/fuse_request.h"
#include "dcfs/metadata_cache.h"
#include "fuse_lowlevel.h"

namespace dcfs {

using cache::InodeId;

// DirCacheFS is the low-level FUSE filesystem: every op below reads (and, in
// later steps, writes) through cache::/backing:: against `ctx_`, never
// touching the backing filesystem or ctx_.mounts directly itself -- that is
// backing.cc's job. Ops not yet implemented (writes; Open/Read land in step
// 3.3) reply ENOSYS themselves (rather than relying on libfuse's default of
// ENOSYS for an absent callback) so that every op has a DirCacheFS method
// and an entry in the ops table (see fuse_ops.h).
class DirCacheFS {
 public:
  // dcfs has exclusive access to the backing tree (nothing else is supposed
  // to modify it out from under the cache), so these default to long: the
  // cache is only invalidated by our own mutations (from Phase 4 on), not by
  // a timeout racing a change dcfs doesn't know about.
  struct Options {
    absl::Duration attr_timeout = absl::Hours(1);
    absl::Duration entry_timeout = absl::Hours(1);
  };

  // `ctx` must outlive this DirCacheFS.
  DirCacheFS(Context &ctx, Options opts);

  absl::Status Init(struct fuse_conn_info &conn);
  absl::Status Destroy();

  absl::Status Getattr(FuseRequest &req, fuse_ino_t ino, fuse_file_info *fi);
  absl::Status Setattr(
      FuseRequest &req, fuse_ino_t ino, struct stat *attr, int to_set,
      fuse_file_info *fi);

  absl::Status Lookup(
      FuseRequest &req, fuse_ino_t parent_ino, std::string_view name);
  // Inode rows persist across restarts (that is what keeps NFS handles
  // valid), so there is nothing to do when the kernel drops its reference.
  void Forget(FuseRequest &req, fuse_ino_t ino, uint64_t nlookup);
  void ForgetMulti(
      FuseRequest &req, std::span<const fuse_forget_data> forgets);

  absl::Status Readlink(FuseRequest &req, fuse_ino_t ino);
  absl::Status Mknod(
      FuseRequest &req, fuse_ino_t parent, std::string_view name,
      mode_t mode, dev_t rdev);
  absl::Status Mkdir(
      FuseRequest &req, fuse_ino_t parent, std::string_view name,
      mode_t mode);
  absl::Status Unlink(
      FuseRequest &req, fuse_ino_t parent, std::string_view name);
  absl::Status Rmdir(
      FuseRequest &req, fuse_ino_t parent, std::string_view name);
  absl::Status Symlink(
      FuseRequest &req, std::string_view link, fuse_ino_t parent,
      std::string_view name);
  absl::Status Rename(
      FuseRequest &req, fuse_ino_t parent, std::string_view name,
      fuse_ino_t newparent, std::string_view newname, unsigned int flags);
  absl::Status Link(
      FuseRequest &req, fuse_ino_t ino, fuse_ino_t newparent,
      std::string_view newname);

  absl::Status Open(FuseRequest &req, fuse_ino_t ino, fuse_file_info &fi);
  absl::Status Read(
      FuseRequest &req, fuse_ino_t ino, size_t size, off_t off,
      fuse_file_info &fi);
  absl::Status Write(
      FuseRequest &req, fuse_ino_t ino, std::span<const char> buf, off_t off,
      fuse_file_info &fi);
  absl::Status Flush(FuseRequest &req, fuse_ino_t ino, fuse_file_info &fi);
  absl::Status Release(FuseRequest &req, fuse_ino_t ino, fuse_file_info &fi);
  absl::Status Fsync(
      FuseRequest &req, fuse_ino_t ino, int datasync, fuse_file_info &fi);

  absl::Status Opendir(FuseRequest &req, fuse_ino_t ino, fuse_file_info &fi);
  absl::Status Readdir(
      FuseRequest &req, fuse_ino_t ino, size_t size, off_t off,
      fuse_file_info &fi);
  absl::Status Readdirplus(
      FuseRequest &req, fuse_ino_t ino, size_t size, off_t off,
      fuse_file_info &fi);
  absl::Status Releasedir(FuseRequest &req, fuse_ino_t ino, fuse_file_info &fi);
  absl::Status Fsyncdir(
      FuseRequest &req, fuse_ino_t ino, int datasync, fuse_file_info &fi);

  absl::Status Statfs(FuseRequest &req, fuse_ino_t ino);

  absl::Status Setxattr(
      FuseRequest &req, fuse_ino_t ino, std::string_view name,
      std::string_view value, int flags);
  absl::Status Getxattr(
      FuseRequest &req, fuse_ino_t ino, std::string_view name, size_t size);
  absl::Status Listxattr(FuseRequest &req, fuse_ino_t ino, size_t size);
  absl::Status Removexattr(
      FuseRequest &req, fuse_ino_t ino, std::string_view name);

  // The mount is started with -o default_permissions, so the kernel checks
  // permissions itself against the cached attributes Getattr/Lookup report;
  // by the time Access() is called the kernel has already decided to allow
  // the operation, so there is nothing left for the filesystem to check.
  absl::Status Access(FuseRequest &req, fuse_ino_t ino, int mask);

  absl::Status Create(
      FuseRequest &req, fuse_ino_t parent, std::string_view name,
      mode_t mode, fuse_file_info &fi);

  absl::Status Fallocate(
      FuseRequest &req, fuse_ino_t ino, int mode, off_t offset, off_t length,
      fuse_file_info &fi);

  // Whether any Open() handle for `id` is still outstanding (has not gone
  // through Release()). Nothing calls this yet -- it exists for
  // TODO(4.3): row deletion needs to know an inode has no open handles
  // before it can safely drop the row (and, for the last link, the
  // backing file).
  bool HasOpenFiles(InodeId id) const;

 private:
  // The fuse_entry_param for `id`: current cached attributes (refreshed
  // first if not valid), nodeid = id, generation = the row's fuse_gen, and
  // timeouts from opts_. NotFound from a non-root id means the kernel is
  // holding a nodeid this cache no longer has a row for (e.g. the backing
  // inode number was recycled and invalidated it) -- reported as ESTALE,
  // not NotFound, since that is what the kernel does with a stale nodeid.
  absl::StatusOr<fuse_entry_param> EntryFor(InodeId id);

  // An open file handle: the fd Open() reopened `ino` with, and the
  // passthrough backing id the kernel assigned it (0 if the kernel did
  // not grant FUSE_CAP_PASSTHROUGH, or fuse_passthrough_open() otherwise
  // failed for this open -- Read() then serves the fallback path itself).
  struct OpenFile {
    InodeId ino;
    FileDescriptor fd;
    int backing_id = 0;
  };

  Context &ctx_;
  Options opts_;

  // fi.fh handles: never a raw pointer (fi.fh crosses the kernel boundary
  // and outlives nothing we control), just a small monotonically
  // increasing counter indexing open_files_.
  uint64_t next_handle_ = 1;
  absl::flat_hash_map<uint64_t, OpenFile> open_files_;
};

}  // namespace dcfs

#endif  // DCFS_DIR_CACHE_FS_H_
