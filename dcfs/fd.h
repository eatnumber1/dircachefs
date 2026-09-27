#ifndef DCFS_FD_H_
#define DCFS_FD_H_

#include "absl/status/status.h"

namespace dcfs {

class FileDescriptor {
 public:
  FileDescriptor();
  explicit FileDescriptor(int fd);
  ~FileDescriptor();
  int operator*() const;
  int Release() &&;
  bool valid() const;
  absl::Status Close() &&;

  // Moveable, but not copyable. Copying would require dup2
  FileDescriptor(FileDescriptor &&);
  FileDescriptor(const FileDescriptor &) = delete;
  FileDescriptor &operator=(FileDescriptor &&);
  FileDescriptor &operator=(const FileDescriptor &) = delete;

 private:
  int fd_ = -1;
};

}  // namespace dcfs

#endif  // DCFS_FD_H_
