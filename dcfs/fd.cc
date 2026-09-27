#include "dcfs/fd.h"

#include "dcfs/syscalls.h"
#include "absl/log/log.h"

namespace dcfs {

FileDescriptor::FileDescriptor() = default;

FileDescriptor::FileDescriptor(int fd) : fd_(fd) {}

FileDescriptor::~FileDescriptor() {
  if (fd_ == -1) return;
  // Note that a failure to close is not recoverable. We must leak the fd.
  if (absl::Status st = syscalls::close(std::move(*this));
      !st.ok()) {
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

absl::Status FileDescriptor::Close() && {
  return syscalls::close(std::move(*this));
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
