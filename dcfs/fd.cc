#include "dcfs/fd.h"

#include <utility>

#include "absl/log/log.h"
#include "absl/status/status.h"
#include "dcfs/syscalls.h"

namespace dcfs {

FileDescriptor::FileDescriptor() = default;

FileDescriptor::FileDescriptor(int fd) : fd_(fd) {}

FileDescriptor::~FileDescriptor() {
  if (fd_ == -1) return;
  // Note that a failure to close is not recoverable. We must leak the fd.
  if (absl::Status st = syscalls::close(std::move(*this)); !st.ok()) {
    LOG(ERROR) << st;
  }
}

int FileDescriptor::operator*() const { return fd_; }

int FileDescriptor::Release() && {
  int fd = fd_;
  fd_ = -1;
  return fd;
}

bool FileDescriptor::valid() const { return fd_ != -1; }

absl::Status FileDescriptor::Close() {
  if (fd_ == -1) {
    return absl::OkStatus();  // Already closed
  }
  // Move the fd to a temporary to avoid using fd_ after close
  FileDescriptor tmp(std::move(*this));
  return syscalls::close(std::move(tmp));
}

FileDescriptor::FileDescriptor(FileDescriptor &&o) : FileDescriptor() {
  *this = std::move(o);
}

FileDescriptor &FileDescriptor::operator=(FileDescriptor &&o) {
  using std::swap;
  swap(fd_, o.fd_);
  return *this;
}

}  // namespace dcfs
