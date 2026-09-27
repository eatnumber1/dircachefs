#ifndef DCFS_FUSE_REQUEST_H_
#define DCFS_FUSE_REQUEST_H_

#define FUSE_USE_VERSION FUSE_MAKE_VERSION(3, 18)

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <string_view>
#include <utility>

#include "absl/base/nullability.h"
#include "absl/log/check.h"
#include "absl/log/log.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/time/time.h"
#include "dcfs/status.h"
#include "dcfs/syscalls.h"
#include "fuse_lowlevel.h"

namespace dcfs {

// A directory entry to hand to FuseRequest::ReplyDirs.
struct FuseDirEntry {
  std::string name;
  struct stat stbuf;
  off_t off;
};

// A directory entry (with a full fuse_entry_param, as used by readdirplus)
// to hand to FuseRequest::ReplyDirsPlus.
struct FuseDirEntryPlus {
  std::string name;
  fuse_entry_param entry;
  off_t off;
};

// Fills `buf` (`bufsize` bytes) with as many `entries`, in order, as fit,
// stopping before the first entry that would not fit -- matching
// fuse_add_direntry()'s convention of returning the space an entry would
// need without writing it if that space exceeds what remains. Returns the
// number of bytes written to `buf`.
//
// `req` is passed through to fuse_add_direntry() unused (libfuse ignores it
// there) and may be null, which is what makes this function testable
// without a live request.
size_t AppendDirEntries(
    fuse_req_t req, char *buf, size_t bufsize,
    std::span<const FuseDirEntry> entries);

// As AppendDirEntries, but for readdirplus via fuse_add_direntry_plus().
size_t AppendDirEntriesPlus(
    fuse_req_t req, char *buf, size_t bufsize,
    std::span<const FuseDirEntryPlus> entries);

// A FuseRequest is a wrapper around fuse_req_t that RAII owns replying to the
// request: exactly one Reply* method must be called before it is destroyed,
// and each Reply* method consumes the request -- after any Reply* call the
// object is spent and calling another Reply* method is a bug (caught by
// RET_CHECK).
class FuseRequest {
 public:
  FuseRequest() = default;

  // Transfers responsibility to this FuseRequest for replying.
  explicit FuseRequest(fuse_req_t req);
  ~FuseRequest();

  FuseRequest(FuseRequest &&);
  FuseRequest(const FuseRequest &) = delete;
  FuseRequest &operator=(FuseRequest &&);
  FuseRequest &operator=(const FuseRequest &) = delete;

  // Fills in a fuse_entry_param from `attr` and replies with it.
  absl::Status ReplyEntry(
      fuse_ino_t nodeid, uint64_t generation, const struct stat &attr,
      absl::Duration attr_timeout, absl::Duration entry_timeout);

  // A negative lookup reply (ino 0): lets the kernel cache the fact that
  // the looked-up name does not exist for `entry_timeout`.
  absl::Status ReplyNegativeEntry(absl::Duration entry_timeout);

  // Delegates to the 3-argument overload with generation 0, for callers
  // (e.g. DirCacheFS::Getattr, which has no inode table yet) that don't
  // have a generation number to report.
  absl::Status ReplyAttr(
      const struct stat &attr, absl::Duration attr_timeout);
  absl::Status ReplyAttr(
      const struct stat &attr, absl::Duration attr_timeout,
      uint64_t generation);

  absl::Status ReplyReadlink(std::string_view target);

  absl::Status ReplyOpen(const fuse_file_info &fi);
  absl::Status ReplyCreate(
      const fuse_entry_param &entry, const fuse_file_info &fi);
  absl::Status ReplyWrite(size_t count);

  // Wraps fuse_passthrough_open(): tells the kernel to serve reads (and,
  // once Phase 4 lands, writes) on this open directly against `fd`
  // instead of routing them through us. The returned int is the backing
  // id to report in fi.backing_id when positive; 0 means the kernel did
  // not grant FUSE_CAP_PASSTHROUGH or this open otherwise failed to set
  // it up -- that is a normal outcome for the caller to fall back on, not
  // a C++-level error (only a dead request, a bug, produces a non-ok
  // status). Unlike the Reply* methods this does not consume the
  // request -- a Reply* call must still follow.
  absl::StatusOr<int> PassthroughOpen(int fd);

  // Wraps fuse_passthrough_close(), undoing a PassthroughOpen() that
  // returned a positive backing id. Also does not consume the request.
  absl::Status PassthroughClose(int backing_id);

  absl::Status ReplyBuf(std::string_view buf);
  // Used when the caller (getxattr/listxattr) passed size 0: replies with
  // just the size the value/list would need.
  absl::Status ReplyXattrSize(size_t size);
  absl::Status ReplyStatfs(const struct statvfs &stbuf);

  absl::Status ReplyDirs(std::span<FuseDirEntry> entries, size_t maxsize);
  absl::Status ReplyDirsPlus(
      std::span<FuseDirEntryPlus> entries, size_t maxsize);

  // A "successful failure" response, e.g. ENOENT, where we succeeded in
  // performing an operation that correctly produces an error code.
  absl::Status ReplyErrno(int errnum);

  // A true failure response, e.g. db connection lost. We failed to do what
  // the user asked for.
  //
  // Must be called with a non-ok status. Always replies with a non-zero
  // errno, even if StatusToErrno(status) would (incorrectly) map to 0.
  absl::Status ReplyFailure(const absl::Status &status);

  // A reply that carries no result, used by ops (e.g. forget, forget_multi)
  // that must not send a normal reply at all.
  void ReplyNone();

  // Implementation detail -- helper for the trampolines in fuse_ops.cc: every
  // DirCacheFS::Xxx op method is expected to reply on its own (directly, or
  // via a stub's ReplyErrno) whenever it returns absl::OkStatus(); this is
  // the uniform trampoline glue that replies with the failure if (and only
  // if) the method returned a non-ok status without having replied itself.
  void ReplyFailureAndLogIfNotOk(const absl::Status &status);

 private:
  // Sends `param` via fuse_reply_entry(); shared by ReplyEntry and
  // ReplyNegativeEntry.
  absl::Status ReplyEntryParam(const fuse_entry_param &param);

  std::optional<fuse_req_t> req_;
};

struct LogFuseFileInfo {
 public:
  explicit LogFuseFileInfo(fuse_file_info *fi);
  explicit LogFuseFileInfo(fuse_file_info &fi);

  template <typename Sink>
  friend void AbslStringify(Sink &sink, const LogFuseFileInfo &lfi);

 private:
  fuse_file_info *absl_nullable fi_ = nullptr;
};

// implementation details below

template <typename Sink>
void AbslStringify(Sink &sink, const LogFuseFileInfo &lfi) {
  if (lfi.fi_ == nullptr) {
    absl::Format(&sink, "nullptr");
    return;
  }
  fuse_file_info &fi = *lfi.fi_;
  absl::Format(&sink, "fuse_file_info {\n");
  if (fi.flags) {
    absl::Format(&sink, "\t.flags = %v\n", LogOpenFlags(fi.flags));
  }
  if (fi.writepage) {
    absl::Format(
        &sink, "\t.writepage = %v\n", static_cast<bool>(fi.writepage));
  }
  if (fi.direct_io) {
    absl::Format(
        &sink, "\t.direct_io = %v\n", static_cast<bool>(fi.direct_io));
  }
  if (fi.keep_cache) {
    absl::Format(
        &sink, "\t.keep_cache = %v\n", static_cast<bool>(fi.keep_cache));
  }
  if (fi.flush) {
    absl::Format(&sink, "\t.flush = %v\n", static_cast<bool>(fi.flush));
  }
  if (fi.nonseekable) {
    absl::Format(
        &sink, "\t.nonseekable = %v\n", static_cast<bool>(fi.nonseekable));
  }
  if (fi.flock_release) {
    absl::Format(
        &sink, "\t.flock_release = %v\n", static_cast<bool>(fi.flock_release));
  }
  if (fi.cache_readdir) {
    absl::Format(
        &sink, "\t.cache_readdir = %v\n", static_cast<bool>(fi.cache_readdir));
  }
  if (fi.noflush) {
    absl::Format(&sink, "\t.noflush = %v\n", static_cast<bool>(fi.noflush));
  }
  if (fi.parallel_direct_writes) {
    absl::Format(
        &sink, "\t.parallel_direct_writes = %v\n",
        static_cast<bool>(fi.parallel_direct_writes));
  }
  if (fi.fh) {
    absl::Format(&sink, "\t.fh = %p\n", reinterpret_cast<void *>(fi.fh));
  }
  if (fi.lock_owner) {
    absl::Format(&sink, "\t.lock_owner = %d\n", fi.lock_owner);
  }
  if (fi.poll_events) {
    absl::Format(&sink, "\t.poll_events = %d\n", fi.poll_events);
  }
  absl::Format(&sink, "}");
}

}  // namespace dcfs

#endif  // DCFS_FUSE_REQUEST_H_
