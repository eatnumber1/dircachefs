#include "dcfs/backing.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/statfs.h>

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "absl/container/flat_hash_set.h"
#include "absl/log/log.h"
#include "absl/status/status.h"
#include "absl/status/status_macros.h"
#include "absl/status/statusor.h"
#include "absl/strings/str_cat.h"
#include "dcfs/context.h"
#include "dcfs/device_id.h"
#include "dcfs/fd.h"
#include "dcfs/file_handle.h"
#include "dcfs/metadata_cache.h"
#include "dcfs/migrate.h"
#include "dcfs/ret_check.h"
#include "dcfs/status.h"
#include "dcfs/syscalls.h"

namespace dcfs::backing {
namespace {

// Everything the cache stores about an inode's attributes.
constexpr unsigned int kAttrMask = STATX_BASIC_STATS | STATX_BTIME;
// Both are requested because a kernel that predates STATX_MNT_ID_UNIQUE
// (Linux 6.8) still answers STATX_MNT_ID; see MountId().
constexpr unsigned int kMountIdMask = STATX_MNT_ID_UNIQUE | STATX_MNT_ID;

// getdents64 buffer size: large enough that a typical directory is read in
// one or two calls.
constexpr size_t kDirentBufferBytes = 64 * 1024;

int ErrnoOf(const absl::Status &status) {
  return GetErrnoFromStatus(status).value_or(0);
}

// The mount id statx reported, or nullopt if it reported none. The unique
// id and the plain one share stx_mnt_id; the mask says which it is, and
// either serves to compare two objects seen by the same kernel.
std::optional<uint64_t> MountId(const struct statx &stx) {
  if ((stx.stx_mask & kMountIdMask) == 0) return std::nullopt;
  return stx.stx_mnt_id;
}

// Whether `child`, an entry of directory `parent`, is the root of a
// different filesystem than `parent`'s. Only a directory can be. A mount
// point has a different mount id; a Btrfs subvolume is not a mount (same
// mount id) but has its own st_dev, so both are compared.
bool IsBoundary(const struct statx &parent, const struct statx &child) {
  if (!S_ISDIR(child.stx_mode)) return false;
  if (child.stx_dev_major != parent.stx_dev_major ||
      child.stx_dev_minor != parent.stx_dev_minor) {
    return true;
  }
  std::optional<uint64_t> parent_mnt = MountId(parent);
  std::optional<uint64_t> child_mnt = MountId(child);
  return parent_mnt.has_value() && child_mnt.has_value() &&
         *parent_mnt != *child_mnt;
}

// The root is reached through the source mount fd, which InitRoot made an
// fd on the root itself: that needs no handle decoding, and since the fd
// pins the object it cannot have been replaced, so there is nothing to
// verify.
absl::StatusOr<FileDescriptor> OpenRoot(Context &ctx, int flags) {
  ABSL_ASSIGN_OR_RETURN(DeviceId source, GetSourceDeviceId(ctx.db));
  ABSL_ASSIGN_OR_RETURN(int mount_fd, ctx.mounts.Get(source));
  return syscalls::openat(mount_fd, ".", flags);
}

// Forgets `id` after the backing layer found it gone. Already being gone
// from the cache is fine: this is only ever cleanup on an error path.
absl::Status ForgetStale(Context &ctx, InodeId id) {
  absl::Status status = cache::InvalidateInode(ctx, id);
  if (absl::IsNotFound(status)) return absl::OkStatus();
  return status;
}

// All xattrs of the object `fd` (any fd, including O_PATH on a symlink or
// device) refers to. Reads through /proc/self/fd rather than reopening the
// object, so it works for every file type, never blocks on a FIFO or
// touches a device, and needs no read permission on the object.
absl::StatusOr<std::vector<std::pair<std::string, std::string>>> XattrsOf(
    int fd) {
  std::vector<std::pair<std::string, std::string>> xattrs;
  absl::StatusOr<std::vector<std::string>> names =
      syscalls::listxattr_opath(fd);
  if (!names.ok()) {
    int err = ErrnoOf(names.status());
    if (err == ENOTSUP || err == EOPNOTSUPP) return xattrs;
    return names.status();
  }
  for (std::string &name : *names) {
    absl::StatusOr<std::string> value = syscalls::getxattr_opath(fd, name);
    if (!value.ok()) {
      // Removed between listing and reading it: it no longer exists.
      if (ErrnoOf(value.status()) == ENODATA) continue;
      return value.status();
    }
    xattrs.emplace_back(std::move(name), *std::move(value));
  }
  return xattrs;
}

// The names in directory `dir_fd` (which must be at offset 0), without "."
// and "..".
absl::StatusOr<std::vector<std::string>> ReadDirNames(int dir_fd) {
  // uint64_t elements keep the buffer 8-byte aligned, which the kernel's
  // linux_dirent64 records (and so the casts below) rely on.
  std::vector<uint64_t> buf(kDirentBufferBytes / sizeof(uint64_t));
  std::vector<std::string> names;
  while (true) {
    ABSL_ASSIGN_OR_RETURN(
        ssize_t nbytes,
        syscalls::getdents64(dir_fd, buf.data(), buf.size() * sizeof(uint64_t)));
    if (nbytes == 0) return names;
    const char *base = reinterpret_cast<const char *>(buf.data());
    for (ssize_t offset = 0; offset < nbytes;) {
      const auto *entry =
          reinterpret_cast<const syscalls::linux_dirent64 *>(base + offset);
      std::string_view name(entry->d_name);
      if (name != "." && name != "..") names.emplace_back(name);
      offset += entry->d_reclen;
    }
  }
}

// What PopulateDirectory learns about one child in its I/O phase.
struct ChildRecord {
  std::string name;
  FileHandle handle;
  struct statx stx {};
  uint64_t backing_gen = 0;
  std::optional<std::string> symlink_target;
  std::vector<std::pair<std::string, std::string>> xattrs;
};

// A filesystem found mounted on a child, to register if it is new.
struct FoundFilesystem {
  DeviceId device;
  int64_t fstype = 0;
  std::string boundary_name;
};

// Reads everything the cache stores about child `name` of `dir_fd`.
// nullopt if the child vanished before it could be opened. A child that is
// the root of another filesystem gets that filesystem's mount fd
// registered and is appended to `filesystems`.
absl::StatusOr<std::optional<ChildRecord>> ProbeChild(
    Context &ctx, int dir_fd, const struct statx &dir_stx,
    const DeviceId &dir_device, std::string_view name,
    std::vector<FoundFilesystem> &filesystems) {
  // Everything below reads through this one fd, so all of it describes the
  // same object even if `name` is replaced meanwhile.
  absl::StatusOr<FileDescriptor> child =
      syscalls::openat(dir_fd, name, O_PATH | O_NOFOLLOW);
  if (!child.ok()) {
    if (ErrnoOf(child.status()) == ENOENT) {
      VLOG(1) << "child " << name << " vanished while listing its directory";
      return std::nullopt;
    }
    return child.status();
  }
  ChildRecord record;
  record.name = std::string(name);
  ABSL_ASSIGN_OR_RETURN(record.stx,
                        syscalls::statx(**child, "", AT_EMPTY_PATH,
                                        kAttrMask | kMountIdMask));

  DeviceId device = dir_device;
  if (IsBoundary(dir_stx, record.stx)) {
    ABSL_ASSIGN_OR_RETURN(device, ctx.device_id_fn(**child));
    if (absl::IsNotFound(ctx.mounts.Get(device).status())) {
      // The mount fd must be a real (non-O_PATH) descriptor because
      // open_by_handle_at resolves it with the non-raw fd class
      // (fs/fhandle.c get_path_from_fd). Reopen the very object we just
      // probed through /proc/self/fd rather than looking `name` up again,
      // so a rename racing with us cannot swap in a different directory.
      ABSL_ASSIGN_OR_RETURN(
          FileDescriptor mount_fd,
          syscalls::ReopenPathFd(**child, O_RDONLY | O_DIRECTORY));
      ABSL_RETURN_IF_ERROR(ctx.mounts.Insert(device, std::move(mount_fd)));
    }
    ABSL_ASSIGN_OR_RETURN(struct statfs sfs, syscalls::fstatfs(**child));
    filesystems.push_back({.device = device,
                           .fstype = static_cast<int64_t>(sfs.f_type),
                           .boundary_name = record.name});
  }

  ABSL_ASSIGN_OR_RETURN(record.handle, FileHandle::FromFd(**child, device));
  ABSL_ASSIGN_OR_RETURN(record.backing_gen,
                        ReadGeneration(**child, record.stx.stx_mode));
  if (S_ISLNK(record.stx.stx_mode)) {
    ABSL_ASSIGN_OR_RETURN(record.symlink_target,
                          syscalls::readlinkat(**child, ""));
  }
  ABSL_ASSIGN_OR_RETURN(record.xattrs, XattrsOf(**child));
  return record;
}

// Why filesystem `fs` should be forgotten, or nullopt if it is still
// mounted where it was found (in which case its mount fd is registered).
absl::StatusOr<std::optional<std::string>> CheckFilesystem(
    Context &ctx, const cache::FilesystemRow &fs) {
  absl::StatusOr<FileDescriptor> parent =
      OpenNode(ctx, *fs.parent_inode, O_PATH | O_DIRECTORY);
  if (!parent.ok()) {
    return absl::StrCat("cannot open its mount point's directory: ",
                        parent.status().ToString());
  }
  // A real (non-O_PATH) fd: `child` is inserted below as the mount fd for
  // `fs.device`, and open_by_handle_at's mount fd argument rejects O_PATH
  // (see ProbeChild's comment on the same restriction).
  absl::StatusOr<FileDescriptor> child = syscalls::openat(
      **parent, *fs.boundary_name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW);
  if (!child.ok()) {
    return absl::StrCat("cannot open its mount point: ",
                        child.status().ToString());
  }
  ABSL_ASSIGN_OR_RETURN(struct statx parent_stx,
                        syscalls::statx(**parent, "", AT_EMPTY_PATH,
                                        kMountIdMask));
  ABSL_ASSIGN_OR_RETURN(struct statx child_stx,
                        syscalls::statx(**child, "", AT_EMPTY_PATH,
                                        STATX_TYPE | kMountIdMask));
  if (!IsBoundary(parent_stx, child_stx)) {
    return std::string("nothing is mounted on its mount point any more");
  }
  absl::StatusOr<DeviceId> device = ctx.device_id_fn(**child);
  if (!device.ok()) {
    return absl::StrCat("cannot identify what is mounted there now: ",
                        device.status().ToString());
  }
  if (*device != fs.device) {
    return absl::StrCat("its mount point now holds ", device->ToString());
  }
  absl::Status inserted = ctx.mounts.Insert(fs.device, *std::move(child));
  if (!inserted.ok() && !absl::IsAlreadyExists(inserted)) return inserted;
  return std::nullopt;
}

struct RootProbe {
  RootIdentity identity;
  struct statx stx {};
};

absl::StatusOr<RootProbe> Probe(Context &ctx, int source_fd) {
  RootProbe probe;
  ABSL_ASSIGN_OR_RETURN(probe.identity.device_id, ctx.device_id_fn(source_fd));
  ABSL_ASSIGN_OR_RETURN(struct statfs sfs, syscalls::fstatfs(source_fd));
  probe.identity.fstype = static_cast<int64_t>(sfs.f_type);
  ABSL_ASSIGN_OR_RETURN(probe.stx,
                        syscalls::statx(source_fd, "", AT_EMPTY_PATH, kAttrMask));
  if (!S_ISDIR(probe.stx.stx_mode)) {
    return dcfs::ErrnoToStatus(ENOTDIR, "the source is not a directory");
  }
  probe.identity.backing_ino = probe.stx.stx_ino;
  ABSL_ASSIGN_OR_RETURN(probe.identity.backing_gen,
                        ReadGeneration(source_fd, probe.stx.stx_mode));
  return probe;
}

}  // namespace

absl::StatusOr<RootIdentity> ProbeRoot(Context &ctx, int source_fd) {
  ABSL_ASSIGN_OR_RETURN(RootProbe probe, Probe(ctx, source_fd));
  return probe.identity;
}

absl::Status InitRoot(Context &ctx, FileDescriptor source_fd) {
  ABSL_ASSIGN_OR_RETURN(RootProbe probe, Probe(ctx, *source_fd));
  const DeviceId &device = probe.identity.device_id;
  ABSL_ASSIGN_OR_RETURN(DeviceId stored, GetSourceDeviceId(ctx.db));
  if (stored != device) {
    return absl::FailedPreconditionError(absl::StrCat(
        "cache database belongs to a different filesystem (",
        stored.ToString(), ") than the source (", device.ToString(), ")"));
  }
  ABSL_ASSIGN_OR_RETURN(FileHandle handle,
                        FileHandle::FromFd(*source_fd, device));
  ABSL_RETURN_IF_ERROR(ctx.db.Transaction([&]() -> absl::Status {
    ABSL_RETURN_IF_ERROR(cache::UpsertRoot(ctx, handle, probe.stx,
                                           probe.identity.backing_gen));
    return cache::EnsureDirectory(ctx, cache::kRootInode);
  }));
  absl::Status inserted = ctx.mounts.Insert(device, std::move(source_fd));
  if (!inserted.ok() && !absl::IsAlreadyExists(inserted)) return inserted;
  return absl::OkStatus();
}

absl::StatusOr<uint64_t> ReadGeneration(int opath_fd, mode_t mode) {
  if (!S_ISREG(mode) && !S_ISDIR(mode)) return 0;
  // FS_IOC_GETVERSION rejects O_PATH fds, so ask through a real one.
  // O_NONBLOCK and O_NOCTTY are belt and braces: only regular files and
  // directories get here.
  absl::StatusOr<FileDescriptor> fd = syscalls::ReopenPathFd(
      opath_fd, O_RDONLY | O_NONBLOCK | O_NOCTTY | O_CLOEXEC);
  absl::StatusOr<uint32_t> gen =
      fd.ok() ? GetInodeGeneration(**fd) : absl::StatusOr<uint32_t>(fd.status());
  if (gen.ok()) return *gen;
  switch (ErrnoOf(gen.status())) {
    case ENOTTY:
    case EOPNOTSUPP:
    case ENOSYS:
    case EACCES:
    case EPERM:
      VLOG(1) << "no inode generation, treating it as unknown: "
              << gen.status();
      return 0;
    default:
      return gen.status();
  }
}

// Verifies that `fd` (opened for `id`, whose cache row is `attr`) still
// refers to the same backing object the row describes, and forgets it
// (ESTALE) otherwise: the handle can decode to a different object than
// intended if the backing filesystem reused the inode number since the row
// was cached.
absl::StatusOr<FileDescriptor> VerifyBackingIdentity(
    Context &ctx, InodeId id, const cache::CachedAttr &attr,
    FileDescriptor fd) {
  ABSL_ASSIGN_OR_RETURN(
      struct statx stx,
      syscalls::statx(*fd, "", AT_EMPTY_PATH, STATX_INO | STATX_TYPE));
  bool same = stx.stx_ino == attr.backing_ino;
  if (same && attr.backing_gen != 0) {
    ABSL_ASSIGN_OR_RETURN(uint64_t gen, ReadGeneration(*fd, stx.stx_mode));
    // 0 means the generation cannot be read right now, not that it changed.
    same = gen == 0 || gen == attr.backing_gen;
  }
  if (!same) {
    ABSL_RETURN_IF_ERROR(ForgetStale(ctx, id));
    return dcfs::ErrnoToStatus(
        ESTALE, absl::StrCat("inode ", id,
                             " was replaced on the backing filesystem"));
  }
  return fd;
}

absl::StatusOr<FileDescriptor> OpenNode(Context &ctx, InodeId id, int flags) {
  if (id == cache::kRootInode) return OpenRoot(ctx, flags);
  ABSL_ASSIGN_OR_RETURN(cache::CachedAttr attr, cache::GetAttr(ctx, id));
  ABSL_ASSIGN_OR_RETURN(FileHandle handle, cache::GetHandle(ctx, id));

  absl::StatusOr<FileDescriptor> fd = handle.Open(ctx.mounts, flags);
  if (!fd.ok()) {
    if (ErrnoOf(fd.status()) == ESTALE) {
      ABSL_RETURN_IF_ERROR(ForgetStale(ctx, id));
      return fd.status();
    }
    if (ErrnoOf(fd.status()) == EPERM) {
      // open_by_handle_at needs CAP_DAC_READ_SEARCH; dcfs is required to
      // run as root (see the design doc), so this is a misconfiguration,
      // not a condition to work around.
      return dcfs::ErrnoToStatus(
          EPERM, "open_by_handle_at: dcfs must run as root "
                 "(CAP_DAC_READ_SEARCH)");
    }
    return fd.status();
  }
  return VerifyBackingIdentity(ctx, id, attr, *std::move(fd));
}

absl::StatusOr<struct statx> StatNode(Context &ctx, InodeId id) {
  ABSL_ASSIGN_OR_RETURN(FileDescriptor fd,
                        OpenNode(ctx, id, O_PATH | O_NOFOLLOW));
  return syscalls::statx(*fd, "", AT_EMPTY_PATH, kAttrMask);
}

absl::Status RefreshAttrs(Context &ctx, InodeId id) {
  ABSL_ASSIGN_OR_RETURN(struct statx stx, StatNode(ctx, id));
  return cache::UpdateAttr(ctx, id, stx);
}

absl::StatusOr<std::string> ReadSymlink(Context &ctx, InodeId id) {
  ABSL_ASSIGN_OR_RETURN(FileDescriptor fd,
                        OpenNode(ctx, id, O_PATH | O_NOFOLLOW));
  // An empty path makes readlinkat read the O_PATH symlink fd itself.
  return syscalls::readlinkat(*fd, "");
}

absl::StatusOr<std::vector<std::pair<std::string, std::string>>> ReadXattrs(
    Context &ctx, InodeId id) {
  ABSL_ASSIGN_OR_RETURN(FileDescriptor fd,
                        OpenNode(ctx, id, O_PATH | O_NOFOLLOW));
  return XattrsOf(*fd);
}

absl::Status RefreshXattrs(Context &ctx, InodeId id) {
  ABSL_ASSIGN_OR_RETURN(
      (std::vector<std::pair<std::string, std::string>> xattrs),
      ReadXattrs(ctx, id));
  return cache::ReplaceXattrs(ctx, id, xattrs);
}

absl::StatusOr<struct statvfs> StatFilesystem(Context &ctx, InodeId id) {
  ABSL_ASSIGN_OR_RETURN(cache::CachedAttr attr, cache::GetAttr(ctx, id));
  ABSL_ASSIGN_OR_RETURN(int mount_fd, ctx.mounts.Get(attr.device));
  return syscalls::fstatvfs(mount_fd);
}

absl::Status PopulateDirectory(Context &ctx, InodeId dir) {
  // Phase A: all the I/O, no transaction.
  ABSL_ASSIGN_OR_RETURN(FileDescriptor dir_fd,
                        OpenNode(ctx, dir, O_RDONLY | O_DIRECTORY));
  ABSL_ASSIGN_OR_RETURN(
      struct statx dir_stx,
      syscalls::statx(*dir_fd, "", AT_EMPTY_PATH, kMountIdMask));
  ABSL_ASSIGN_OR_RETURN(cache::CachedAttr dir_attr, cache::GetAttr(ctx, dir));
  ABSL_ASSIGN_OR_RETURN(std::vector<std::string> names, ReadDirNames(*dir_fd));

  std::vector<ChildRecord> children;
  std::vector<FoundFilesystem> filesystems;
  children.reserve(names.size());
  for (const std::string &name : names) {
    ABSL_ASSIGN_OR_RETURN(std::optional<ChildRecord> child,
                          ProbeChild(ctx, *dir_fd, dir_stx, dir_attr.device,
                                     name, filesystems));
    if (child.has_value()) children.push_back(*std::move(child));
  }
  VLOG(1) << "populating directory " << dir << ": " << children.size()
          << " entries";

  // Phase B: one transaction, no syscalls.
  return ctx.db.Transaction([&]() -> absl::Status {
    for (const FoundFilesystem &fs : filesystems) {
      absl::StatusOr<cache::FilesystemRow> known =
          cache::GetFilesystem(ctx, fs.device);
      if (known.ok()) continue;
      if (!absl::IsNotFound(known.status())) return known.status();
      ABSL_RETURN_IF_ERROR(cache::AddFilesystem(ctx, fs.device, fs.fstype, dir,
                                                fs.boundary_name));
    }
    std::vector<std::string> seen;
    seen.reserve(children.size());
    for (const ChildRecord &child : children) {
      ABSL_ASSIGN_OR_RETURN(
          cache::UpsertResult row,
          cache::UpsertInode(ctx, child.handle, child.stx, child.backing_gen));
      if (S_ISDIR(child.stx.stx_mode)) {
        ABSL_RETURN_IF_ERROR(cache::EnsureDirectory(ctx, row.id));
      }
      ABSL_RETURN_IF_ERROR(cache::LinkDentry(ctx, dir, child.name, row.id));
      if (child.symlink_target.has_value()) {
        ABSL_RETURN_IF_ERROR(
            cache::SetSymlink(ctx, row.id, *child.symlink_target));
      }
      ABSL_RETURN_IF_ERROR(cache::ReplaceXattrs(ctx, row.id, child.xattrs));
      seen.push_back(child.name);
    }
    ABSL_RETURN_IF_ERROR(cache::PruneDentriesNotIn(ctx, dir, seen));
    return cache::MarkDirComplete(ctx, dir, true);
  });
}

absl::StatusOr<cache::LookupResult> LookupOrPopulate(Context &ctx,
                                                     InodeId parent,
                                                     std::string_view name) {
  ABSL_ASSIGN_OR_RETURN(cache::LookupResult result,
                        cache::Lookup(ctx, parent, name));
  if (result.kind != cache::LookupResult::kUnknown) return result;

  ABSL_ASSIGN_OR_RETURN(bool complete, cache::IsDirComplete(ctx, parent));
  if (!complete) {
    ABSL_RETURN_IF_ERROR(PopulateDirectory(ctx, parent));
    ABSL_ASSIGN_OR_RETURN(result, cache::Lookup(ctx, parent, name));
    if (result.kind != cache::LookupResult::kUnknown) return result;
  }
  // The listing is complete and does not have `name`: remember that.
  ABSL_RETURN_IF_ERROR(cache::SetNegative(ctx, parent, name));
  return cache::LookupResult{.kind = cache::LookupResult::kNegative, .id = 0};
}

absl::Status StartupPurge(Context &ctx) {
  // Filesystems already checked and kept, so later passes skip them.
  absl::flat_hash_set<DeviceId> kept;
  // Each purge can cascade to filesystems mounted beneath the purged one,
  // so the list is re-read after every purge. ListFilesystems returns rows
  // in insertion order, and a filesystem is always found (and inserted)
  // through its parent, so parents are checked before their children and a
  // kept parent's mount fd is registered before its children need it.
  bool purged = true;
  while (purged) {
    purged = false;
    ABSL_ASSIGN_OR_RETURN(std::vector<cache::FilesystemRow> filesystems,
                          cache::ListFilesystems(ctx));
    for (const cache::FilesystemRow &fs : filesystems) {
      if (!fs.parent_inode.has_value()) continue;  // The source.
      if (kept.contains(fs.device)) continue;
      RET_CHECK(fs.boundary_name.has_value())
          << "filesystem " << fs.device.ToString()
          << " has a parent inode but no boundary name";
      ABSL_ASSIGN_OR_RETURN(std::optional<std::string> reason,
                            CheckFilesystem(ctx, fs));
      if (!reason.has_value()) {
        kept.insert(fs.device);
        continue;
      }
      LOG(INFO) << "forgetting filesystem " << fs.device.ToString() << " ("
                << FstypeName(fs.fstype) << ") mounted on inode "
                << *fs.parent_inode << " name " << *fs.boundary_name << ": "
                << *reason;
      absl::Status status = cache::PurgeFilesystem(ctx, fs.device);
      // Opening the parent may itself have invalidated it (ESTALE), which
      // already cascaded to this filesystem.
      if (!status.ok() && !absl::IsNotFound(status)) return status;
      ctx.mounts.Erase(fs.device);
      purged = true;
      break;
    }
  }
  return absl::OkStatus();
}

}  // namespace dcfs::backing
