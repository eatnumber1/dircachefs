#include "dfs/fd.h"

#include "dfs/syscalls.h"
#include "absl/log/log.h"

namespace dfs {

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

FileDescriptor::FileDescriptor(FileDescriptor &&o) : FileDescriptor() {
  *this = std::move(o);
}

FileDescriptor &FileDescriptor::operator=(FileDescriptor &&o) {
  using std::swap;
  swap(fd_, o.fd_);
  return *this;
}

}  // namespace dfs
