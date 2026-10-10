#ifndef DCFS_FUSE_REQUEST_H_
#define DCFS_FUSE_REQUEST_H_

#define FUSE_USE_VERSION FUSE_MAKE_VERSION(3, 18)

#include <sys/stat.h>
#include <sys/statvfs.h>

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>

#include "absl/log/check.h"
#include "absl/log/log.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/time/time.h"
#include "dcfs/credentials.h"
#include "dcfs/status.h"
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
size_t AppendDirEntries(fuse_req_t req, char *buf, size_t bufsize,
                        std::span<const FuseDirEntry> entries);

// As AppendDirEntries, but for readdirplus via fuse_add_direntry_plus().
size_t AppendDirEntriesPlus(fuse_req_t req, char *buf, size_t bufsize,
                            std::span<const FuseDirEntryPlus> entries);

// A FuseRequest is a wrapper around fuse_req_t that RAII owns replying to the
// request: exactly one Reply* method must be called before it is destroyed,
// and each Reply* method consumes the request -- after any Reply* call the
// object is spent and calling another Reply* method is a bug (caught by
// RET_CHECK).
class FuseRequest {
 public:
  // Transfers responsibility to this FuseRequest for replying.
  explicit FuseRequest(fuse_req_t req);
  ~FuseRequest();

  // Neither copied nor moved: each lives in the libfuse callback that
  // serves its request (fuse_ops.cc), which is what makes "replied by the
  // time it is destroyed" checkable.
  FuseRequest(FuseRequest &&) = delete;
  FuseRequest(const FuseRequest &) = delete;
  FuseRequest &operator=(FuseRequest &&) = delete;
  FuseRequest &operator=(const FuseRequest &) = delete;

  // The caller's filesystem identity: the uid/gid the kernel sent with the
  // request (fuse_req_ctx) and the calling thread's supplementary groups
  // (fuse_req_getgroups, which reads them from /proc/<tid>/task/<tid>/status
  // -- the kernel does not send them). If the groups cannot be read (the
  // caller already exited, or lives in a pid namespace the daemon cannot
  // see, where the kernel sends pid 0), the result has NO supplementary
  // groups: never more than the caller has, at worst a spurious EACCES for
  // access granted only through one of them. Does not consume the request;
  // must be called before a Reply*.
  absl::StatusOr<Credentials> Caller() const;

  // Fills in a fuse_entry_param from `attr` and replies with it.
  absl::Status ReplyEntry(fuse_ino_t nodeid, uint64_t generation,
                          const struct stat &attr, absl::Duration attr_timeout,
                          absl::Duration entry_timeout);

  // A negative lookup reply (ino 0): lets the kernel cache the fact that
  // the looked-up name does not exist for `entry_timeout`.
  absl::Status ReplyNegativeEntry(absl::Duration entry_timeout);

  absl::Status ReplyAttr(const struct stat &attr, absl::Duration attr_timeout);

  absl::Status ReplyReadlink(std::string_view target);

  absl::Status ReplyOpen(const fuse_file_info &fi);
  absl::Status ReplyCreate(const fuse_entry_param &entry,
                           const fuse_file_info &fi);
  absl::Status ReplyWrite(size_t count);
  // An ioctl's result: `result` (0) and its output buffer.
  absl::Status ReplyIoctl(int result, std::string_view buf);

  // Wraps fuse_passthrough_open(): tells the kernel to serve reads and
  // writes on this open directly against `fd` instead of routing them
  // through us. The returned int is the backing
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
  absl::Status ReplyDirsPlus(std::span<FuseDirEntryPlus> entries,
                             size_t maxsize);

  // A "successful failure" response, e.g. ENOENT, where we succeeded in
  // performing an operation that correctly produces an error code.
  absl::Status ReplyErrno(int errnum);

  // The errno of the reply sent so far: what ReplyErrno (or ReplyFailure)
  // sent, 0 if none was (a successful reply, or none yet: see replied()).
  int errno_sent() const { return errno_sent_; }
  // Whether a Reply* method has been called.
  bool replied() const { return !req_.has_value(); }

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
  int errno_sent_ = 0;
};

}  // namespace dcfs

#endif  // DCFS_FUSE_REQUEST_H_
