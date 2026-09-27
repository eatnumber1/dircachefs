#ifndef DCFS_FD_H_
#define DCFS_FD_H_

namespace dcfs {

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

}  // namespace dcfs

#endif  // DCFS_FD_H_
