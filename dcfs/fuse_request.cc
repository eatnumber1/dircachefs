#include "dcfs/fuse_request.h"

#include <cerrno>
#include <cstdint>
#include <cstring>
#include <string_view>
#include <utility>
#include <vector>

#include "absl/log/check.h"
#include "absl/log/log.h"
#include "absl/status/status.h"
#include "absl/time/time.h"
#include "dcfs/ret_check.h"
#include "dcfs/status.h"

namespace dcfs {

size_t AppendDirEntries(
    fuse_req_t req, char *buf, size_t bufsize,
    std::span<const FuseDirEntry> entries) {
  size_t used = 0;
  for (const FuseDirEntry &entry : entries) {
    size_t remaining = bufsize - used;
    // fuse_add_direntry() returns the space this entry needs without
    // writing it if that exceeds `remaining` (it checks buf == nullptr ||
    // needed > bufsize internally), so a single call both measures and
    // writes each entry.
    size_t needed = fuse_add_direntry(
        req, buf == nullptr ? nullptr : buf + used, remaining,
        entry.name.c_str(), &entry.stbuf, entry.off);
    if (needed > remaining) break;
    used += needed;
  }
  return used;
}

size_t AppendDirEntriesPlus(
    fuse_req_t req, char *buf, size_t bufsize,
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

FuseRequest::FuseRequest(FuseRequest &&o)
    : FuseRequest() {
  *this = std::move(o);
}

FuseRequest &FuseRequest::operator=(FuseRequest &&o) {
  using std::swap;
  swap(req_, o.req_);
  return *this;
}

FuseRequest::~FuseRequest() {
  if (!req_) return;
  // Every op method is expected to reply before returning (see
  // ReplyFailureAndLogIfNotOk); reaching here means one didn't. FuseRequest
  // only wraps the raw req_, not which op or inode it was for, so that
  // context can't be logged here -- replying ECOMM at least keeps the
  // kernel from waiting on a request that will never get a normal reply.
  LOG(WARNING) << "Replying to FuseRequest in destructor";
  absl::Status st = ReplyErrno(ECOMM);
  LOG_IF(ERROR, !st.ok()) << "Failed to send reply: " << st;
}

absl::Status FuseRequest::ReplyEntryParam(const fuse_entry_param &param) {
  RET_CHECK(req_.has_value()) << "FuseRequest already replied";
  absl::Status st = dcfs::ErrnoToStatus(
      -fuse_reply_entry(*req_, &param), "fuse_reply_entry");
  req_ = std::nullopt;
  return st;
}

absl::Status FuseRequest::ReplyEntry(
    fuse_ino_t nodeid, uint64_t generation, const struct stat &attr,
    absl::Duration attr_timeout, absl::Duration entry_timeout) {
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

absl::Status FuseRequest::ReplyAttr(
    const struct stat &attr, absl::Duration attr_timeout) {
  RET_CHECK(req_.has_value()) << "FuseRequest already replied";
  absl::Status st = dcfs::ErrnoToStatus(
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
  absl::Status st = dcfs::ErrnoToStatus(
      -fuse_reply_readlink(*req_, target_str.c_str()), "fuse_reply_readlink");
  req_ = std::nullopt;
  return st;
}

absl::Status FuseRequest::ReplyOpen(const fuse_file_info &fi) {
  RET_CHECK(req_.has_value()) << "FuseRequest already replied";
  absl::Status st =
    dcfs::ErrnoToStatus(-fuse_reply_open(*req_, &fi), "fuse_reply_open");
  req_ = std::nullopt;
  return st;
}

absl::Status FuseRequest::ReplyCreate(
    const fuse_entry_param &entry, const fuse_file_info &fi) {
  RET_CHECK(req_.has_value()) << "FuseRequest already replied";
  absl::Status st = dcfs::ErrnoToStatus(
      -fuse_reply_create(*req_, &entry, &fi), "fuse_reply_create");
  req_ = std::nullopt;
  return st;
}

absl::Status FuseRequest::ReplyWrite(size_t count) {
  RET_CHECK(req_.has_value()) << "FuseRequest already replied";
  absl::Status st =
    dcfs::ErrnoToStatus(-fuse_reply_write(*req_, count), "fuse_reply_write");
  req_ = std::nullopt;
  return st;
}

absl::Status FuseRequest::ReplyIoctl(int result, std::string_view buf) {
  RET_CHECK(req_.has_value()) << "FuseRequest already replied";
  absl::Status st = dcfs::ErrnoToStatus(
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
    return dcfs::ErrnoToStatus(errno, "fuse_passthrough_close");
  }
  return absl::OkStatus();
}

absl::StatusOr<Credentials> FuseRequest::Caller() const {
  RET_CHECK(req_.has_value()) << "FuseRequest already replied";
  const fuse_ctx *ctx = fuse_req_ctx(*req_);
  Credentials caller{
      .uid = ctx->uid, .gid = ctx->gid, .groups = {}, .umask = ctx->umask};
  // Usually a handful of groups; fuse_req_getgroups returns the full count
  // even when it exceeds the buffer, so one retry at the right size
  // suffices (NGROUPS_MAX is 65536).
  std::vector<gid_t> groups(32);
  int n = fuse_req_getgroups(*req_, groups.size(), groups.data());
  if (n > static_cast<int>(groups.size())) {
    groups.resize(n);
    n = fuse_req_getgroups(*req_, groups.size(), groups.data());
  }
  if (n < 0 || n > static_cast<int>(groups.size())) {
    VLOG(1) << "pid " << ctx->pid << ": supplementary groups unreadable ("
            << (n < 0 ? ErrnoToStatus(-n, "fuse_req_getgroups").ToString()
                      : std::string("list grew"))
            << "); using none";
    return caller;
  }
  groups.resize(n);
  caller.groups = std::move(groups);
  return caller;
}

absl::Status FuseRequest::ReplyErrno(int errnum) {
  RET_CHECK(req_.has_value()) << "FuseRequest already replied";
  absl::Status st =
    dcfs::ErrnoToStatus(-fuse_reply_err(*req_, errnum), "fuse_reply_err");
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
  // An interrupted request (dcfs/checkpoint.h) is no error of dcfs's.
  if (StatusToErrno(status) == EINTR) {
    LOG(INFO) << status;
  } else {
    LOG(ERROR) << status;
  }
  absl::Status reply_s = ReplyFailure(status);
  LOG_IF(WARNING, !reply_s.ok()) << "Failed to reply with failure: " << reply_s;
}

absl::Status FuseRequest::ReplyBuf(std::string_view buf) {
  RET_CHECK(req_.has_value()) << "FuseRequest already replied";
  absl::Status st =
    dcfs::ErrnoToStatus(
        -fuse_reply_buf(*req_, buf.data(), buf.size()),
        "fuse_reply_buf");
  req_ = std::nullopt;
  return st;
}

absl::Status FuseRequest::ReplyXattrSize(size_t size) {
  RET_CHECK(req_.has_value()) << "FuseRequest already replied";
  absl::Status st =
    dcfs::ErrnoToStatus(-fuse_reply_xattr(*req_, size), "fuse_reply_xattr");
  req_ = std::nullopt;
  return st;
}

absl::Status FuseRequest::ReplyDirs(
    std::span<FuseDirEntry> entries, size_t maxsize) {
  RET_CHECK(req_.has_value()) << "FuseRequest already replied";
  std::vector<char> buf(maxsize);
  size_t used = AppendDirEntries(*req_, buf.data(), buf.size(), entries);
  return ReplyBuf(std::string_view(buf.data(), used));
}

absl::Status FuseRequest::ReplyDirsPlus(
    std::span<FuseDirEntryPlus> entries, size_t maxsize) {
  RET_CHECK(req_.has_value()) << "FuseRequest already replied";
  std::vector<char> buf(maxsize);
  size_t used = AppendDirEntriesPlus(*req_, buf.data(), buf.size(), entries);
  return ReplyBuf(std::string_view(buf.data(), used));
}

absl::Status FuseRequest::ReplyStatfs(const struct statvfs &stbuf) {
  RET_CHECK(req_.has_value()) << "FuseRequest already replied";
  absl::Status st =
    dcfs::ErrnoToStatus(
        -fuse_reply_statfs(*req_, &stbuf),
        "fuse_reply_statfs");
  req_ = std::nullopt;
  return st;
}

void FuseRequest::ReplyNone() {
  // ReplyNone() returns void (there is nothing to report to the caller, and
  // no reply function call that could itself fail), so unlike the other
  // Reply* methods this uses CHECK rather than RET_CHECK to enforce the
  // single-reply invariant.
  CHECK(req_.has_value()) << "FuseRequest already replied";
  fuse_reply_none(*req_);
  req_ = std::nullopt;
}

}  // namespace dcfs
