#include "dcfs/fuse_ops.h"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <sys/stat.h>
#include <sys/types.h>

#include "absl/log/check.h"
#include "absl/log/log.h"
#include "absl/status/status.h"
#include "dcfs/dir_cache_fs.h"
#include "dcfs/fuse_request.h"
#include "fuse_lowlevel.h"

namespace dcfs {
namespace {

// Every request handler goes through this, so it is also where the
// periodic sync point runs: at the start of a request, never in the middle
// of one (see DirCacheFS::MaybeSyncBacking).
DirCacheFS &GetFS(fuse_req_t req) {
  auto *fs = static_cast<DirCacheFS *>(fuse_req_userdata(req));
  CHECK_NE(fs, nullptr);
  fs->MaybeSyncBacking();
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

void ForgetMulti(fuse_req_t req, size_t count, fuse_forget_data *forgets) {
  CHECK_NE(forgets, nullptr);
  FuseRequest fr(req);
  GetFS(req).ForgetMulti(fr, std::span<const fuse_forget_data>(forgets, count));
}

void Getattr(fuse_req_t req, fuse_ino_t ino, fuse_file_info *fi) {
  FuseRequest fr(req);
  fr.ReplyFailureAndLogIfNotOk(GetFS(req).Getattr(fr, ino, fi));
}

void Setattr(
    fuse_req_t req, fuse_ino_t ino, struct stat *attr, int to_set,
    fuse_file_info *fi) {
  FuseRequest fr(req);
  fr.ReplyFailureAndLogIfNotOk(GetFS(req).Setattr(fr, ino, attr, to_set, fi));
}

void Readlink(fuse_req_t req, fuse_ino_t ino) {
  FuseRequest fr(req);
  fr.ReplyFailureAndLogIfNotOk(GetFS(req).Readlink(fr, ino));
}

void Mknod(
    fuse_req_t req, fuse_ino_t parent, const char *name, mode_t mode,
    dev_t rdev) {
  FuseRequest fr(req);
  fr.ReplyFailureAndLogIfNotOk(GetFS(req).Mknod(fr, parent, name, mode, rdev));
}

void Mkdir(fuse_req_t req, fuse_ino_t parent, const char *name, mode_t mode) {
  FuseRequest fr(req);
  fr.ReplyFailureAndLogIfNotOk(GetFS(req).Mkdir(fr, parent, name, mode));
}

void Unlink(fuse_req_t req, fuse_ino_t parent, const char *name) {
  FuseRequest fr(req);
  fr.ReplyFailureAndLogIfNotOk(GetFS(req).Unlink(fr, parent, name));
}

void Rmdir(fuse_req_t req, fuse_ino_t parent, const char *name) {
  FuseRequest fr(req);
  fr.ReplyFailureAndLogIfNotOk(GetFS(req).Rmdir(fr, parent, name));
}

void Symlink(
    fuse_req_t req, const char *link, fuse_ino_t parent, const char *name) {
  FuseRequest fr(req);
  fr.ReplyFailureAndLogIfNotOk(GetFS(req).Symlink(fr, link, parent, name));
}

void Rename(
    fuse_req_t req, fuse_ino_t parent, const char *name,
    fuse_ino_t newparent, const char *newname, unsigned int flags) {
  FuseRequest fr(req);
  fr.ReplyFailureAndLogIfNotOk(
      GetFS(req).Rename(fr, parent, name, newparent, newname, flags));
}

void Link(
    fuse_req_t req, fuse_ino_t ino, fuse_ino_t newparent,
    const char *newname) {
  FuseRequest fr(req);
  fr.ReplyFailureAndLogIfNotOk(GetFS(req).Link(fr, ino, newparent, newname));
}

void Open(fuse_req_t req, fuse_ino_t ino, fuse_file_info *fi) {
  CHECK_NE(fi, nullptr);
  FuseRequest fr(req);
  fr.ReplyFailureAndLogIfNotOk(GetFS(req).Open(fr, ino, *fi));
}

void Read(
    fuse_req_t req, fuse_ino_t ino, size_t size, off_t off,
    fuse_file_info *fi) {
  CHECK_NE(fi, nullptr);
  FuseRequest fr(req);
  fr.ReplyFailureAndLogIfNotOk(GetFS(req).Read(fr, ino, size, off, *fi));
}

void Write(
    fuse_req_t req, fuse_ino_t ino, const char *buf, size_t size, off_t off,
    fuse_file_info *fi) {
  CHECK_NE(fi, nullptr);
  FuseRequest fr(req);
  fr.ReplyFailureAndLogIfNotOk(
      GetFS(req).Write(fr, ino, std::span<const char>(buf, size), off, *fi));
}

void Flush(fuse_req_t req, fuse_ino_t ino, fuse_file_info *fi) {
  CHECK_NE(fi, nullptr);
  FuseRequest fr(req);
  fr.ReplyFailureAndLogIfNotOk(GetFS(req).Flush(fr, ino, *fi));
}

void Release(fuse_req_t req, fuse_ino_t ino, fuse_file_info *fi) {
  CHECK_NE(fi, nullptr);
  FuseRequest fr(req);
  fr.ReplyFailureAndLogIfNotOk(GetFS(req).Release(fr, ino, *fi));
}

void Fsync(fuse_req_t req, fuse_ino_t ino, int datasync, fuse_file_info *fi) {
  CHECK_NE(fi, nullptr);
  FuseRequest fr(req);
  fr.ReplyFailureAndLogIfNotOk(GetFS(req).Fsync(fr, ino, datasync, *fi));
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

void Readdirplus(
    fuse_req_t req, fuse_ino_t ino, size_t size, off_t off,
    fuse_file_info *fi) {
  CHECK_NE(fi, nullptr);
  FuseRequest fr(req);
  fr.ReplyFailureAndLogIfNotOk(
      GetFS(req).Readdirplus(fr, ino, size, off, *fi));
}

void Releasedir(fuse_req_t req, fuse_ino_t ino, fuse_file_info *fi) {
  CHECK_NE(fi, nullptr);
  FuseRequest fr(req);
  fr.ReplyFailureAndLogIfNotOk(GetFS(req).Releasedir(fr, ino, *fi));
}

void Fsyncdir(
    fuse_req_t req, fuse_ino_t ino, int datasync, fuse_file_info *fi) {
  CHECK_NE(fi, nullptr);
  FuseRequest fr(req);
  fr.ReplyFailureAndLogIfNotOk(GetFS(req).Fsyncdir(fr, ino, datasync, *fi));
}

void Statfs(fuse_req_t req, fuse_ino_t ino) {
  FuseRequest fr(req);
  fr.ReplyFailureAndLogIfNotOk(GetFS(req).Statfs(fr, ino));
}

void Setxattr(
    fuse_req_t req, fuse_ino_t ino, const char *name, const char *value,
    size_t size, int flags) {
  FuseRequest fr(req);
  fr.ReplyFailureAndLogIfNotOk(
      GetFS(req).Setxattr(
          fr, ino, name, std::string_view(value, size), flags));
}

void Getxattr(fuse_req_t req, fuse_ino_t ino, const char *name, size_t size) {
  FuseRequest fr(req);
  fr.ReplyFailureAndLogIfNotOk(GetFS(req).Getxattr(fr, ino, name, size));
}

void Listxattr(fuse_req_t req, fuse_ino_t ino, size_t size) {
  FuseRequest fr(req);
  fr.ReplyFailureAndLogIfNotOk(GetFS(req).Listxattr(fr, ino, size));
}

void Removexattr(fuse_req_t req, fuse_ino_t ino, const char *name) {
  FuseRequest fr(req);
  fr.ReplyFailureAndLogIfNotOk(GetFS(req).Removexattr(fr, ino, name));
}

void Access(fuse_req_t req, fuse_ino_t ino, int mask) {
  FuseRequest fr(req);
  fr.ReplyFailureAndLogIfNotOk(GetFS(req).Access(fr, ino, mask));
}

void Create(
    fuse_req_t req, fuse_ino_t parent, const char *name, mode_t mode,
    fuse_file_info *fi) {
  CHECK_NE(fi, nullptr);
  FuseRequest fr(req);
  fr.ReplyFailureAndLogIfNotOk(GetFS(req).Create(fr, parent, name, mode, *fi));
}

void Fallocate(
    fuse_req_t req, fuse_ino_t ino, int mode, off_t offset, off_t length,
    fuse_file_info *fi) {
  CHECK_NE(fi, nullptr);
  FuseRequest fr(req);
  fr.ReplyFailureAndLogIfNotOk(
      GetFS(req).Fallocate(fr, ino, mode, offset, length, *fi));
}

}  // namespace

fuse_lowlevel_ops MakeFuseOps() {
  fuse_lowlevel_ops ops = {};
  ops.init = Init;
  ops.destroy = Destroy;
  ops.lookup = Lookup;
  ops.forget = Forget;
  ops.forget_multi = ForgetMulti;
  ops.getattr = Getattr;
  ops.setattr = Setattr;
  ops.readlink = Readlink;
  ops.mknod = Mknod;
  ops.mkdir = Mkdir;
  ops.unlink = Unlink;
  ops.rmdir = Rmdir;
  ops.symlink = Symlink;
  ops.rename = Rename;
  ops.link = Link;
  ops.open = Open;
  ops.read = Read;
  ops.write = Write;
  ops.flush = Flush;
  ops.release = Release;
  ops.fsync = Fsync;
  ops.opendir = Opendir;
  ops.readdir = Readdir;
  ops.readdirplus = Readdirplus;
  ops.releasedir = Releasedir;
  ops.fsyncdir = Fsyncdir;
  ops.statfs = Statfs;
  ops.setxattr = Setxattr;
  ops.getxattr = Getxattr;
  ops.listxattr = Listxattr;
  ops.removexattr = Removexattr;
  ops.access = Access;
  ops.create = Create;
  ops.fallocate = Fallocate;
  return ops;
}

}  // namespace dcfs
