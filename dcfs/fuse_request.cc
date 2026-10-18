// SPDX-License-Identifier: MIT
#include "dcfs/fuse_request.h"

#include <linux/capability.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/types.h>

#include <cerrno>
#include <cstdint>
#include <cstring>
#include <optional>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

#include "absl/container/fixed_array.h"
#include "absl/log/check.h"
#include "absl/log/log.h"
#include "absl/status/status.h"
#include "absl/status/status_macros.h"
#include "absl/status/statusor.h"
#include "absl/time/time.h"
#include "dcfs/credentials.h"
#include "dcfs/proc_status.h"
#include "dcfs/ret_check.h"
#include "dcfs/status.h"
#include "fuse_lowlevel.h"

namespace dcfs {

size_t AppendDirEntries(fuse_req_t req, char *buf, size_t bufsize,
                        std::span<const FuseDirEntry> entries) {
  size_t used = 0;
  for (const FuseDirEntry &entry : entries) {
    size_t remaining = bufsize - used;
    // fuse_add_direntry() returns the space this entry needs without
    // writing it if that exceeds `remaining` (it checks buf == nullptr ||
    // needed > bufsize internally), so a single call both measures and
    // writes each entry.
    size_t needed =
        fuse_add_direntry(req, buf == nullptr ? nullptr : buf + used, remaining,
                          entry.name.c_str(), &entry.stbuf, entry.off);
    if (needed > remaining) break;
    used += needed;
  }
  return used;
}

size_t AppendDirEntriesPlus(fuse_req_t req, char *buf, size_t bufsize,
                            std::span<const FuseDirEntryPlus> entries) {
  size_t used = 0;
  for (const FuseDirEntryPlus &entry : entries) {
    size_t remaining = bufsize - used;
    size_t needed = fuse_add_direntry_plus(
        req, buf == nullptr ? nullptr : buf + used, remaining,
        entry.name.c_str(), &entry.entry, entry.off);
    if (needed > remaining) break;
    used += needed;
  }
  return used;
}

FuseRequest::FuseRequest(fuse_req_t req) : req_(std::move(req)) {}

FuseRequest::~FuseRequest() {
  if (!req_) return;
  // Every op method is expected to reply before returning (see
  // ReplyFailureAndLogIfNotOk); reaching here means one didn't. FuseRequest
  // only wraps the raw req_, not which op or inode it was for, so that
  // context can't be logged here -- replying ECOMM at least keeps the
  // kernel from waiting on a request that will never get a normal reply.
  LOG(ERROR) << "Replying to FuseRequest in destructor";
  absl::Status st = ReplyErrno(ECOMM);
  LOG_IF(ERROR, !st.ok()) << "Failed to send reply: " << st;
}

absl::Status FuseRequest::ReplyEntryParam(const fuse_entry_param &param) {
  RET_CHECK(req_.has_value()) << "FuseRequest already replied";
  absl::Status st = ProducedErrnoToStatus(-fuse_reply_entry(*req_, &param),
                                          "fuse_reply_entry");
  req_ = std::nullopt;
  return st;
}

absl::Status FuseRequest::ReplyEntry(fuse_ino_t nodeid, uint64_t generation,
                                     const struct stat &attr,
                                     absl::Duration attr_timeout,
                                     absl::Duration entry_timeout) {
  fuse_entry_param param{
      .ino = nodeid,
      .generation = generation,
      .attr = attr,
      .attr_timeout = absl::ToDoubleSeconds(attr_timeout),
      .entry_timeout = absl::ToDoubleSeconds(entry_timeout),
  };
  return ReplyEntryParam(param);
}

absl::Status FuseRequest::ReplyNegativeEntry(absl::Duration entry_timeout) {
  fuse_entry_param param = {};
  param.ino = 0;
  param.entry_timeout = absl::ToDoubleSeconds(entry_timeout);
  return ReplyEntryParam(param);
}

absl::Status FuseRequest::ReplyAttr(const struct stat &attr,
                                    absl::Duration attr_timeout) {
  RET_CHECK(req_.has_value()) << "FuseRequest already replied";
  absl::Status st = ProducedErrnoToStatus(
      -fuse_reply_attr(*req_, &attr, absl::ToDoubleSeconds(attr_timeout)),
      "fuse_reply_attr");
  req_ = std::nullopt;
  return st;
}

absl::Status FuseRequest::ReplyReadlink(std::string_view target) {
  RET_CHECK(req_.has_value()) << "FuseRequest already replied";
  // fuse_reply_readlink() wants a NUL-terminated C string; `target` is not
  // guaranteed to be one.
  std::string target_str(target);
  absl::Status st = ProducedErrnoToStatus(
      -fuse_reply_readlink(*req_, target_str.c_str()), "fuse_reply_readlink");
  req_ = std::nullopt;
  return st;
}

absl::Status FuseRequest::ReplyOpen(const fuse_file_info &fi) {
  RET_CHECK(req_.has_value()) << "FuseRequest already replied";
  absl::Status st =
      ProducedErrnoToStatus(-fuse_reply_open(*req_, &fi), "fuse_reply_open");
  req_ = std::nullopt;
  return st;
}

absl::Status FuseRequest::ReplyCreate(const fuse_entry_param &entry,
                                      const fuse_file_info &fi) {
  RET_CHECK(req_.has_value()) << "FuseRequest already replied";
  absl::Status st = ProducedErrnoToStatus(
      -fuse_reply_create(*req_, &entry, &fi), "fuse_reply_create");
  req_ = std::nullopt;
  return st;
}

absl::Status FuseRequest::ReplyWrite(size_t count) {
  RET_CHECK(req_.has_value()) << "FuseRequest already replied";
  absl::Status st = ProducedErrnoToStatus(-fuse_reply_write(*req_, count),
                                          "fuse_reply_write");
  req_ = std::nullopt;
  return st;
}

absl::Status FuseRequest::ReplyIoctl(int result, std::string_view buf) {
  RET_CHECK(req_.has_value()) << "FuseRequest already replied";
  absl::Status st = ProducedErrnoToStatus(
      -fuse_reply_ioctl(*req_, result, buf.data(), buf.size()),
      "fuse_reply_ioctl");
  req_ = std::nullopt;
  return st;
}

absl::StatusOr<int> FuseRequest::PassthroughOpen(int fd) {
  RET_CHECK(req_.has_value()) << "FuseRequest already replied";
  // fuse_passthrough_open() itself never returns negative -- it clamps any
  // ioctl failure to 0 (logging the errno itself) -- so 0 vs. positive is
  // the whole contract; this does not consume req_.
  return fuse_passthrough_open(*req_, fd);
}

absl::Status FuseRequest::PassthroughClose(int backing_id) {
  RET_CHECK(req_.has_value()) << "FuseRequest already replied";
  // Unlike fuse_passthrough_open(), fuse_passthrough_close() passes the
  // underlying ioctl()'s return straight through, so a negative result
  // here means -1 with errno set, not -errno.
  if (fuse_passthrough_close(*req_, backing_id) < 0) {
    return ErrnoToStatus(errno, "fuse_passthrough_close");
  }
  return absl::OkStatus();
}

const absl::StatusOr<ProcCredentials> &FuseRequest::CallerStatus(
    pid_t pid) const {
  if (!caller_status_.has_value()) caller_status_ = ReadProcCredentials(pid);
  return *caller_status_;
}

absl::StatusOr<Credentials> FuseRequest::Caller() const {
  RET_CHECK(req_.has_value()) << "FuseRequest already replied";
  const fuse_ctx *ctx = fuse_req_ctx(*req_);
  Credentials caller{
      .uid = ctx->uid, .gid = ctx->gid, .groups = {}, .umask = ctx->umask};
  // pid 0 (a caller outside dcfs's pid namespace) has no /proc entry, and
  // a caller that already exited has none any more.
  const absl::StatusOr<ProcCredentials> &proc = CallerStatus(ctx->pid);
  if (!proc.ok()) {
    // Rate limited: one per request while it lasts.
    LOG_EVERY_N_SEC(WARNING, 60)
        << "pid " << ctx->pid << ": supplementary groups unreadable ("
        << proc.status() << "); using none";
    return caller;
  }
  caller.groups = proc->groups;
  return caller;
}

namespace {

// Whether `proc`, the status of the thread that sent the request `ctx`
// describes, shows CAP_FSETID for that request's uid. /proc shows the
// task's objective credentials, and its capabilities in its own user
// namespace. They are the caller's only where they are the ones the
// request came with (a kernel caller acting with overridden credentials,
// nfsd for a client, shows its own fsuid there, not the request's), and
// count only in dcfs's user namespace, which is the init user namespace
// (main.cc refuses to start in any other), where
// setattr_should_drop_suidgid's capable(CAP_FSETID) asks for them
// (fs/attr.c:75 (Linux v7.3-rc7)).
absl::StatusOr<bool> CapFsetidShown(const ProcCredentials &proc,
                                    const fuse_ctx &ctx) {
  if (proc.fsuid != ctx.uid) return false;
  if (!proc.cap_effective.Has(CAP_FSETID)) return false;
  return SharesMyUserNamespace(ctx.pid);
}

}  // namespace

absl::StatusOr<bool> FuseRequest::CallerHasCapFsetid() const {
  RET_CHECK(req_.has_value()) << "FuseRequest already replied";
  // An answer of the kernel's own (fuse_ctx has no capabilities) is what
  // docs/plan/notes/kernel-patches.md row 4 asks for.
  const fuse_ctx *ctx = fuse_req_ctx(*req_);
  const absl::StatusOr<ProcCredentials> &proc = CallerStatus(ctx->pid);
  absl::StatusOr<bool> shown = proc.ok() ? CapFsetidShown(*proc, *ctx)
                                         : absl::StatusOr<bool>(proc.status());
  if (shown.ok()) return *shown;
  // Rate limited: one per request while it lasts.
  LOG_EVERY_N_SEC(WARNING, 60)
      << "pid " << ctx->pid << ": CAP_FSETID unverifiable (" << shown.status()
      << "); taken as absent";
  return false;
}

absl::Status FuseRequest::ReplyErrno(int errnum) {
  RET_CHECK(req_.has_value()) << "FuseRequest already replied";
  errno_sent_ = errnum;
  absl::Status st =
      ProducedErrnoToStatus(-fuse_reply_err(*req_, errnum), "fuse_reply_err");
  req_ = std::nullopt;
  return st;
}

absl::Status FuseRequest::ReplyFailure(const absl::Status &status) {
  RET_CHECK(!status.ok()) << "ReplyFailure called with an ok status";
  int errnum = StatusToErrno(status);
  // A non-ok status must never turn into a "success" (0) errno reply.
  if (errnum == 0) errnum = EIO;
  return ReplyErrno(errnum);
}

void FuseRequest::ReplyFailureAndLogIfNotOk(const absl::Status &status) {
  if (status.ok()) return;
  // The outermost caller of a request: the one place that logs its failure
  // (docs/style.md 1.7), with all the context the call chain added. At
  // ERROR if dcfs produced the error; an errno forwarded from the backing
  // filesystem (ENOENT, EEXIST, ...) is the answer to the request, not a
  // failure of dcfs, and an interrupted request (dcfs/interruption_point.h)
  // is no error either: `--v=2` shows the replies (Serve's LogRequest).
  if (ProducedByDcfs(status)) LOG(ERROR) << status;
  // A failed success reply used the request up (the status is the failure
  // of that reply, logged above): there is nothing left to reply to.
  if (!req_.has_value()) return;
  absl::Status reply_s = ReplyFailure(status);
  LOG_IF(ERROR, !reply_s.ok()) << "Failed to reply with failure: " << reply_s;
}

absl::Status FuseRequest::ReplyBuf(std::string_view buf) {
  RET_CHECK(req_.has_value()) << "FuseRequest already replied";
  absl::Status st = ProducedErrnoToStatus(
      -fuse_reply_buf(*req_, buf.data(), buf.size()), "fuse_reply_buf");
  req_ = std::nullopt;
  return st;
}

absl::Status FuseRequest::ReplyXattrSize(size_t size) {
  RET_CHECK(req_.has_value()) << "FuseRequest already replied";
  absl::Status st =
      ProducedErrnoToStatus(-fuse_reply_xattr(*req_, size), "fuse_reply_xattr");
  req_ = std::nullopt;
  return st;
}

absl::Status FuseRequest::ReplyDirs(std::span<FuseDirEntry> entries,
                                    size_t maxsize) {
  RET_CHECK(req_.has_value()) << "FuseRequest already replied";
  absl::FixedArray<char> buf(maxsize);
  size_t used = AppendDirEntries(*req_, buf.data(), buf.size(), entries);
  return ReplyBuf(std::string_view(buf.data(), used));
}

absl::Status FuseRequest::ReplyDirsPlus(std::span<FuseDirEntryPlus> entries,
                                        size_t maxsize) {
  RET_CHECK(req_.has_value()) << "FuseRequest already replied";
  absl::FixedArray<char> buf(maxsize);
  size_t used = AppendDirEntriesPlus(*req_, buf.data(), buf.size(), entries);
  return ReplyBuf(std::string_view(buf.data(), used));
}

absl::Status FuseRequest::ReplyStatfs(const struct statvfs &stbuf) {
  RET_CHECK(req_.has_value()) << "FuseRequest already replied";
  absl::Status st = ProducedErrnoToStatus(-fuse_reply_statfs(*req_, &stbuf),
                                          "fuse_reply_statfs");
  req_ = std::nullopt;
  return st;
}

void FuseRequest::ReplyNone() {
  if (!req_.has_value()) {
    LOG(ERROR) << "FuseRequest already replied: no second fuse_reply_none";
    return;
  }
  fuse_reply_none(*req_);
  req_ = std::nullopt;
}

}  // namespace dcfs
