#include "dcfs/file_handle.h"

#include <cerrno>
#include <fcntl.h>
#include <functional>
#include <memory>
#include <stdint.h>
#include <string>
#include <string_view>
#include <utility>

#include "absl/cleanup/cleanup.h"
#include "absl/functional/function_ref.h"
#include "absl/log/die_if_null.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/str_cat.h"
#include "dcfs/device_uuid.h"
#include "dcfs/fd.h"
#include "dcfs/mount_fd_cache.h"
#include "dcfs/ret_check.h"
#include "dcfs/status.h"
#include "dcfs/syscalls.h"
#include "dcfs/memory.h"
#include "libmount.h"

namespace dcfs {
namespace {

absl::StatusOr<unsigned int> GetHandleSize(
    int dirfd, std::string_view path, int flags) {
  int unused = 0;
  file_handle handle { .handle_bytes = 0 };
  absl::Status status = syscalls::name_to_handle_at(
      dirfd, path, handle, unused, flags);
  if (status.ok()) {
    return absl::UnknownError(
        "name_to_handle_at failed to error when handle_bytes = 0");
  }
  ASSIGN_OR_RETURN(int eno, GetErrnoFromStatus(status));
  // Some care is needed here as EOVERFLOW can also indicate that no file
  // handle is available for this particular name in a filesystem which does
  // normally support file-handle lookup. This case can be detected when the
  // EOVERFLOW error is returned without handle_bytes being increased.
  if (eno != EOVERFLOW || handle.handle_bytes == 0) return status;
  return handle.handle_bytes;
}

std::unique_ptr<file_handle, RawMemoryDeleter> NewFileHandle(
    unsigned int handle_bytes) {
  return std::unique_ptr<file_handle, RawMemoryDeleter>(
        static_cast<file_handle *>(
          ::operator new (sizeof(file_handle) + handle_bytes)),
        RawMemoryDeleter());
}

absl::StatusOr<
  std::pair</*mount_id=*/int, std::unique_ptr<file_handle, RawMemoryDeleter>>>
  NameToHandleAndMountID(
    int dirfd, std::string_view path, int flags) {
  ASSIGN_OR_RETURN(
      unsigned int handle_bytes, GetHandleSize(dirfd, path, flags));

  std::unique_ptr<file_handle, RawMemoryDeleter> handle =
    NewFileHandle(handle_bytes);
  handle->handle_bytes = handle_bytes;

  int mount_id = 0;
  RETURN_IF_ERROR(
      syscalls::name_to_handle_at(dirfd, path, *handle, mount_id, flags));

  return std::make_pair(mount_id, std::move(handle));
}

class MountOpener {
 public:
  MountOpener(
      std::string mount_target,
      MountFDCache *absl_nonnull fd_cache,
      DeviceUUID uuid)
    : variant_(
        std::move(mount_target), std::move(uuid),
        *ABSL_DIE_IF_NULL(fd_cache))
  {}

  absl::StatusOr<int> operator() {
    return std::visit(
      [this](std::shared_ptr<const FileDescriptor> &fd) -> absl::StatusOr<int> {
        return **fd
      },
      [this](std::tuple<std::string, DeviceUUID, std::reference_wrapper<MountFDCache>>> &t) -> absl::StatusOr<int> {
        std::string_view mount_target = std::get<0>(t);
        const DeviceUUID &uuid = std::get<1>(t);
        MountFDCache &cache = std::get<2>(t);
        ASSIGN_OR_RETURN(
            std::shared_ptr<const FileDescriptor> fd,
            fd_cache.Get(uuid, mount_target));
        int ret = **fd;
        variant_ = std::move(fd);
      }
    );
  }

 private:
  std::variant<
    absl_nonnull std::shared_ptr<const FileDescriptor>,
    std::tuple<
      std::string, DeviceUUID, std::reference_wrapper<MountFDCache>>> variant_;
};

}  // namespace

std::unique_ptr<file_handle, RawMemoryDeleter> CopyFileHandle(
    const file_handle &fh) {
  std::unique_ptr<file_handle, RawMemoryDeleter> handle =
    NewFileHandle(fh.handle_bytes);
  std::memcpy(handle.get(), &fh, sizeof(file_handle) + fh.handle_bytes);
  return handle;
}

FileHandle::Builder::Builder(
  libmnt_table *mtab,
  libmnt_cache *cache,
  MountFDCache *fd_cache)
  : mtab_(*ABSL_DIE_IF_NULL(mtab)),
    cache_(*ABSL_DIE_IF_NULL(cache)),
    fd_cache_(*ABSL_DIE_IF_NULL(fd_cache))
{}

absl::Status FileHandle::Builder::ForEachMount(
    absl::FunctionRef<absl::StatusOr<bool>(libmnt_fs &)> cb) {
  libmnt_iter *iter = mnt_new_iter(MNT_ITER_FORWARD);
  if (iter == nullptr) return absl::UnknownError("mnt_new_iter");
  absl::Cleanup dealloc_iter([&]() { mnt_free_iter(iter); });

  libmnt_fs *fs = nullptr;
  while (mnt_table_next_fs(&mtab_, iter, &fs) == 0) {
    RET_CHECK_NE(fs, nullptr);
    ASSIGN_OR_RETURN(bool cont, cb(*fs));
    if (!cont) break;
  }

  return absl::OkStatus();
}

absl::StatusOr<std::reference_wrapper<libmnt_fs>>
FileHandle::Builder::FindMount(int mount_id) {
  libmnt_fs *ret = nullptr;
  RETURN_IF_ERROR(
      ForEachMount([&](libmnt_fs &fs) -> absl::StatusOr<bool> {
        if (mnt_fs_get_id(&fs) != mount_id) return true;
        ret = &fs;
        return false;
      }));
  if (ret == nullptr) {
    return absl::NotFoundError(
        absl::StrCat("Could not find mount with id ", mount_id));
  }
  return *ret;
}

absl::StatusOr<std::reference_wrapper<libmnt_fs>>
FileHandle::Builder::FindMount(const DeviceUUID &uuid) {
  libmnt_fs *ret = nullptr;
  RETURN_IF_ERROR(
      ForEachMount([&](libmnt_fs &fs) -> absl::StatusOr<bool> {
        if (!mnt_fs_match_source(
              &fs, absl::StrCat("UUID=", uuid.value).c_str(), &cache_)) {
          return true;
        }
        ret = &fs;
        return false;
      }));
  if (ret == nullptr) {
    return absl::NotFoundError(
        absl::StrCat(
          "Could not find mount for device with UUID ", uuid.value));
  }
  return *ret;
}

absl::StatusOr<absl::AnyInvocable<absl::StatusOr<int>()>>
FileHandle::Builder::CreateMountOpener(const libmnt_fs &fs, DeviceUUID uuid) {
  const char *target = mnt_fs_get_target(const_cast<libmnt_fs *>(&fs));
  if (target == nullptr) {
    return absl::UnknownError(
        absl::StrCat(
          "Unknown mount target for mount with device uuid ", uuid));
  }

  return MountOpener(std::string(target), std::move(uuid), &fd_cache_);
}

absl::StatusOr<DeviceUUID> FileHandle::Builder::GetMountUUID(
    const libmnt_fs &fs) {
  const char *tag;
  const char *val;
  auto *mfs = const_cast<libmnt_fs *>(&fs);
  if (mnt_fs_get_tag(mfs, &tag, &val) == 0 &&
      std::string_view(tag) == "UUID") {
    return DeviceUUID(val);
  }

  const char *src = mnt_fs_get_source(mfs);
  if (src == nullptr) {
    return absl::UnknownError(
        absl::StrCat("Unknown mount source for mount"));
  }

  char *canon_src = mnt_resolve_spec(src, &cache_);
  if (canon_src == nullptr) {
    return absl::UnknownError(
        absl::StrCat(
          "Could not resolve spec for mount with source ", src));
  }
  if (mnt_cache_read_tags(&cache_, canon_src) < 0) {
    return absl::UnknownError(
        absl::StrCat(
          "Could not read tags for mount with source ", canon_src));
  }
  char *uuid = mnt_cache_find_tag_value(&cache_, canon_src, "UUID");
  if (uuid == nullptr) {
    return absl::UnknownError(
        absl::StrCat(
          "Could not read UUID tag for mount with source ", canon_src));
  }
  return DeviceUUID{uuid};
}

absl::StatusOr<FileHandle> FileHandle::Builder::MakeHandle(
    int dirfd, std::string_view path, int flags) {
  ASSIGN_OR_RETURN(
      auto mhp, NameToHandleAndMountID(dirfd, path, flags));
  int mount_id = mhp.first;
  std::unique_ptr<file_handle, RawMemoryDeleter> handle = std::move(mhp.second);

  // There's a race here in that the mount_id can be reused before we figure out
  // the UUID and open the mountpoint. This isn't fixable without changing
  // name_to_handle_at to return an mount_id that's not reused (statx supports
  // this, but name_to_handle_at does not).
  ASSIGN_OR_RETURN(libmnt_fs &fs, FindMount(mount_id));
  ASSIGN_OR_RETURN(DeviceUUID uuid, GetMountUUID(fs));
  ASSIGN_OR_RETURN(
      absl::AnyInvocable<absl::StatusOr<int>()> mount_opener, CreateMountOpener(fs, uuid));

  return FileHandle(std::move(handle), std::move(uuid), std::move(mount_opener));
}

absl::StatusOr<FileHandle> FileHandle::Builder::MakeHandle(int fd, int flags) {
  return MakeHandle(fd, /*path=*/"", flags | AT_EMPTY_PATH);
}

absl::StatusOr<FileHandle> FileHandle::Builder::MakeHandle(
    std::unique_ptr<file_handle, RawMemoryDeleter> handle,
    DeviceUUID uuid) {
  ASSIGN_OR_RETURN(libmnt_fs &fs, FindMount(uuid));
  ASSIGN_OR_RETURN(
      absl::AnyInvocable<absl::StatusOr<int>()> mount_opener, CreateMountOpener(fs, uuid));
  return FileHandle(
      std::move(handle), std::move(uuid), std::move(mount_opener));
}

FileHandle::FileHandle(
  std::unique_ptr<file_handle, RawMemoryDeleter> handle,
  DeviceUUID uuid,
  absl::AnyInvocable<absl::StatusOr<int>()> mount_opener)
  : handle_(std::move(handle)),
    uuid_(std::move(uuid)),
    mount_opener_(std::move(mount_opener))
{}

const file_handle &FileHandle::GetHandle() const {
  return *handle_;
}

const DeviceUUID &FileHandle::GetUUID() const {
  return uuid_;
}

absl::StatusOr<FileDescriptor> FileHandle::Open(int flags) {
  return syscalls::open_by_handle_at(**mount_fd_, *handle_, flags);
}

FileHandle::FileHandle(const FileHandle &o) : FileHandle() {
  *this = std::move(o);
}

FileHandle &FileHandle::operator=(const FileHandle &o) {
  if (o.handle_ != nullptr) {
    handle_ = CopyFileHandle(*o.handle_);
  }
  mount_fd_ = o.mount_fd_;
  uuid_ = o.uuid_;
  return *this;
}

}  // namespace dcfs
