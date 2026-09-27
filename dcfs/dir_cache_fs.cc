#include "dcfs/dir_cache_fs.h"

#include <cerrno>
#include <cstdint>
#include <fcntl.h>
#include <span>
#include <string_view>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <utility>
#include <vector>

#include "absl/log/check.h"
#include "absl/log/log.h"
#include "absl/status/status.h"
#include "absl/status/status_macros.h"
#include "dcfs/fd.h"
#include "dcfs/fuse_request.h"
#include "dcfs/status.h"
#include "dcfs/syscalls.h"
#include "fuse_lowlevel.h"

namespace dcfs {

DirCacheFS::DirCacheFS(FileDescriptor source_fd)
    : source_fd_(std::move(source_fd)) {}

absl::Status DirCacheFS::Init(struct fuse_conn_info &conn) {
  VLOG(1)
    << "Fuse connection using kernel protocol version " << conn.proto_major
    << "." << conn.proto_minor;
  // Readdirplus is wired into the ops table (see fuse_ops.h) but only
  // ENOSYS-stubbed so far (DirCacheFS::Readdirplus). Registering the
  // callback grants the kernel FUSE_CAP_READDIRPLUS by default, and with
  // FUSE_CAP_READDIRPLUS_AUTO also granted, the kernel decides per-call
  // (e.g. for `ls -l`, which stats every entry) to use readdirplus instead
  // of readdir -- which would then hit ENOSYS and fail the listing outright,
  // instead of the graceful per-op ENOSYS a real caller would see. Withhold
  // both capabilities until Readdirplus is actually implemented.
  fuse_unset_feature_flag(&conn, FUSE_CAP_READDIRPLUS_AUTO);
  fuse_unset_feature_flag(&conn, FUSE_CAP_READDIRPLUS);
  return absl::OkStatus();
}

absl::Status DirCacheFS::Destroy() {
  return absl::OkStatus();
}

absl::Status DirCacheFS::Getattr(
    FuseRequest &req, fuse_ino_t ino, fuse_file_info *fi) {
  if (ino != FUSE_ROOT_ID) return req.ReplyErrno(ENOENT);

  ABSL_ASSIGN_OR_RETURN(
      struct stat st, syscalls::fstatat(*source_fd_, "", AT_EMPTY_PATH));
  st.st_ino = FUSE_ROOT_ID;
  // No cache of our own exists yet, so tell the kernel not to cache either.
  return req.ReplyAttr(st, /*attr_timeout=*/absl::ZeroDuration());
}

absl::Status DirCacheFS::Setattr(
    FuseRequest &req, fuse_ino_t ino, struct stat *attr, int to_set,
    fuse_file_info *fi) {
  return req.ReplyErrno(ENOSYS);
}

absl::Status DirCacheFS::Lookup(
    FuseRequest &req, fuse_ino_t parent_ino, std::string_view name) {
  // Nothing is known to exist under the root yet.
  return req.ReplyErrno(ENOENT);
}

void DirCacheFS::Forget(FuseRequest &req, fuse_ino_t ino, uint64_t nlookup) {
  // No inode table exists yet, so there is nothing to forget.
  req.ReplyNone();
}

void DirCacheFS::ForgetMulti(
    FuseRequest &req, std::span<const fuse_forget_data> forgets) {
  // No inode table exists yet, so there is nothing to forget. Note that,
  // unlike the other not-yet-implemented ops below, forget/forget_multi have
  // no error reply -- fuse_reply_none is the only valid reply -- so this
  // cannot be a req.ReplyErrno(ENOSYS) stub.
  req.ReplyNone();
}

absl::Status DirCacheFS::Readlink(FuseRequest &req, fuse_ino_t ino) {
  return req.ReplyErrno(ENOSYS);
}

absl::Status DirCacheFS::Mknod(
    FuseRequest &req, fuse_ino_t parent, std::string_view name, mode_t mode,
    dev_t rdev) {
  return req.ReplyErrno(ENOSYS);
}

absl::Status DirCacheFS::Mkdir(
    FuseRequest &req, fuse_ino_t parent, std::string_view name,
    mode_t mode) {
  return req.ReplyErrno(ENOSYS);
}

absl::Status DirCacheFS::Unlink(
    FuseRequest &req, fuse_ino_t parent, std::string_view name) {
  return req.ReplyErrno(ENOSYS);
}

absl::Status DirCacheFS::Rmdir(
    FuseRequest &req, fuse_ino_t parent, std::string_view name) {
  return req.ReplyErrno(ENOSYS);
}

absl::Status DirCacheFS::Symlink(
    FuseRequest &req, std::string_view link, fuse_ino_t parent,
    std::string_view name) {
  return req.ReplyErrno(ENOSYS);
}

absl::Status DirCacheFS::Rename(
    FuseRequest &req, fuse_ino_t parent, std::string_view name,
    fuse_ino_t newparent, std::string_view newname, unsigned int flags) {
  return req.ReplyErrno(ENOSYS);
}

absl::Status DirCacheFS::Link(
    FuseRequest &req, fuse_ino_t ino, fuse_ino_t newparent,
    std::string_view newname) {
  return req.ReplyErrno(ENOSYS);
}

absl::Status DirCacheFS::Open(
    FuseRequest &req, fuse_ino_t ino, fuse_file_info &fi) {
  return req.ReplyErrno(ENOSYS);
}

absl::Status DirCacheFS::Read(
    FuseRequest &req, fuse_ino_t ino, size_t size, off_t off,
    fuse_file_info &fi) {
  return req.ReplyErrno(ENOSYS);
}

absl::Status DirCacheFS::Write(
    FuseRequest &req, fuse_ino_t ino, std::span<const char> buf, off_t off,
    fuse_file_info &fi) {
  return req.ReplyErrno(ENOSYS);
}

absl::Status DirCacheFS::Flush(
    FuseRequest &req, fuse_ino_t ino, fuse_file_info &fi) {
  return req.ReplyErrno(ENOSYS);
}

absl::Status DirCacheFS::Release(
    FuseRequest &req, fuse_ino_t ino, fuse_file_info &fi) {
  return req.ReplyErrno(ENOSYS);
}

absl::Status DirCacheFS::Fsync(
    FuseRequest &req, fuse_ino_t ino, int datasync, fuse_file_info &fi) {
  return req.ReplyErrno(ENOSYS);
}

absl::Status DirCacheFS::Opendir(
    FuseRequest &req, fuse_ino_t ino, fuse_file_info &fi) {
  if (ino != FUSE_ROOT_ID) return req.ReplyErrno(ENOENT);
  return req.ReplyOpen(fi);
}

absl::Status DirCacheFS::Readdir(
    FuseRequest &req, fuse_ino_t ino, size_t size, off_t off,
    fuse_file_info &fi) {
  if (ino != FUSE_ROOT_ID) return req.ReplyErrno(ENOENT);

  struct stat st = {};
  st.st_ino = FUSE_ROOT_ID;
  st.st_mode = S_IFDIR;

  std::vector<FuseDirEntry> entries;
  if (off < 1) entries.push_back({.name = ".", .stbuf = st, .off = 1});
  if (off < 2) entries.push_back({.name = "..", .stbuf = st, .off = 2});
  return req.ReplyDirs(entries, size);
}

absl::Status DirCacheFS::Readdirplus(
    FuseRequest &req, fuse_ino_t ino, size_t size, off_t off,
    fuse_file_info &fi) {
  return req.ReplyErrno(ENOSYS);
}

absl::Status DirCacheFS::Releasedir(
    FuseRequest &req, fuse_ino_t ino, fuse_file_info &fi) {
  return req.ReplyErrno(0);
}

absl::Status DirCacheFS::Fsyncdir(
    FuseRequest &req, fuse_ino_t ino, int datasync, fuse_file_info &fi) {
  return req.ReplyErrno(ENOSYS);
}

absl::Status DirCacheFS::Statfs(FuseRequest &req, fuse_ino_t ino) {
  if (ino != FUSE_ROOT_ID) return req.ReplyErrno(ENOENT);
  ABSL_ASSIGN_OR_RETURN(struct statvfs st, syscalls::fstatvfs(*source_fd_));
  return req.ReplyStatfs(st);
}

absl::Status DirCacheFS::Setxattr(
    FuseRequest &req, fuse_ino_t ino, std::string_view name,
    std::string_view value, int flags) {
  return req.ReplyErrno(ENOSYS);
}

absl::Status DirCacheFS::Getxattr(
    FuseRequest &req, fuse_ino_t ino, std::string_view name, size_t size) {
  return req.ReplyErrno(ENOSYS);
}

absl::Status DirCacheFS::Listxattr(
    FuseRequest &req, fuse_ino_t ino, size_t size) {
  return req.ReplyErrno(ENOSYS);
}

absl::Status DirCacheFS::Removexattr(
    FuseRequest &req, fuse_ino_t ino, std::string_view name) {
  return req.ReplyErrno(ENOSYS);
}

absl::Status DirCacheFS::Access(FuseRequest &req, fuse_ino_t ino, int mask) {
  // No permission model exists yet; allow everything.
  return req.ReplyErrno(0);
}

absl::Status DirCacheFS::Create(
    FuseRequest &req, fuse_ino_t parent, std::string_view name, mode_t mode,
    fuse_file_info &fi) {
  return req.ReplyErrno(ENOSYS);
}

absl::Status DirCacheFS::Fallocate(
    FuseRequest &req, fuse_ino_t ino, int mode, off_t offset, off_t length,
    fuse_file_info &fi) {
  return req.ReplyErrno(ENOSYS);
}

}  // namespace dcfs
