#ifndef DCFS_FILE_HANDLE_H_
#define DCFS_FILE_HANDLE_H_

#include <fcntl.h>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <utility>

#include "absl/functional/function_ref.h"
#include "absl/status/statusor.h"
#include "dcfs/device_uuid.h"
#include "dcfs/fd.h"
#include "dcfs/memory.h"
#include "dcfs/mount_fd_cache.h"
#include "libmount.h"

namespace dcfs {

std::unique_ptr<file_handle, RawMemoryDeleter> CopyFileHandle(const file_handle &fh);

// A FileHandle is a durable reference to a single inode (file, directory, etc.)
// on a filesystem. It refers to the object in the filesystem, *not* the path,
// so if the file is renamed, or even in some cases deleted, FileHandle can
// still open the file.
//
// Note: In order to open a file handle (producing an fd), FileHandle needs a
// pre-existing fd pointing to the mounted filesystem that the
// file-object-to-be-opened resides on. This "mount fd" is lazily created
// automatically on the first Open call, then retained for the lifetime of the
// FileHandle instance.
class FileHandle {
 public:
  class Builder {
   public:
    // `fd_cache` is retained by both Builder and any FileHandles built by this
    // Builder. `mtab` and `cache` is retained only by the Builder class.
    Builder(
      // TODO mtab and cache needs to be made thread-safe.
      libmnt_table *absl_nonnull mtab,
      libmnt_cache *absl_nonnull cache,
      MountFDCache *absl_nonnull fd_cache);

    absl::StatusOr<FileHandle> MakeHandle(
        int dirfd, std::string_view path, int flags = 0);
    absl::StatusOr<FileHandle> MakeHandle(int fd, int flags = 0);

    absl::StatusOr<FileHandle> MakeHandle(
        std::unique_ptr<file_handle, RawMemoryDeleter> handle,
        DeviceUUID uuid);

   private:
    absl::StatusOr<std::reference_wrapper<libmnt_fs>> FindMount(
        int mount_id);
    absl::StatusOr<std::reference_wrapper<libmnt_fs>> FindMount(
        const DeviceUUID &uuid);

    absl::StatusOr<absl::AnyInvocable<absl::StatusOr<int>()>> CreateMountOpener(
        const libmnt_fs &fs, const DeviceUUID &uuid);

    absl::StatusOr<DeviceUUID> GetMountUUID(const libmnt_fs &fs);

    // Callback should return false when you want to stop iterating.
    absl::Status ForEachMount(
        absl::FunctionRef<absl::StatusOr<bool>(libmnt_fs &)> cb);

    libmnt_table &mtab_;
    libmnt_cache &cache_;
    MountFDCache &fd_cache_;
  };

  FileHandle() = default;

  FileHandle(FileHandle &&) = default;
  FileHandle(const FileHandle &);
  FileHandle &operator=(FileHandle &&) = default;
  FileHandle &operator=(const FileHandle &);

  const file_handle &GetHandle() const;
  const DeviceUUID &GetUUID() const;

  absl::StatusOr<FileDescriptor> Open(int flags = 0);

 private:
  FileHandle(
    std::unique_ptr<file_handle, RawMemoryDeleter> handle,
    DeviceUUID uuid,
    absl::AnyInvocable<absl::StatusOr<int>()> mount_opener);

  std::unique_ptr<file_handle, RawMemoryDeleter> handle_;
  DeviceUUID uuid_;
  absl::AnyInvocable<absl::StatusOr<int>()> mount_opener_;
};

}  // namespace dcfs

#endif  // DCFS_FILE_HANDLE_H_
