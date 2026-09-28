#include "dcfs/backing.h"

// Needed only for the FUSE_SET_ATTR_* constants SetAttr() dispatches on;
// nothing here otherwise touches libfuse.
#define FUSE_USE_VERSION FUSE_MAKE_VERSION(3, 18)

#include <fcntl.h>
#include <linux/limits.h>  // NAME_MAX (255, the generic Linux VFS cap)
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
#include "absl/strings/str_format.h"
#include "absl/strings/str_join.h"
#include "dcfs/context.h"
#include "dcfs/device_id.h"
#include "dcfs/fd.h"
#include "dcfs/file_handle.h"
#include "dcfs/metadata_cache.h"
#include "dcfs/migrate.h"
#include "dcfs/status.h"
#include "dcfs/syscalls.h"
#include "fuse_lowlevel.h"

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

// Whether `id` has a writable dcfs open outstanding (see
// Context::open_for_write).
bool OpenForWrite(const Context &ctx, InodeId id) {
  return ctx.open_for_write != nullptr && ctx.open_for_write->contains(id);
}

// Records `stx` as `id`'s attributes -- current, unless `id` is open for
// writing (then stored but still marked unknown, in the same transaction:
// the kernel may write through the passthrough fd at any moment, so the
// values are stale as soon as they are read; see the README's "Crash
// robustness").
//
// Also kept unknown when the link count is 0 (an unlinked file dcfs still
// holds open): such a row only lives until the last close deletes it
// (DirCacheFS::Release), and a crash in between must leave it unknown --
// re-read by handle, so ESTALE -- rather than serve nlink 0 as current
// (audit F8).
absl::Status RecordAttrs(Context &ctx, InodeId id, const struct statx &stx) {
  return ctx.db.Transaction([&]() -> absl::Status {
    ABSL_RETURN_IF_ERROR(cache::UpdateAttr(ctx, id, stx));
    if (OpenForWrite(ctx, id) || stx.stx_nlink == 0) {
      ABSL_RETURN_IF_ERROR(cache::MarkAttrsUnknown(ctx, id));
    }
    return absl::OkStatus();
  });
}

std::string FormatTime(int64_t sec, uint32_t nsec) {
  return absl::StrFormat("%d.%09u", sec, nsec);
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

// Reads everything the cache stores about the object `fd` names -- already
// open, and already statx'd into `stx` -- beyond what the caller already
// knows: its file handle (identified by `device`, the containing
// directory's device -- a mount-boundary child never reaches here, see
// ProbeChild), inode generation, symlink target if it is one, and its
// xattrs. Shared by ProbeChild (an existing directory entry) and
// RecordNewChild (a freshly created object, which cannot be a boundary).
absl::StatusOr<ChildRecord> ProbeObject(int fd, std::string_view name,
                                        const DeviceId &device,
                                        const struct statx &stx) {
  ChildRecord record;
  record.name = std::string(name);
  record.stx = stx;
  ABSL_ASSIGN_OR_RETURN(record.handle, FileHandle::FromFd(fd, device));
  ABSL_ASSIGN_OR_RETURN(record.backing_gen, ReadGeneration(fd, stx.stx_mode));
  if (S_ISLNK(stx.stx_mode)) {
    ABSL_ASSIGN_OR_RETURN(record.symlink_target, syscalls::readlinkat(fd, ""));
  }
  ABSL_ASSIGN_OR_RETURN(record.xattrs, XattrsOf(fd));
  return record;
}

// Logs (once per (dir, name) per daemon run, via ctx.refused_boundaries)
// that `name` under `dir` is refused because it is a mount point or
// subvolume boundary -- see amendment 12 in the plan and the README's
// Limitations: dcfs requires one backing filesystem below --source, since
// one st_dev is what makes backing inode numbers (st_ino, shown to users
// unchanged) unambiguous. Returns whether this was the first time this
// daemon run saw this particular (dir, name) refused, purely so a caller
// that wants to log something itself can also dedupe (none currently do).
bool RecordRefusedBoundary(Context &ctx, InodeId dir, std::string_view name) {
  bool first = true;
  if (ctx.refused_boundaries != nullptr) {
    first =
        ctx.refused_boundaries->insert(RefusedBoundary{dir, std::string(name)})
            .second;
  }
  if (first) {
    LOG(ERROR) << "refusing to cache " << name << " under inode " << dir
               << ": it is a mount point or subvolume boundary; dcfs does "
                  "not support submounts (see README)";
  }
  // Kernel-supported FUSE submounts would plug in here: given
  // FUSE_ATTR_SUBMOUNT on this entry's fuse_entry_out and a distinct
  // st_dev the kernel assigns it, the kernel treats the entry as the root
  // of a separate super_block instead of relying on dcfs's own inode
  // numbers being unique across filesystems. That needs an INIT-time
  // opt-in on /dev/fuse (virtiofs has it today; see amendment 12) and is
  // left for later.
  return first;
}

// Reads everything the cache stores about child `name` of `dir_fd`
// (InodeId `dir`). nullopt if the child vanished before it could be opened
// (ENOENT racing the listing), or if it is a mount point or subvolume
// boundary (IsBoundary): amendment 12 refuses to cache across a boundary,
// so it is neither registered as a filesystem nor cached at all, and this
// returns nullopt for it too -- exactly as for a vanished child, since
// either way PopulateDirectory must leave `name` out of the listing. The
// two cases are told apart by RecordRefusedBoundary's caller checking
// ctx.refused_boundaries, not by this function's return value: see
// PopulateDirectory.
absl::StatusOr<std::optional<ChildRecord>> ProbeChild(
    Context &ctx, InodeId dir, int dir_fd, const struct statx &dir_stx,
    const DeviceId &dir_device, std::string_view name) {
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
  ABSL_ASSIGN_OR_RETURN(struct statx stx,
                        syscalls::statx(**child, "", AT_EMPTY_PATH,
                                        kAttrMask | kMountIdMask));

  if (IsBoundary(dir_stx, stx)) {
    RecordRefusedBoundary(ctx, dir, name);
    return std::nullopt;
  }

  return ProbeObject(**child, name, dir_device, stx);
}

struct RootProbe {
  RootIdentity identity;
  struct statx stx {};
};

absl::StatusOr<RootProbe> Probe(Context &ctx, int source_fd) {
  RootProbe probe;
  ABSL_ASSIGN_OR_RETURN(probe.identity.device_id, GetDeviceId(source_fd));
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

absl::Status ReconcileAttrs(Context &ctx, InodeId id,
                            const cache::CachedAttr &cached,
                            const struct statx &fresh) {
  // Out-of-band change detection, only where it is free. dcfs requires
  // exclusive access to the backing trees and does not look for changes
  // made behind its back (no fanotify, no revalidation). But whenever it
  // already holds a fresh statx of an object anyway -- VerifyBackingIdentity
  // on every handle open (OpenNode), and PopulateDirectory probing each
  // child -- it compares that against the cached attributes and, if they
  // disagree, logs it and adopts the fresh ones. No syscall is ever added
  // for this: a code path without a statx already in hand (OpenRoot, cache
  // hits, ...) detects nothing, and atime/blocks are deliberately not
  // compared (reads and delayed allocation change those legitimately).
  if (!cached.valid) return absl::OkStatus();
  std::vector<std::string> diffs;
  auto check = [&](unsigned int mask, std::string_view field, bool differs,
                   std::string from, std::string to) {
    if ((fresh.stx_mask & mask) != mask || !differs) return;
    diffs.push_back(absl::StrCat(field, " ", from, " -> ", to));
  };
  const struct stat &st = cached.st;
  check(STATX_TYPE | STATX_MODE, "mode", st.st_mode != fresh.stx_mode,
        absl::StrFormat("0%o", st.st_mode), absl::StrFormat("0%o", fresh.stx_mode));
  check(STATX_UID, "uid", st.st_uid != fresh.stx_uid, absl::StrCat(st.st_uid),
        absl::StrCat(fresh.stx_uid));
  check(STATX_GID, "gid", st.st_gid != fresh.stx_gid, absl::StrCat(st.st_gid),
        absl::StrCat(fresh.stx_gid));
  check(STATX_NLINK, "nlink", st.st_nlink != fresh.stx_nlink,
        absl::StrCat(st.st_nlink), absl::StrCat(fresh.stx_nlink));
  check(STATX_SIZE, "size",
        static_cast<uint64_t>(st.st_size) != fresh.stx_size,
        absl::StrCat(st.st_size), absl::StrCat(fresh.stx_size));
  const bool mtime_differs =
      (fresh.stx_mask & STATX_MTIME) != 0 &&
      (st.st_mtim.tv_sec != fresh.stx_mtime.tv_sec ||
       st.st_mtim.tv_nsec != static_cast<long>(fresh.stx_mtime.tv_nsec));
  const bool ctime_differs =
      (fresh.stx_mask & STATX_CTIME) != 0 &&
      (st.st_ctim.tv_sec != fresh.stx_ctime.tv_sec ||
       st.st_ctim.tv_nsec != static_cast<long>(fresh.stx_ctime.tv_nsec));
  check(STATX_MTIME, "mtime", mtime_differs,
        FormatTime(st.st_mtim.tv_sec, st.st_mtim.tv_nsec),
        FormatTime(fresh.stx_mtime.tv_sec, fresh.stx_mtime.tv_nsec));
  check(STATX_CTIME, "ctime", ctime_differs,
        FormatTime(st.st_ctim.tv_sec, st.st_ctim.tv_nsec),
        FormatTime(fresh.stx_ctime.tv_sec, fresh.stx_ctime.tv_nsec));
  if (diffs.empty()) return absl::OkStatus();

  const bool is_dir = S_ISDIR(fresh.stx_mode);
  const bool relist = is_dir && (mtime_differs || ctime_differs);
  LOG(WARNING) << "inode " << id
               << ": out-of-band change on the backing filesystem "
                  "(unsupported): "
               << absl::StrJoin(diffs, ", ") << "; adopting the new attributes"
               << (relist ? ", relisting the directory" : "")
               << (ctime_differs ? ", rereading its xattrs" : "");
  // The fresh statx is already in hand, so reacting costs no I/O: adopt it,
  // and mark unknown whatever else the change may have touched unseen.
  return ctx.db.Transaction([&]() -> absl::Status {
    ABSL_RETURN_IF_ERROR(RecordAttrs(ctx, id, fresh));
    if (relist) {
      ABSL_RETURN_IF_ERROR(cache::ForgetNegativeDentries(ctx, id));
    }
    if (ctime_differs) {
      ABSL_RETURN_IF_ERROR(cache::MarkXattrsUnknown(ctx, id));
    }
    return absl::OkStatus();
  });
}

// Verifies that `fd` (opened for `id`, whose cache row is `attr`) still
// refers to the same backing object the row describes, and forgets it
// (ESTALE) otherwise: the handle can decode to a different object than
// intended if the backing filesystem reused the inode number since the row
// was cached. The statx asks for every attribute the cache stores (the same
// single syscall either way), so that a still-matching object's attributes
// are also checked against the cache for free (ReconcileAttrs).
absl::StatusOr<FileDescriptor> VerifyBackingIdentity(
    Context &ctx, InodeId id, const cache::CachedAttr &attr,
    FileDescriptor fd) {
  ABSL_ASSIGN_OR_RETURN(struct statx stx,
                        syscalls::statx(*fd, "", AT_EMPTY_PATH, kAttrMask));
  bool same = stx.stx_ino == attr.backing_ino;
  uint64_t gen = 0;
  if (same && attr.backing_gen != 0) {
    ABSL_ASSIGN_OR_RETURN(gen, ReadGeneration(*fd, stx.stx_mode));
    // 0 means the generation cannot be read right now, not that it changed.
    same = gen == 0 || gen == attr.backing_gen;
  }
  // The birth time, when both sides know it, tells apart objects that share
  // a handle: btrfs can reissue an identical (ino, generation, handle)
  // after its own power loss (audit F6). Free: the statx above asked for it.
  const bool btime_known =
      (stx.stx_mask & STATX_BTIME) != 0 &&
      (stx.stx_btime.tv_sec != 0 || stx.stx_btime.tv_nsec != 0) &&
      (attr.btime.tv_sec != 0 || attr.btime.tv_nsec != 0);
  if (same && btime_known) {
    same = attr.btime.tv_sec == stx.stx_btime.tv_sec &&
           attr.btime.tv_nsec == static_cast<long>(stx.stx_btime.tv_nsec);
  }
  if (!same) {
    LOG(WARNING) << "inode " << id
                 << ": out-of-band change on the backing filesystem "
                    "(unsupported): its handle now reaches a different "
                    "object (inode number "
                 << attr.backing_ino << " -> " << stx.stx_ino
                 << ", generation " << attr.backing_gen << " -> " << gen
                 << ", birth time "
                 << FormatTime(attr.btime.tv_sec, attr.btime.tv_nsec) << " -> "
                 << FormatTime(stx.stx_btime.tv_sec, stx.stx_btime.tv_nsec)
                 << "); forgetting it (ESTALE)";
    ABSL_RETURN_IF_ERROR(ForgetStale(ctx, id));
    return dcfs::ErrnoToStatus(
        ESTALE, absl::StrCat("inode ", id,
                             " was replaced on the backing filesystem"));
  }
  ABSL_RETURN_IF_ERROR(ReconcileAttrs(ctx, id, attr, stx));
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
  return RecordAttrs(ctx, id, stx);
}

absl::Status RefreshAttrsFromFd(Context &ctx, InodeId id, int fd) {
  ABSL_ASSIGN_OR_RETURN(
      struct statx stx, syscalls::statx(fd, "", AT_EMPTY_PATH, kAttrMask));
  return RecordAttrs(ctx, id, stx);
}

absl::StatusOr<std::string> ReadFile(int fd, size_t size, off_t offset) {
  std::string buf(size, '\0');
  size_t total = 0;
  while (total < size) {
    ABSL_ASSIGN_OR_RETURN(
        size_t n,
        syscalls::pread(fd, buf.data() + total, size - total, offset + total));
    if (n == 0) break;  // EOF short of `size`.
    total += n;
  }
  buf.resize(total);
  return buf;
}

absl::StatusOr<size_t> WriteFile(int fd, std::span<const char> buf,
                                 off_t offset) {
  size_t total = 0;
  while (total < buf.size()) {
    ABSL_ASSIGN_OR_RETURN(
        size_t n, syscalls::pwrite(fd, buf.data() + total, buf.size() - total,
                                   offset + total));
    if (n == 0) break;  // Should not happen for a regular file.
    total += n;
  }
  return total;
}

absl::Status FallocateFd(int fd, int mode, off_t offset, off_t length) {
  return syscalls::fallocate(fd, mode, offset, length);
}

absl::Status FsyncFd(int fd, bool datasync) {
  return datasync ? syscalls::fdatasync(fd) : syscalls::fsync(fd);
}

absl::Status FsyncDir(Context &ctx, InodeId id, bool datasync) {
  ABSL_ASSIGN_OR_RETURN(FileDescriptor fd,
                        OpenNode(ctx, id, O_RDONLY | O_DIRECTORY));
  return FsyncFd(*fd, datasync);
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

namespace {

// Shared by SetXattr/RemoveXattr: an O_PATH fd on `id`, its type, and
// whichever real fd `apply` should use -- `open_fd` if given, else a fresh
// /proc reopen for a regular file/directory (freed automatically at the end
// of the calling statement). Special files use `opath_fd` itself via the
// *_opath syscalls instead of ever calling `apply`.
template <typename ApplyReal, typename ApplyOpath>
absl::Status ApplyXattrOp(Context &ctx, InodeId id, std::optional<int> open_fd,
                          ApplyReal apply_real, ApplyOpath apply_opath) {
  ABSL_ASSIGN_OR_RETURN(FileDescriptor opath_fd,
                        OpenNode(ctx, id, O_PATH | O_NOFOLLOW));
  ABSL_ASSIGN_OR_RETURN(
      struct statx stx, syscalls::statx(*opath_fd, "", AT_EMPTY_PATH, STATX_MODE));
  mode_t type = stx.stx_mode & S_IFMT;
  if (S_ISREG(type) || S_ISDIR(type)) {
    if (open_fd.has_value()) return apply_real(*open_fd);
    ABSL_ASSIGN_OR_RETURN(
        FileDescriptor fd,
        syscalls::ReopenPathFd(*opath_fd, O_RDONLY | O_CLOEXEC));
    return apply_real(*fd);
  }
  return apply_opath(*opath_fd);
}

}  // namespace

absl::Status SetXattr(Context &ctx, InodeId id, std::string_view name,
                      std::string_view value, int flags,
                      std::optional<int> open_fd) {
  std::span<const uint8_t> value_bytes(
      reinterpret_cast<const uint8_t *>(value.data()), value.size());
  return ApplyXattrOp(
      ctx, id, open_fd,
      [&](int fd) { return syscalls::fsetxattr(fd, name, value_bytes, flags); },
      [&](int fd) {
        return syscalls::setxattr_opath(fd, name, value_bytes, flags);
      });
}

absl::Status RemoveXattr(Context &ctx, InodeId id, std::string_view name,
                         std::optional<int> open_fd) {
  return ApplyXattrOp(
      ctx, id, open_fd,
      [&](int fd) { return syscalls::fremovexattr(fd, name); },
      [&](int fd) { return syscalls::removexattr_opath(fd, name); });
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

  // A child left out of `children` below is either a vanished dirent
  // (raced ENOENT) or a refused mount/subvolume boundary (see ProbeChild);
  // either way it is not linked, and not added to `seen` below, so
  // PruneDentriesNotIn (also below) drops any stale dentry that was cached
  // under that name before -- e.g. a plain directory that a filesystem got
  // mounted onto since the last populate.
  std::vector<ChildRecord> children;
  children.reserve(names.size());
  for (const std::string &name : names) {
    ABSL_ASSIGN_OR_RETURN(
        std::optional<ChildRecord> child,
        ProbeChild(ctx, dir, *dir_fd, dir_stx, dir_attr.device, name));
    if (child.has_value()) children.push_back(*std::move(child));
  }
  VLOG(1) << "populating directory " << dir << ": " << children.size()
          << " entries";

  // Phase B: one transaction, no syscalls.
  return ctx.db.Transaction([&]() -> absl::Status {
    std::vector<std::string> seen;
    seen.reserve(children.size());
    for (const ChildRecord &child : children) {
      // The probe's statx is free out-of-band detection for a child whose
      // row is already cached (under this name, and still the same object):
      // see ReconcileAttrs. A row adopted fresh by OpenNode just before
      // (e.g. `dir` itself) already matches, so nothing is logged twice.
      ABSL_ASSIGN_OR_RETURN(cache::LookupResult cached_dentry,
                            cache::Lookup(ctx, dir, child.name));
      if (cached_dentry.kind == cache::LookupResult::kFound) {
        ABSL_ASSIGN_OR_RETURN(cache::CachedAttr cached,
                              cache::GetAttr(ctx, cached_dentry.id));
        if (cached.device == child.handle.device &&
            cached.backing_ino == child.stx.stx_ino &&
            cached.backing_gen == child.backing_gen &&
            (cached.btime.tv_sec == child.stx.stx_btime.tv_sec &&
             cached.btime.tv_nsec ==
                 static_cast<long>(child.stx.stx_btime.tv_nsec))) {
          ABSL_RETURN_IF_ERROR(
              ReconcileAttrs(ctx, cached_dentry.id, cached, child.stx));
        }
      }
      ABSL_ASSIGN_OR_RETURN(
          cache::UpsertResult row,
          cache::UpsertInode(ctx, child.handle, child.stx, child.backing_gen));
      // UpsertInode marks the attributes current; a child that is open for
      // writing keeps them unknown (see RecordAttrs).
      if (OpenForWrite(ctx, row.id)) {
        ABSL_RETURN_IF_ERROR(cache::MarkAttrsUnknown(ctx, row.id));
      }
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
  // A name this long can never be a real directory entry on any local
  // filesystem this daemon backs onto (ext4/btrfs/xfs all cap a single
  // path component at NAME_MAX, the generic Linux VFS limit): reject it
  // outright rather than falling through to the "directory listing is
  // complete and doesn't have it" negative-entry path below, which would
  // otherwise misreport ENOENT (found via pjdfstest's chmod/02.t: a name
  // one byte past NAME_MAX got ENOENT once the parent directory's listing
  // was already cached, instead of ENAMETOOLONG). A name that reaches an
  // actual backing syscall (create, mkdir, ...) already gets ENAMETOOLONG
  // for free from the real filesystem; this only covers the pure-lookup
  // fast path, which never touches the backing filesystem at all once a
  // directory is known complete.
  if (name.size() > NAME_MAX) {
    return dcfs::ErrnoToStatus(ENAMETOOLONG, "path component too long");
  }
  ABSL_ASSIGN_OR_RETURN(cache::LookupResult result,
                        cache::Lookup(ctx, parent, name));
  if (result.kind != cache::LookupResult::kUnknown) return result;

  ABSL_ASSIGN_OR_RETURN(bool complete, cache::IsDirComplete(ctx, parent));
  if (!complete) {
    ABSL_RETURN_IF_ERROR(PopulateDirectory(ctx, parent));
    ABSL_ASSIGN_OR_RETURN(result, cache::Lookup(ctx, parent, name));
    if (result.kind != cache::LookupResult::kUnknown) return result;
  }
  // `name` was refused this daemon run as a mount point or subvolume
  // boundary (see ProbeChild/RecordRefusedBoundary): it is deliberately
  // absent from the cache, but it is not "known absent" the way a genuine
  // ENOENT is, so it must not be cached negative -- that would keep
  // answering ENOENT even after the offending mount goes away and the
  // directory is repopulated with `name` unmarked. Answer EXDEV instead,
  // every time, straight from this in-memory set.
  if (ctx.refused_boundaries != nullptr &&
      ctx.refused_boundaries->contains(
          RefusedBoundary{.parent = parent, .name = std::string(name)})) {
    return dcfs::ErrnoToStatus(
        EXDEV, "mount point or subvolume boundary (submounts are not "
               "supported; see README)");
  }
  // The listing is complete and does not have `name`: remember that.
  ABSL_RETURN_IF_ERROR(cache::SetNegative(ctx, parent, name));
  return cache::LookupResult{.kind = cache::LookupResult::kNegative, .id = 0};
}

absl::StatusOr<NewChild> RecordNewChild(Context &ctx, InodeId parent,
                                        int parent_fd, std::string_view name,
                                        bool open_for_write) {
  // Phase A: I/O -- probe the object just created, exactly like ProbeChild
  // does for an existing directory entry, minus the mount-boundary check
  // (nothing can already be mounted on an object that did not exist a
  // moment ago).
  ABSL_ASSIGN_OR_RETURN(FileDescriptor child_fd,
                        syscalls::openat(parent_fd, name, O_PATH | O_NOFOLLOW));
  ABSL_ASSIGN_OR_RETURN(
      struct statx stx,
      syscalls::statx(*child_fd, "", AT_EMPTY_PATH, kAttrMask | kMountIdMask));
  ABSL_ASSIGN_OR_RETURN(cache::CachedAttr parent_attr, cache::GetAttr(ctx, parent));
  ABSL_ASSIGN_OR_RETURN(
      ChildRecord record, ProbeObject(*child_fd, name, parent_attr.device, stx));

  // Phase B: one transaction, no syscalls.
  NewChild result;
  ABSL_RETURN_IF_ERROR(ctx.db.Transaction([&]() -> absl::Status {
    ABSL_ASSIGN_OR_RETURN(
        cache::UpsertResult row,
        cache::UpsertInode(ctx, record.handle, record.stx, record.backing_gen));
    if (S_ISDIR(record.stx.stx_mode)) {
      ABSL_RETURN_IF_ERROR(cache::EnsureDirectory(ctx, row.id));
      // Unlike EnsureDirectory's usual caller (PopulateDirectory,
      // discovering a pre-existing, not-yet-listed subdirectory), a
      // directory RecordNewChild is recording was *just* created by this
      // same op: nothing has had a chance to put anything in it yet, so it
      // is already known to have zero children -- no separate populate is
      // needed before a listing of it can be served from the cache.
      ABSL_RETURN_IF_ERROR(cache::MarkDirComplete(ctx, row.id, true));
    }
    ABSL_RETURN_IF_ERROR(cache::LinkDentry(ctx, parent, name, row.id));
    if (record.symlink_target.has_value()) {
      ABSL_RETURN_IF_ERROR(
          cache::SetSymlink(ctx, row.id, *record.symlink_target));
    }
    ABSL_RETURN_IF_ERROR(cache::ReplaceXattrs(ctx, row.id, record.xattrs));
    // The new row is dirty too: if a power loss keeps this transaction but
    // the backing filesystem loses the create, recovery must not serve its
    // attributes. (Its parent was made durably dirty by the caller's phase
    // 1; this insert cannot outlive the row, being in the same
    // transaction.)
    const InodeId new_id[] = {row.id};
    ABSL_RETURN_IF_ERROR(cache::MarkDirty(ctx, new_id));
    if (open_for_write || OpenForWrite(ctx, row.id)) {
      ABSL_RETURN_IF_ERROR(cache::MarkAttrsUnknown(ctx, row.id));
    }
    result = NewChild{.id = row.id, .fuse_gen = row.fuse_gen, .stx = record.stx};
    return absl::OkStatus();
  }));
  return result;
}

absl::Status MkdirAt(Context &ctx, int parent_fd, std::string_view name,
                     mode_t mode) {
  return syscalls::mkdirat(parent_fd, name, mode);
}

absl::Status MknodAt(Context &ctx, int parent_fd, std::string_view name,
                     mode_t mode, dev_t rdev) {
  return syscalls::mknodat(parent_fd, name, mode, rdev);
}

absl::Status SymlinkAt(Context &ctx, int parent_fd, std::string_view name,
                       std::string_view target) {
  return syscalls::symlinkat(target, parent_fd, name);
}

absl::StatusOr<FileDescriptor> CreateAt(Context &ctx, int parent_fd,
                                        std::string_view name, int flags,
                                        mode_t mode) {
  return syscalls::openat(parent_fd, name, flags | O_CREAT, mode);
}

absl::Status LinkAt(Context &ctx, InodeId src, InodeId newparent,
                    std::string_view newname) {
  ABSL_ASSIGN_OR_RETURN(FileDescriptor src_fd,
                        OpenNode(ctx, src, O_PATH | O_NOFOLLOW));
  ABSL_ASSIGN_OR_RETURN(
      FileDescriptor newparent_fd,
      OpenNode(ctx, newparent, O_RDONLY | O_DIRECTORY));
  return syscalls::linkat(*src_fd, "", *newparent_fd, newname, AT_EMPTY_PATH);
}

absl::StatusOr<struct statx> RecordNewLink(Context &ctx, InodeId src,
                                           InodeId newparent,
                                           std::string_view newname) {
  ABSL_ASSIGN_OR_RETURN(struct statx stx, StatNode(ctx, src));
  ABSL_RETURN_IF_ERROR(ctx.db.Transaction([&]() -> absl::Status {
    ABSL_RETURN_IF_ERROR(cache::LinkDentry(ctx, newparent, newname, src));
    return RecordAttrs(ctx, src, stx);
  }));
  return stx;
}

absl::Status UnlinkAt(Context &ctx, InodeId parent, std::string_view name,
                      int flags) {
  ABSL_ASSIGN_OR_RETURN(FileDescriptor parent_fd,
                        OpenNode(ctx, parent, O_RDONLY | O_DIRECTORY));
  return syscalls::unlinkat(*parent_fd, name, flags);
}

absl::Status RenameAt(Context &ctx, InodeId parent, std::string_view name,
                      InodeId newparent, std::string_view newname,
                      unsigned flags) {
  ABSL_ASSIGN_OR_RETURN(FileDescriptor parent_fd,
                        OpenNode(ctx, parent, O_RDONLY | O_DIRECTORY));
  ABSL_ASSIGN_OR_RETURN(FileDescriptor newparent_fd,
                        OpenNode(ctx, newparent, O_RDONLY | O_DIRECTORY));
  return syscalls::renameat2(*parent_fd, name, *newparent_fd, newname, flags);
}

absl::StatusOr<std::optional<uint64_t>> BackingNlink(Context &ctx,
                                                     InodeId id) {
  absl::StatusOr<FileDescriptor> fd = OpenNode(ctx, id, O_PATH | O_NOFOLLOW);
  if (!fd.ok()) {
    int err = ErrnoOf(fd.status());
    // ESTALE: OpenNode has already invalidated the row. ENOENT is not one
    // OpenNode handles itself, so the row is forgotten here to give the
    // caller the same "already gone" contract either way.
    if (err == ESTALE) return std::nullopt;
    if (err == ENOENT) {
      ABSL_RETURN_IF_ERROR(ForgetStale(ctx, id));
      return std::nullopt;
    }
    return fd.status();
  }
  ABSL_ASSIGN_OR_RETURN(struct statx stx,
                        syscalls::statx(**fd, "", AT_EMPTY_PATH, STATX_NLINK));
  return static_cast<uint64_t>(stx.stx_nlink);
}

absl::Status StartupPurge(Context &ctx) {
  // Every non-source row left in the filesystems table is purged
  // unconditionally. Since amendment 12, ProbeChild/PopulateDirectory never
  // add one (a mount point or subvolume boundary is refused, not cached --
  // see RecordRefusedBoundary), so any row found here can only be left over
  // from a database built before that change, back when such a boundary
  // was cached like any other filesystem. Nothing recorded there is ever
  // legitimate to keep any more, regardless of whether it happens to still
  // be mounted where it was found -- amendment 12 forbids serving cached
  // content across a filesystem boundary at all -- so this no longer
  // re-probes each one (CheckFilesystem, which used to do that, is gone);
  // it just forgets it, the same way an unmounted or replaced filesystem
  // used to be forgotten. (In the common case there is nothing to do here:
  // the startup check in main.cc already refuses to start at all if a real
  // mount lies below --source, so a fresh database's filesystems table
  // holds only the source row.) Each purge can cascade to filesystems that
  // were mounted beneath the one just purged, so the list is re-read after
  // every purge.
  bool purged = true;
  while (purged) {
    purged = false;
    ABSL_ASSIGN_OR_RETURN(std::vector<cache::FilesystemRow> filesystems,
                          cache::ListFilesystems(ctx));
    for (const cache::FilesystemRow &fs : filesystems) {
      if (!fs.parent_inode.has_value()) continue;  // The source.
      LOG(INFO) << "forgetting filesystem " << fs.device.ToString() << " ("
                << FstypeName(fs.fstype) << ") mounted on inode "
                << *fs.parent_inode << " name "
                << fs.boundary_name.value_or("<unknown>")
                << ": submounts are no longer supported (see README)";
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

absl::Status SyncBacking(Context &ctx) {
  for (int fd : ctx.mounts.Fds()) {
    ABSL_RETURN_IF_ERROR(syscalls::syncfs(fd));
  }
  std::vector<InodeId> keep;
  if (ctx.open_for_write != nullptr) {
    keep.assign(ctx.open_for_write->begin(), ctx.open_for_write->end());
  }
  return cache::ClearDirty(ctx, keep);
}

absl::Status StartRun(Context &ctx, std::string_view boot_id) {
  ABSL_ASSIGN_OR_RETURN(bool clean, GetCleanShutdown(ctx.db));
  ABSL_ASSIGN_OR_RETURN(std::optional<std::string> last_boot_id,
                        GetBootId(ctx.db));
  const bool unclean = !clean;
  ABSL_ASSIGN_OR_RETURN(int64_t recovered, cache::RecoverDirty(ctx));
  if (unclean || recovered > 0) {
    const bool rebooted =
        last_boot_id.has_value() && *last_boot_id != boot_id;
    LOG(WARNING) << "the last run did not shut down cleanly ("
                 << (rebooted ? "the machine rebooted meanwhile: a crash or "
                                "power loss"
                              : "the daemon died; same boot")
                 << "); recovered " << recovered
                 << " dirty cache entries (their cached state will be "
                    "re-read from the backing filesystem)";
  }
  return ctx.db.Transaction(
      [&]() -> absl::Status {
        ABSL_RETURN_IF_ERROR(SetCleanShutdown(ctx.db, false));
        return SetBootId(ctx.db, boot_id);
      },
      sqlite3::Durability::kSync);
}

absl::Status FinishRun(Context &ctx) {
  ABSL_RETURN_IF_ERROR(SyncBacking(ctx));
  ABSL_RETURN_IF_ERROR(ctx.db.Checkpoint());
  if (ctx.dirty.any) {
    return absl::FailedPreconditionError(
        "dirty cache entries remain (a writable open is still "
        "outstanding); leaving the clean-shutdown flag unset");
  }
  return ctx.db.Transaction(
      [&] { return SetCleanShutdown(ctx.db, true); },
      sqlite3::Durability::kSync);
}

namespace {

// Bits SetAttr's mode-related branches (an explicit FUSE_SET_ATTR_MODE, or
// a KILL_SUID/KILL_SGID-only clear) share: reopening `opath_fd` as a real
// fd when the node is a regular file or directory (fchmod rejects O_PATH),
// else fchmod_opath -- except a symlink, which cannot be chmod'd at all on
// Linux (no lchmod).
absl::Status ApplyMode(int opath_fd, mode_t type, mode_t mode) {
  if (S_ISLNK(type)) {
    return dcfs::ErrnoToStatus(EOPNOTSUPP, "chmod on a symlink");
  }
  if (S_ISREG(type) || S_ISDIR(type)) {
    ABSL_ASSIGN_OR_RETURN(
        FileDescriptor fd,
        syscalls::ReopenPathFd(
            opath_fd, O_RDONLY | O_NONBLOCK | O_NOCTTY | O_CLOEXEC));
    return syscalls::fchmod(*fd, mode);
  }
  return syscalls::fchmod_opath(opath_fd, mode);
}

// FUSE_SET_ATTR_SIZE: only ever valid for a regular file, same as the
// kernel's own VFS-level check (notify_change() rejects ATTR_SIZE on
// anything else before a filesystem ever sees it).
absl::Status ApplySize(int opath_fd, mode_t type, off_t size) {
  if (S_ISDIR(type)) {
    return dcfs::ErrnoToStatus(EISDIR, "truncate on a directory");
  }
  if (!S_ISREG(type)) {
    return dcfs::ErrnoToStatus(
        EINVAL, "truncate on a non-regular, non-directory file");
  }
  ABSL_ASSIGN_OR_RETURN(
      FileDescriptor fd, syscalls::ReopenPathFd(opath_fd, O_WRONLY | O_CLOEXEC));
  return syscalls::ftruncate(*fd, size);
}

// FUSE_SET_ATTR_ATIME/MTIME(_NOW): the regular/dir-vs-other-types split as
// ApplyMode, but with no symlink exception -- futimens_opath is verified
// safe there (see syscalls.h).
absl::Status ApplyTimes(int opath_fd, mode_t type, const struct timespec times[2]) {
  if (S_ISREG(type) || S_ISDIR(type)) {
    ABSL_ASSIGN_OR_RETURN(
        FileDescriptor fd,
        syscalls::ReopenPathFd(
            opath_fd, O_RDONLY | O_NONBLOCK | O_NOCTTY | O_CLOEXEC));
    return syscalls::futimens(*fd, times);
  }
  return syscalls::futimens_opath(opath_fd, times);
}

}  // namespace

absl::Status SetAttr(
    Context &ctx, InodeId id, const struct stat &attr, int to_set) {
  ABSL_ASSIGN_OR_RETURN(FileDescriptor fd, OpenNode(ctx, id, O_PATH | O_NOFOLLOW));
  ABSL_ASSIGN_OR_RETURN(
      struct statx stx, syscalls::statx(*fd, "", AT_EMPTY_PATH, STATX_MODE));
  mode_t type = stx.stx_mode & S_IFMT;

  if (to_set & FUSE_SET_ATTR_SIZE) {
    ABSL_RETURN_IF_ERROR(ApplySize(*fd, type, attr.st_size));
  }

  if (to_set & (FUSE_SET_ATTR_UID | FUSE_SET_ATTR_GID)) {
    uid_t uid = (to_set & FUSE_SET_ATTR_UID) ? attr.st_uid
                                             : static_cast<uid_t>(-1);
    gid_t gid = (to_set & FUSE_SET_ATTR_GID) ? attr.st_gid
                                             : static_cast<gid_t>(-1);
    // fchownat works on an O_PATH fd via AT_EMPTY_PATH with an empty path.
    ABSL_RETURN_IF_ERROR(syscalls::fchownat(*fd, "", uid, gid, AT_EMPTY_PATH));
  }

  if (to_set & FUSE_SET_ATTR_MODE) {
    ABSL_RETURN_IF_ERROR(ApplyMode(*fd, type, attr.st_mode));
  } else if ((to_set & (FUSE_SET_ATTR_KILL_SUID | FUSE_SET_ATTR_KILL_SGID)) &&
             !S_ISLNK(type)) {
    // KILL_SUID/KILL_SGID sent without MODE: the kernel wants the
    // setuid/setgid bits cleared from whatever the node's mode already
    // is (e.g. after a write), not set to any particular new mode.
    // Nothing to do for a symlink, which has no meaningful mode bits.
    mode_t current_mode = stx.stx_mode & 07777;
    if (to_set & FUSE_SET_ATTR_KILL_SUID) current_mode &= ~S_ISUID;
    if (to_set & FUSE_SET_ATTR_KILL_SGID) current_mode &= ~S_ISGID;
    ABSL_RETURN_IF_ERROR(ApplyMode(*fd, type, current_mode));
  }

  if (to_set & (FUSE_SET_ATTR_ATIME | FUSE_SET_ATTR_MTIME |
                FUSE_SET_ATTR_ATIME_NOW | FUSE_SET_ATTR_MTIME_NOW)) {
    struct timespec times[2] = {
        {.tv_sec = 0, .tv_nsec = UTIME_OMIT},
        {.tv_sec = 0, .tv_nsec = UTIME_OMIT},
    };
    if (to_set & FUSE_SET_ATTR_ATIME_NOW) {
      times[0].tv_nsec = UTIME_NOW;
    } else if (to_set & FUSE_SET_ATTR_ATIME) {
      times[0] = attr.st_atim;
    }
    if (to_set & FUSE_SET_ATTR_MTIME_NOW) {
      times[1].tv_nsec = UTIME_NOW;
    } else if (to_set & FUSE_SET_ATTR_MTIME) {
      times[1] = attr.st_mtim;
    }
    ABSL_RETURN_IF_ERROR(ApplyTimes(*fd, type, times));
  }

  // FUSE_SET_ATTR_CTIME is intentionally never consulted: ctime cannot be
  // set directly (see SetAttr's declaration comment).
  return absl::OkStatus();
}

}  // namespace dcfs::backing
