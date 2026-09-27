#ifndef DCFS_FUSE_H_
#define DCFS_FUSE_H_

#define FUSE_USE_VERSION FUSE_MAKE_VERSION(3, 18)

#include <span>
#include <string>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <utility>
#include <string_view>

#include "absl/log/check.h"
#include "absl/time/time.h"
#include "absl/log/log.h"
#include "absl/status/statusor.h"
#include "dcfs/fd.h"
#include "dcfs/status.h"
#include "dcfs/syscalls.h"
#include "dcfs/attributes.h"
#include "fuse_lowlevel.h"

namespace dcfs {

struct FuseDirEntry {
   static std::vector<char> GetDirEntryBuffer(fuse_req_t req, std::span<FuseDirEntry> entries);

   std::string name;
   struct stat stbuf;
   off_t off;
};

// A FuseRequest is a wrapper around fuse_req_t that RAII owns replying to the
// request.
class FuseRequest {
 public:
  FuseRequest() = default;

  // Transfers responsibility to this FuseRequest for replying.
  FuseRequest(fuse_req_t req);
  ~FuseRequest();

  FuseRequest(FuseRequest &&);
  FuseRequest(const FuseRequest &) = delete;
  FuseRequest &operator=(FuseRequest &&);
  FuseRequest &operator=(const FuseRequest &) = delete;

  fuse_req_t &operator*();
  fuse_req_t &Get();

  absl::Status ReplyAttr(
    const struct stat &attr, absl::Duration attr_timeout);

  absl::Status ReplyOpen(const fuse_file_info &fi);

  // A "successful failure" response, e.g. ENOENT, where we succeeded in
  // performing an operation that correctly produces an error code.
  absl::Status ReplyErrno(int errnum);

  // A true failure response, e.g. db connection lost. We failed to do what the
  // user asked for.
  //
  // Must be called with a non-ok status.
  absl::Status ReplyFailure(const absl::Status &status);

  absl::Status ReplyBuf(std::string_view buf);
  absl::Status ReplyDirs(std::span<FuseDirEntry> entries, size_t maxsize);

  absl::Status ReplyEntry(
      fuse_ino_t ino, uint64_t generation, struct stat attr,
      absl::Duration attr_timeout, absl::Duration entry_timeout);

  absl::Status ReplyStatfs(const struct statvfs &stbuf);

  // A reply that carries no result, used by ops (e.g. forget) that must not
  // send a normal reply at all.
  void ReplyNone();

  // Implementation details -- helpers for the ops table in dir_cache_fs.cc.
  //
  // ReplyFailureAndLogIfNotOk is for ops (e.g. getattr, lookup) whose method
  // implementation replies on its own on success, and only needs the
  // trampoline to reply with an error if the method returned a non-ok status
  // without having replied itself.
  void ReplyFailureAndLogIfNotOk(const absl::Status &status);
  // ReplyAlwaysAndLogIfNotOk is for ops (e.g. releasedir) that always need an
  // errno reply, whether or not the method's status was ok.
  void ReplyAlwaysAndLogIfNotOk(const absl::Status &status);
 private:
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

#endif  // DCFS_FUSE_H_
