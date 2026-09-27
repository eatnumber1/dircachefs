#include "dcfs/fuse.h"

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <fcntl.h>
#include <iostream>
#include <unistd.h>
#include <utility>

#include "absl/log/check.h"
#include "absl/log/log.h"
#include "absl/time/time.h"
#include "absl/status/status.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/str_format.h"
#include "absl/strings/str_join.h"
#include "dcfs/syscalls.h"

namespace dcfs {

std::vector<char> FuseDirEntry::GetDirEntryBuffer(
    fuse_req_t req, std::span<FuseDirEntry> entries) {
  size_t bufsiz = 0;
  for (const FuseDirEntry &entry : entries) {
    // TODO rewrite this to have an initial buffer size
    bufsiz += fuse_add_direntry(
        req, /*buf=*/nullptr, /*bufsize=*/0, entry.name.c_str(), &entry.stbuf,
        entry.off);
  }

  std::vector<char> bufvec;
  bufvec.resize(bufsiz);
  char *buf = bufvec.data();
  for (const FuseDirEntry &entry : entries) {
    size_t adv = fuse_add_direntry(
        req, buf, bufsiz, entry.name.c_str(), &entry.stbuf, entry.off);
    CHECK_LE(adv, bufsiz);
    buf += adv;
    bufsiz -= adv;
  }
  return bufvec;
}

FuseRequest::FuseRequest(fuse_req_t req) : req_(std::move(req)) {}

fuse_req_t &FuseRequest::operator*() { return *req_; }
fuse_req_t &FuseRequest::Get() { return *req_; }

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
  // TODO imporve this warning
  LOG(WARNING) << "Replying to FuseRequest in destructor";
  absl::Status st = ReplyErrno(ECOMM);
  LOG_IF(ERROR, !st.ok()) << "Failed to send reply: " << st;
}

absl::Status FuseRequest::ReplyAttr(
    const struct stat &attr, absl::Duration attr_timeout) {
  if (!req_) return absl::OkStatus();
  absl::Status st = absl::ErrnoToStatus(
      -fuse_reply_attr(*req_, &attr, absl::ToDoubleSeconds(attr_timeout)),
      "fuse_reply_attr");
  req_ = std::nullopt;
  return st;
}

absl::Status FuseRequest::ReplyOpen(const fuse_file_info &fi) {
  if (!req_) return absl::OkStatus();
  absl::Status st =
    absl::ErrnoToStatus(-fuse_reply_open(*req_, &fi), "fuse_reply_open");
  req_ = std::nullopt;
  return st;
}

absl::Status FuseRequest::ReplyErrno(int errnum) {
  if (!req_) return absl::OkStatus();
  absl::Status st =
    absl::ErrnoToStatus(-fuse_reply_err(*req_, errnum), "fuse_reply_err");
  req_ = std::nullopt;
  return st;
}

absl::Status FuseRequest::ReplyFailure(const absl::Status &status) {
  CHECK(!status.ok()) << status;
  if (!req_) return absl::OkStatus();
  absl::Status st = ReplyErrno(StatusToErrno(status));
  req_ = std::nullopt;
  return st;
}

void FuseRequest::ReplyFailureAndLogIfNotOk(const absl::Status &status) {
  if (status.ok()) return;
  LOG(ERROR) << status;
  absl::Status reply_s = ReplyFailure(status);
  LOG_IF(WARNING, !reply_s.ok()) << "Failed to reply with failure: " << reply_s;
}

void FuseRequest::ReplyAlwaysAndLogIfNotOk(const absl::Status &status) {
  LOG_IF(ERROR, !status.ok()) << status;
  absl::Status reply_s = ReplyErrno(StatusToErrno(status));
  LOG_IF(WARNING, !reply_s.ok()) << "Failed to reply with failure: " << reply_s;
}

absl::Status FuseRequest::ReplyBuf(std::string_view buf) {
  if (!req_) return absl::OkStatus();
  absl::Status st =
    absl::ErrnoToStatus(
        -fuse_reply_buf(*req_, buf.data(), buf.size()),
        "fuse_reply_buf");
  req_ = std::nullopt;
  return st;
}

absl::Status FuseRequest::ReplyDirs(
    std::span<FuseDirEntry> entries, size_t maxsize) {
  std::vector<char> buf = FuseDirEntry::GetDirEntryBuffer(*req_, entries);
  return ReplyBuf({buf.data(), std::min(buf.size(), maxsize)});
}

absl::Status FuseRequest::ReplyEntry(
    fuse_ino_t ino, uint64_t generation, struct stat attr,
    absl::Duration attr_timeout, absl::Duration entry_timeout) {
  fuse_entry_param param {
    .ino = ino,
    .generation = generation,
    .attr = attr,
    .attr_timeout = absl::ToDoubleSeconds(attr_timeout),
    .entry_timeout = absl::ToDoubleSeconds(entry_timeout),
  };
  absl::Status st =
    absl::ErrnoToStatus(
        -fuse_reply_entry(*req_, &param),
        "fuse_reply_entry");
  req_ = std::nullopt;
  return st;
}

absl::Status FuseRequest::ReplyStatfs(const struct statvfs &stbuf) {
  if (!req_) return absl::OkStatus();
  absl::Status st =
    absl::ErrnoToStatus(
        -fuse_reply_statfs(*req_, &stbuf),
        "fuse_reply_statfs");
  req_ = std::nullopt;
  return st;
}

void FuseRequest::ReplyNone() {
  if (!req_) return;
  fuse_reply_none(*req_);
  req_ = std::nullopt;
}

LogFuseFileInfo::LogFuseFileInfo(fuse_file_info *fi) : fi_(fi) {}
LogFuseFileInfo::LogFuseFileInfo(fuse_file_info &fi) : LogFuseFileInfo(&fi) {}

}  // namespace dcfs
