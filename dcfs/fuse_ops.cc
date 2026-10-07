#include "dcfs/fuse_ops.h"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <string_view>
#include <linux/fs.h>  // FS_IOC_SETFLAGS
#include <sys/stat.h>
#include <sys/types.h>

#include "absl/log/check.h"
#include "absl/log/log.h"
#include "absl/status/status.h"
#include "dcfs/context.h"
#include "dcfs/dir_cache_fs.h"
#include "dcfs/fuse_request.h"
#include "dcfs/invariant_checks.h"
#include "dcfs/protocol_events.h"
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

events::Ino Ino(fuse_ino_t ino) { return static_cast<events::Ino>(ino); }

// Serves one request as one frame of the protocol events (see
// dcfs/protocol_events.h): it begins after GetFS's periodic sync point,
// which is a frame of its own, and ends after the reply. `handler(fs, fr)`
// returns the status to reply.
//
// It is also one frame of the runtime invariant checks
// (dcfs/invariant_checks.h), around the protocol-event frame: RequestEnd
// comes after the reply has been sent.
template <typename Handler>
void Serve(fuse_req_t req, const events::Request &request, Handler handler) {
  FuseRequest fr(req);
  DirCacheFS &fs = GetFS(req);
  Context &ctx = fs.context();
  ctx.checks->RequestBegin(ctx, fs, request);
  // The request checkpoints ask about (dcfs/interrupts.h).
  ctx.interrupts->Begin(req);
  {
    events::RequestScope scope(*ctx.events, ctx, request);
    fr.ReplyFailureAndLogIfNotOk(scope.Finish(handler(fs, fr)));
  }
  ctx.interrupts->End();
  ctx.checks->RequestEnd(ctx, fs, request);
}

void Init(void *userdata, fuse_conn_info *conn) {
  CHECK_NE(userdata, nullptr);
  CHECK_NE(conn, nullptr);
  absl::Status s = static_cast<DirCacheFS *>(userdata)->Init(*conn);
  LOG_IF(ERROR, !s.ok()) << s;
}

void Destroy(void *userdata) {
  CHECK_NE(userdata, nullptr);
  auto *fs = static_cast<DirCacheFS *>(userdata);
  absl::Status s = fs->Destroy();
  LOG_IF(ERROR, !s.ok()) << s;
  fs->context().checks->Destroyed(fs->context(), *fs);
}

void Lookup(fuse_req_t req, fuse_ino_t parent, const char *name) {
  Serve(req, {.op = events::Op::kLookup, .ino = Ino(parent), .name = name},
        [&](DirCacheFS &fs, FuseRequest &fr) {
          return fs.Lookup(fr, parent, name);
        });
}

// FORGET and BATCH_FORGET are no protocol-event frame (they change nothing
// the model has), but they are invariant-check frames like any request.
void Forget(fuse_req_t req, fuse_ino_t ino, uint64_t nlookup) {
  FuseRequest fr(req);
  DirCacheFS &fs = GetFS(req);
  Context &ctx = fs.context();
  const events::Request request{.op = events::Op::kForget, .ino = Ino(ino)};
  ctx.checks->RequestBegin(ctx, fs, request);
  ctx.checks->Forgetting(ctx, fs, ino, nlookup);
  fs.Forget(fr, ino, nlookup);
  ctx.checks->RequestEnd(ctx, fs, request);
}

void ForgetMulti(fuse_req_t req, size_t count, fuse_forget_data *forgets) {
  CHECK_NE(forgets, nullptr);
  FuseRequest fr(req);
  DirCacheFS &fs = GetFS(req);
  Context &ctx = fs.context();
  const events::Request request{
      .op = events::Op::kBatchForget,
      .ino = count > 0 ? Ino(forgets[0].ino) : 0};
  ctx.checks->RequestBegin(ctx, fs, request);
  for (size_t i = 0; i < count; ++i) {
    ctx.checks->Forgetting(ctx, fs, forgets[i].ino, forgets[i].nlookup);
  }
  fs.ForgetMulti(fr, std::span<const fuse_forget_data>(forgets, count));
  ctx.checks->RequestEnd(ctx, fs, request);
}

void Getattr(fuse_req_t req, fuse_ino_t ino, fuse_file_info *fi) {
  Serve(req, {.op = events::Op::kGetattr, .ino = Ino(ino)},
        [&](DirCacheFS &fs, FuseRequest &fr) {
          return fs.Getattr(fr, ino, fi);
        });
}

void Setattr(
    fuse_req_t req, fuse_ino_t ino, struct stat *attr, int to_set,
    fuse_file_info *fi) {
  Serve(req,
        {.op = events::Op::kSetattr,
         .ino = Ino(ino),
         .flags = static_cast<unsigned int>(to_set)},
        [&](DirCacheFS &fs, FuseRequest &fr) {
          return fs.Setattr(fr, ino, attr, to_set, fi);
        });
}

void Readlink(fuse_req_t req, fuse_ino_t ino) {
  Serve(req, {.op = events::Op::kReadlink, .ino = Ino(ino)},
        [&](DirCacheFS &fs, FuseRequest &fr) { return fs.Readlink(fr, ino); });
}

void Mknod(
    fuse_req_t req, fuse_ino_t parent, const char *name, mode_t mode,
    dev_t rdev) {
  Serve(req, {.op = events::Op::kMknod, .ino = Ino(parent), .name = name},
        [&](DirCacheFS &fs, FuseRequest &fr) {
          return fs.Mknod(fr, parent, name, mode, rdev);
        });
}

void Mkdir(fuse_req_t req, fuse_ino_t parent, const char *name, mode_t mode) {
  Serve(req, {.op = events::Op::kMkdir, .ino = Ino(parent), .name = name},
        [&](DirCacheFS &fs, FuseRequest &fr) {
          return fs.Mkdir(fr, parent, name, mode);
        });
}

void Unlink(fuse_req_t req, fuse_ino_t parent, const char *name) {
  Serve(req, {.op = events::Op::kUnlink, .ino = Ino(parent), .name = name},
        [&](DirCacheFS &fs, FuseRequest &fr) {
          return fs.Unlink(fr, parent, name);
        });
}

void Rmdir(fuse_req_t req, fuse_ino_t parent, const char *name) {
  Serve(req, {.op = events::Op::kRmdir, .ino = Ino(parent), .name = name},
        [&](DirCacheFS &fs, FuseRequest &fr) {
          return fs.Rmdir(fr, parent, name);
        });
}

void Symlink(
    fuse_req_t req, const char *link, fuse_ino_t parent, const char *name) {
  Serve(req, {.op = events::Op::kSymlink, .ino = Ino(parent), .name = name},
        [&](DirCacheFS &fs, FuseRequest &fr) {
          return fs.Symlink(fr, link, parent, name);
        });
}

void Rename(
    fuse_req_t req, fuse_ino_t parent, const char *name,
    fuse_ino_t newparent, const char *newname, unsigned int flags) {
  Serve(req,
        {.op = events::Op::kRename,
         .ino = Ino(parent),
         .name = name,
         .newparent = Ino(newparent),
         .newname = newname,
         .flags = flags},
        [&](DirCacheFS &fs, FuseRequest &fr) {
          return fs.Rename(fr, parent, name, newparent, newname, flags);
        });
}

void Link(
    fuse_req_t req, fuse_ino_t ino, fuse_ino_t newparent,
    const char *newname) {
  // The link of an unnamed O_TMPFILE file is, to the new parent, a create
  // (the protocol events' kLinkTmpfile).
  const DirCacheFS *fs = static_cast<DirCacheFS *>(fuse_req_userdata(req));
  CHECK_NE(fs, nullptr);
  Serve(req,
        {.op = fs->IsUnnamedTmpfile(ino) ? events::Op::kLinkTmpfile
                                         : events::Op::kLink,
         .ino = Ino(ino),
         .newparent = Ino(newparent),
         .newname = newname},
        [&](DirCacheFS &fs, FuseRequest &fr) {
          return fs.Link(fr, ino, newparent, newname);
        });
}

void Open(fuse_req_t req, fuse_ino_t ino, fuse_file_info *fi) {
  CHECK_NE(fi, nullptr);
  Serve(req, {.op = events::Op::kOpen, .ino = Ino(ino)},
        [&](DirCacheFS &fs, FuseRequest &fr) { return fs.Open(fr, ino, *fi); });
}

void Read(
    fuse_req_t req, fuse_ino_t ino, size_t size, off_t off,
    fuse_file_info *fi) {
  CHECK_NE(fi, nullptr);
  Serve(req, {.op = events::Op::kRead, .ino = Ino(ino)},
        [&](DirCacheFS &fs, FuseRequest &fr) {
          return fs.Read(fr, ino, size, off, *fi);
        });
}

void Write(
    fuse_req_t req, fuse_ino_t ino, const char *buf, size_t size, off_t off,
    fuse_file_info *fi) {
  CHECK_NE(fi, nullptr);
  Serve(req, {.op = events::Op::kWrite, .ino = Ino(ino)},
        [&](DirCacheFS &fs, FuseRequest &fr) {
          return fs.Write(fr, ino, std::span<const char>(buf, size), off, *fi);
        });
}

void Flush(fuse_req_t req, fuse_ino_t ino, fuse_file_info *fi) {
  CHECK_NE(fi, nullptr);
  Serve(req, {.op = events::Op::kFlush, .ino = Ino(ino)},
        [&](DirCacheFS &fs, FuseRequest &fr) { return fs.Flush(fr, ino, *fi); });
}

void Release(fuse_req_t req, fuse_ino_t ino, fuse_file_info *fi) {
  CHECK_NE(fi, nullptr);
  Serve(req, {.op = events::Op::kRelease, .ino = Ino(ino)},
        [&](DirCacheFS &fs, FuseRequest &fr) {
          return fs.Release(fr, ino, *fi);
        });
}

void Fsync(fuse_req_t req, fuse_ino_t ino, int datasync, fuse_file_info *fi) {
  CHECK_NE(fi, nullptr);
  Serve(req, {.op = events::Op::kFsync, .ino = Ino(ino)},
        [&](DirCacheFS &fs, FuseRequest &fr) {
          return fs.Fsync(fr, ino, datasync, *fi);
        });
}

void Opendir(fuse_req_t req, fuse_ino_t ino, fuse_file_info *fi) {
  CHECK_NE(fi, nullptr);
  Serve(req, {.op = events::Op::kOpendir, .ino = Ino(ino)},
        [&](DirCacheFS &fs, FuseRequest &fr) {
          return fs.Opendir(fr, ino, *fi);
        });
}

void Readdir(
    fuse_req_t req, fuse_ino_t ino, size_t size, off_t off,
    fuse_file_info *fi) {
  CHECK_NE(fi, nullptr);
  Serve(req, {.op = events::Op::kReaddir, .ino = Ino(ino), .offset = off},
        [&](DirCacheFS &fs, FuseRequest &fr) {
          return fs.Readdir(fr, ino, size, off, *fi);
        });
}

void Readdirplus(
    fuse_req_t req, fuse_ino_t ino, size_t size, off_t off,
    fuse_file_info *fi) {
  CHECK_NE(fi, nullptr);
  Serve(req, {.op = events::Op::kReaddirplus, .ino = Ino(ino), .offset = off},
        [&](DirCacheFS &fs, FuseRequest &fr) {
          return fs.Readdirplus(fr, ino, size, off, *fi);
        });
}

void Releasedir(fuse_req_t req, fuse_ino_t ino, fuse_file_info *fi) {
  CHECK_NE(fi, nullptr);
  Serve(req, {.op = events::Op::kReleasedir, .ino = Ino(ino)},
        [&](DirCacheFS &fs, FuseRequest &fr) {
          return fs.Releasedir(fr, ino, *fi);
        });
}

void Fsyncdir(
    fuse_req_t req, fuse_ino_t ino, int datasync, fuse_file_info *fi) {
  CHECK_NE(fi, nullptr);
  Serve(req, {.op = events::Op::kFsyncdir, .ino = Ino(ino)},
        [&](DirCacheFS &fs, FuseRequest &fr) {
          return fs.Fsyncdir(fr, ino, datasync, *fi);
        });
}

void Statfs(fuse_req_t req, fuse_ino_t ino) {
  Serve(req, {.op = events::Op::kStatfs, .ino = Ino(ino)},
        [&](DirCacheFS &fs, FuseRequest &fr) { return fs.Statfs(fr, ino); });
}

void Setxattr(
    fuse_req_t req, fuse_ino_t ino, const char *name, const char *value,
    size_t size, int flags) {
  Serve(req, {.op = events::Op::kSetxattr, .ino = Ino(ino)},
        [&](DirCacheFS &fs, FuseRequest &fr) {
          return fs.Setxattr(fr, ino, name, std::string_view(value, size),
                             flags);
        });
}

void Getxattr(fuse_req_t req, fuse_ino_t ino, const char *name, size_t size) {
  Serve(req, {.op = events::Op::kGetxattr, .ino = Ino(ino)},
        [&](DirCacheFS &fs, FuseRequest &fr) {
          return fs.Getxattr(fr, ino, name, size);
        });
}

void Listxattr(fuse_req_t req, fuse_ino_t ino, size_t size) {
  Serve(req, {.op = events::Op::kListxattr, .ino = Ino(ino)},
        [&](DirCacheFS &fs, FuseRequest &fr) {
          return fs.Listxattr(fr, ino, size);
        });
}

void Removexattr(fuse_req_t req, fuse_ino_t ino, const char *name) {
  Serve(req, {.op = events::Op::kRemovexattr, .ino = Ino(ino)},
        [&](DirCacheFS &fs, FuseRequest &fr) {
          return fs.Removexattr(fr, ino, name);
        });
}

void Access(fuse_req_t req, fuse_ino_t ino, int mask) {
  Serve(req, {.op = events::Op::kAccess, .ino = Ino(ino)},
        [&](DirCacheFS &fs, FuseRequest &fr) {
          return fs.Access(fr, ino, mask);
        });
}

void Create(
    fuse_req_t req, fuse_ino_t parent, const char *name, mode_t mode,
    fuse_file_info *fi) {
  CHECK_NE(fi, nullptr);
  Serve(req, {.op = events::Op::kCreate, .ino = Ino(parent), .name = name},
        [&](DirCacheFS &fs, FuseRequest &fr) {
          return fs.Create(fr, parent, name, mode, *fi);
        });
}

void Fallocate(
    fuse_req_t req, fuse_ino_t ino, int mode, off_t offset, off_t length,
    fuse_file_info *fi) {
  CHECK_NE(fi, nullptr);
  Serve(req, {.op = events::Op::kFallocate, .ino = Ino(ino)},
        [&](DirCacheFS &fs, FuseRequest &fr) {
          return fs.Fallocate(fr, ino, mode, offset, length, *fi);
        });
}

void CopyFileRange(fuse_req_t req, fuse_ino_t ino_in, off_t off_in,
                   fuse_file_info *fi_in, fuse_ino_t ino_out, off_t off_out,
                   fuse_file_info *fi_out, size_t len, int flags) {
  CHECK_NE(fi_in, nullptr);
  CHECK_NE(fi_out, nullptr);
  Serve(req, {.op = events::Op::kCopyFileRange, .ino = Ino(ino_out)},
        [&](DirCacheFS &fs, FuseRequest &fr) {
          return fs.CopyFileRange(fr, ino_in, off_in, *fi_in, ino_out,
                                  off_out, *fi_out, len, flags);
        });
}

void Ioctl(fuse_req_t req, fuse_ino_t ino, unsigned int cmd, void *arg,
           fuse_file_info *fi, unsigned flags, const void *in_buf,
           size_t in_bufsz, size_t out_bufsz) {
  int ioctl_arg = 0;
  if (cmd == FS_IOC_SETFLAGS && in_bufsz >= sizeof(ioctl_arg)) {
    std::memcpy(&ioctl_arg, in_buf, sizeof(ioctl_arg));
  }
  Serve(req,
        {.op = events::Op::kIoctl,
         .ino = Ino(ino),
         .flags = cmd,
         .ioctl_arg = ioctl_arg},
        [&](DirCacheFS &fs, FuseRequest &fr) {
          return fs.Ioctl(
              fr, ino, cmd, fi, flags,
              std::string_view(static_cast<const char *>(in_buf), in_bufsz),
              out_bufsz);
        });
}

void Tmpfile(fuse_req_t req, fuse_ino_t parent, mode_t mode,
             fuse_file_info *fi) {
  CHECK_NE(fi, nullptr);
  Serve(req, {.op = events::Op::kTmpfile, .ino = Ino(parent)},
        [&](DirCacheFS &fs, FuseRequest &fr) {
          return fs.Tmpfile(fr, parent, mode, *fi);
        });
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
  ops.copy_file_range = CopyFileRange;
  ops.ioctl = Ioctl;
  ops.tmpfile = Tmpfile;
  return ops;
}

}  // namespace dcfs
