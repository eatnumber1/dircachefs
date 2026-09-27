#include "dcfs/dir_cache_fs.h"

#include <cerrno>
#include <cstdint>
#include <fcntl.h>
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
#include "dcfs/fuse.h"
#include "dcfs/status.h"
#include "dcfs/syscalls.h"
#include "fuse_lowlevel.h"

namespace dcfs {
namespace {

DirCacheFS &GetFS(fuse_req_t req) {
  auto *fs = static_cast<DirCacheFS *>(fuse_req_userdata(req));
  CHECK_NE(fs, nullptr);
  return *fs;
}

void Init(void *userdata, fuse_conn_info *conn) {
  CHECK_NE(userdata, nullptr);
  CHECK_NE(conn, nullptr);
  absl::Status s = static_cast<DirCacheFS *>(userdata)->Init(*conn);
  LOG_IF(ERROR, !s.ok()) << s;
}

void Destroy(void *userdata) {
  CHECK_NE(userdata, nullptr);
  absl::Status s = static_cast<DirCacheFS *>(userdata)->Destroy();
  LOG_IF(ERROR, !s.ok()) << s;
}

void Lookup(fuse_req_t req, fuse_ino_t parent, const char *name) {
  FuseRequest fr(req);
  fr.ReplyFailureAndLogIfNotOk(GetFS(req).Lookup(fr, parent, name));
}

void Forget(fuse_req_t req, fuse_ino_t ino, uint64_t nlookup) {
  FuseRequest fr(req);
  GetFS(req).Forget(fr, ino, nlookup);
}

void Getattr(fuse_req_t req, fuse_ino_t ino, fuse_file_info *fi) {
  FuseRequest fr(req);
  fr.ReplyFailureAndLogIfNotOk(GetFS(req).Getattr(fr, ino, fi));
}

void Opendir(fuse_req_t req, fuse_ino_t ino, fuse_file_info *fi) {
  CHECK_NE(fi, nullptr);
  FuseRequest fr(req);
  fr.ReplyFailureAndLogIfNotOk(GetFS(req).Opendir(fr, ino, *fi));
}

void Readdir(
    fuse_req_t req, fuse_ino_t ino, size_t size, off_t off,
    fuse_file_info *fi) {
  CHECK_NE(fi, nullptr);
  FuseRequest fr(req);
  fr.ReplyFailureAndLogIfNotOk(GetFS(req).Readdir(fr, ino, size, off, *fi));
}

void Releasedir(fuse_req_t req, fuse_ino_t ino, fuse_file_info *fi) {
  CHECK_NE(fi, nullptr);
  FuseRequest fr(req);
  fr.ReplyAlwaysAndLogIfNotOk(GetFS(req).Releasedir(fr, ino, *fi));
}

void Statfs(fuse_req_t req, fuse_ino_t ino) {
  FuseRequest fr(req);
  fr.ReplyFailureAndLogIfNotOk(GetFS(req).Statfs(fr, ino));
}

void Access(fuse_req_t req, fuse_ino_t ino, int mask) {
  FuseRequest fr(req);
  fr.ReplyFailureAndLogIfNotOk(GetFS(req).Access(fr, ino, mask));
}

}  // namespace

DirCacheFS::DirCacheFS(FileDescriptor source_fd)
    : source_fd_(std::move(source_fd)) {}

absl::Status DirCacheFS::Init(struct fuse_conn_info &conn) {
  VLOG(1)
    << "Fuse connection using kernel protocol version " << conn.proto_major
    << "." << conn.proto_minor;
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

absl::Status DirCacheFS::Lookup(
    FuseRequest &req, fuse_ino_t parent_ino, std::string_view name) {
  // Nothing is known to exist under the root yet.
  return req.ReplyErrno(ENOENT);
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

absl::Status DirCacheFS::Releasedir(
    FuseRequest &req, fuse_ino_t ino, fuse_file_info &fi) {
  return req.ReplyErrno(0);
}

absl::Status DirCacheFS::Statfs(FuseRequest &req, fuse_ino_t ino) {
  if (ino != FUSE_ROOT_ID) return req.ReplyErrno(ENOENT);
  ABSL_ASSIGN_OR_RETURN(struct statvfs st, syscalls::fstatvfs(*source_fd_));
  return req.ReplyStatfs(st);
}

absl::Status DirCacheFS::Access(FuseRequest &req, fuse_ino_t ino, int mask) {
  // No permission model exists yet; allow everything.
  return req.ReplyErrno(0);
}

void DirCacheFS::Forget(FuseRequest &req, fuse_ino_t ino, uint64_t nlookup) {
  // No inode table exists yet, so there is nothing to forget.
  req.ReplyNone();
}

fuse_lowlevel_ops MakeDirCacheFsOps() {
  fuse_lowlevel_ops ops = {};
  ops.init = Init;
  ops.destroy = Destroy;
  ops.lookup = Lookup;
  ops.forget = Forget;
  ops.getattr = Getattr;
  ops.opendir = Opendir;
  ops.readdir = Readdir;
  ops.releasedir = Releasedir;
  ops.statfs = Statfs;
  ops.access = Access;
  return ops;
}

}  // namespace dcfs
