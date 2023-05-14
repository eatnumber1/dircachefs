#ifndef DFS_FD_H_
#define DFS_FD_H_

#include "absl/status/status.h"

namespace dfs {

class FileDescriptor {
 public:
  FileDescriptor();
  FileDescriptor(int fd);
  ~FileDescriptor();
  int operator*() const;
  int Release() &&;

  // Moveable, but not copyable. Copying would require dup2
  FileDescriptor(FileDescriptor &&);
  FileDescriptor(const FileDescriptor &) = delete;
  FileDescriptor &operator=(FileDescriptor &&);
  FileDescriptor &operator=(const FileDescriptor &) = delete;

 private:
  int fd_ = -1;
};

}  // namespace dfs

#endif  // DFS_FD_H_
