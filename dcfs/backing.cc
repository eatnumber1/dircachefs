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
#include <span>
#include <type_traits>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "absl/cleanup/cleanup.h"
#include "absl/container/flat_hash_set.h"
#include "absl/functional/function_ref.h"
#include "absl/log/check.h"
#include "absl/log/log.h"
#include "absl/status/status.h"
#include "absl/status/status_macros.h"
#include "absl/status/statusor.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/str_format.h"
#include "absl/strings/str_join.h"
#include "dcfs/context.h"
#include "dcfs/credentials.h"
#include "dcfs/device_id.h"
#include "dcfs/fd.h"
#include "dcfs/escape.h"
#include "dcfs/file_handle.h"
#include "dcfs/metadata_cache.h"
#include "dcfs/migrate.h"
#include "dcfs/protocol_events.h"
#include "dcfs/ret_check.h"
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

// --- Taking on the caller's identity -----------------------------------------
//
// dcfs runs as root, but a backing syscall made on behalf of a FUSE request
// must behave as if the caller had made it: a new object must be owned by
// the caller (and get its group from the caller's fsgid, or from a setgid
// parent), chown/utimes/truncate/xattr rules must be decided for the
// caller, and a sticky directory must protect other users' entries from
// it. So the syscalls whose outcome depends on who asks run inside
// AsCaller, which switches the thread's filesystem credentials to the
// caller's for just that syscall -- the approach virtiofsd and nfsd take.
//
// Only the one syscall: never the open_by_handle_at (OpenNode) or /proc
// reopen (ReopenPathFd) that reaches the object first, nor the phase-3
// probes after it. open_by_handle_at needs CAP_DAC_READ_SEARCH, which the
// kernel removes from the effective set while fsuid is not 0 (together
// with every other filesystem capability: CAP_CHOWN, CAP_DAC_OVERRIDE,
// CAP_FOWNER, CAP_FSETID, CAP_LINUX_IMMUTABLE, CAP_MAC_OVERRIDE, CAP_MKNOD;
// cap_task_fix_setuid), and restores when it returns to 0. Reaching the
// object is dcfs's own business (identity-checked by OpenNode); what the
// caller may do to it is the kernel's, and default_permissions has the
// kernel check that on the FUSE side too before the request is even sent.
// Capabilities outside that set (CAP_SYS_ADMIN, CAP_SETFCAP, ...) stay:
// the kernel has already checked the real caller for those on the FUSE
// side (trusted.*/security.* xattrs), and the kernel's own killpriv
// removal of security.capability, sent as the caller, needs CAP_SETFCAP.
//
// The switch is per thread: setfsuid/setfsgid are, and so is the raw
// setgroups system call (glibc's setgroups() is not: it signals every
// thread to apply it process-wide). dcfs serves requests on one thread
// today; with one thread per io_uring queue later, each thread switches
// only itself, so this stays correct as long as a switch never spans a
// suspension point (a co_await) -- AsCaller wraps one synchronous syscall.

// The caller's umask (Credentials::umask) is switched to as well, and back
// to 0 (main.cc's) afterwards: the kernel sends the mode of a create
// unmasked (dcfs requests FUSE_CAP_DONT_MASK, which POSIX ACLs need), and
// the backing filesystem applies the umask only where the parent has no
// default ACL. Unlike the credentials, the umask is per process (the
// fs_struct); worker threads will need unshare(CLONE_FS), as virtiofsd
// does. Outside a switch it is whatever it was (0 in the daemon: main.cc).

// The thread's supplementary groups before a switch, to restore after it.
using SavedGroups = std::vector<gid_t>;

// Puts the thread's credentials back after a switch. Cannot fail for a
// process whose effective uid is root; if it somehow did, carrying on
// would perform every later backing operation as the wrong user, so that
// is fatal.
void RestoreRoot(const SavedGroups &groups) {
  syscalls::setfsuid(0);
  absl::Status status = syscalls::setgroups_thread(groups);
  syscalls::setfsgid(0);
  CHECK(status.ok() && syscalls::fsuid() == 0 && syscalls::fsgid() == 0)
      << "cannot restore root filesystem credentials: " << status
      << " (fsuid " << syscalls::fsuid() << ", fsgid " << syscalls::fsgid()
      << ")";
}

// Switches to `caller`: fsgid and groups first (setgroups needs
// CAP_SETGID, which a nonzero fsuid would not remove, but the order keeps
// the thread from ever being "the caller" with root's groups), fsuid last.
// setfsuid/setfsgid report no errors (an id the kernel will not accept --
// e.g. -1, sent for a caller whose id has no mapping -- is silently
// ignored), so each is read back; a switch that did not take is EPERM,
// with the thread restored. Refuses to nest: a thread not at fsuid/fsgid 0
// on entry means an earlier switch leaked.
absl::StatusOr<SavedGroups> SwitchTo(const Credentials &caller) {
  RET_CHECK_EQ(syscalls::fsuid(), 0u) << "credential switch already active";
  RET_CHECK_EQ(syscalls::fsgid(), 0u) << "credential switch already active";
  ABSL_ASSIGN_OR_RETURN(SavedGroups saved, syscalls::getgroups());
  absl::Status status;
  syscalls::setfsgid(caller.gid);
  if (syscalls::fsgid() != caller.gid) {
    status = dcfs::ErrnoToStatus(
        EPERM, absl::StrCat("setfsgid(", caller.gid, ") did not take"));
  }
  if (status.ok()) status = syscalls::setgroups_thread(caller.groups);
  if (status.ok()) {
    syscalls::setfsuid(caller.uid);
    if (syscalls::fsuid() != caller.uid) {
      status = dcfs::ErrnoToStatus(
          EPERM, absl::StrCat("setfsuid(", caller.uid, ") did not take"));
    }
  }
  if (!status.ok()) {
    RestoreRoot(saved);
    return status;
  }
  return saved;
}

// Runs `op` (one backing syscall, returning absl::Status or
// absl::StatusOr<T>) with the thread's filesystem credentials switched to
// `caller`, and switches back to root afterwards, whatever `op` returned.
template <typename Op>
std::invoke_result_t<Op> AsCaller(const Credentials &caller, Op &&op) {
  ABSL_ASSIGN_OR_RETURN(SavedGroups saved, SwitchTo(caller));
  const mode_t saved_umask = syscalls::umask(caller.umask & 0777);
  absl::Cleanup restore = [&saved, saved_umask] {
    syscalls::umask(saved_umask);
    RestoreRoot(saved);
  };
  return std::forward<Op>(op)();
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
//
// The caller must already have established it may write (a fill's
// cache::CanFill, a mutation's Owns), in the same transaction.
absl::Status WriteAttrs(Context &ctx, InodeId id, const struct statx &stx) {
  return ctx.db.Transaction([&]() -> absl::Status {
    ABSL_RETURN_IF_ERROR(cache::UpdateAttr(ctx, id, stx));
    if (OpenForWrite(ctx, id) || stx.stx_nlink == 0) {
      ABSL_RETURN_IF_ERROR(cache::MarkAttrsUnknown(ctx, id));
    }
    return absl::OkStatus();
  });
}

// WriteAttrs as a fill that took `snapshot` (see cache::CanFill): records
// nothing if a mutation of `id` began, ended or is in flight since.
absl::Status FillAttrs(Context &ctx, cache::FillSnapshot snapshot, InodeId id,
                       const struct statx &stx) {
  bool recorded = false;
  ABSL_RETURN_IF_ERROR(ctx.db.Transaction([&]() -> absl::Status {
    if (!cache::CanFill(ctx, snapshot, id)) {
      VLOG(1) << "inode " << id
              << ": not caching attributes read concurrently with a "
                 "mutation of it";
      return absl::OkStatus();
    }
    recorded = true;
    return WriteAttrs(ctx, id, stx);
  }));
  // Model: the fill that ends a getattr, readdirplus or mutation.
  ctx.events->AttrsFilled(ctx, id, recorded);
  return absl::OkStatus();
}

// What a probe's statx says the name holds, for the protocol events.
events::Probe ProbeOf(const struct statx &stx) {
  return {.kind = events::Probe::kPresent,
          .ino = stx.stx_ino,
          .btime_sec = stx.stx_btime.tv_sec,
          .btime_nsec = stx.stx_btime.tv_nsec};
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

// Logs that `name` under `dir` is refused because it is a mount point or
// subvolume boundary -- see amendment 12 in the plan and the README's
// Limitations: dcfs requires one backing filesystem below --source, since
// one st_dev is what makes backing inode numbers (st_ino, shown to users
// unchanged) unambiguous. No de-duplication: once `dir` is fully populated
// and the refusal is persisted (see PopulateDirectory/cache::SetRefused),
// a later lookup answers straight from the cache without ever calling this
// again, so in practice this logs at most once per (dir, name) per
// populate of `dir` -- which itself only happens again if something marks
// `dir` incomplete (e.g. an out-of-band change; see ReconcileAttrs), a rare
// enough event that re-logging then is fine.
void LogRefusedBoundary(InodeId dir, std::string_view name) {
  LOG(ERROR) << "refusing to cache " << EscapeBytes(name) << " under inode " << dir
             << ": it is a mount point or subvolume boundary; dcfs does "
                "not support submounts (see README)";
  // Kernel-supported FUSE submounts would plug in here: given
  // FUSE_ATTR_SUBMOUNT on this entry's fuse_entry_out and a distinct
  // st_dev the kernel assigns it, the kernel treats the entry as the root
  // of a separate super_block instead of relying on dcfs's own inode
  // numbers being unique across filesystems. That needs an INIT-time
  // opt-in on /dev/fuse (virtiofs has it today; see amendment 12) and is
  // left for later.
}

// Reads everything the cache stores about child `name` of `dir_fd`
// (InodeId `dir`). nullopt if the child vanished before it could be opened
// (ENOENT racing the listing) or if it is a mount point or subvolume
// boundary (IsBoundary): amendment 12 refuses to cache across a boundary,
// so it is neither registered as a filesystem nor cached as a normal child,
// and this returns nullopt for it too -- exactly as for a vanished child.
// `refused` tells the two apart (false for a vanished child) so the caller
// (PopulateDirectory) can persist the refusal instead of just dropping the
// name: see cache::SetRefused and this file's top comment on why it must
// never be cached as a plain negative entry.
//
// `on_read` is called with what the probe read as soon as it is known (after
// the openat and statx, before anything else): the read point a protocol
// event must mark (see ResolveName).
absl::StatusOr<std::optional<ChildRecord>> ProbeChild(
    int dir_fd, const struct statx &dir_stx, const DeviceId &dir_device,
    InodeId dir, std::string_view name, bool &refused,
    absl::FunctionRef<void(const events::Probe &)> on_read) {
  refused = false;
  // Everything below reads through this one fd, so all of it describes the
  // same object even if `name` is replaced meanwhile.
  absl::StatusOr<FileDescriptor> child =
      syscalls::openat(dir_fd, name, O_PATH | O_NOFOLLOW);
  if (!child.ok()) {
    if (ErrnoOf(child.status()) == ENOENT) {
      VLOG(1) << "child " << EscapeBytes(name) << " vanished while listing its directory";
      on_read({.kind = events::Probe::kAbsent});
      return std::nullopt;
    }
    return child.status();
  }
  ABSL_ASSIGN_OR_RETURN(struct statx stx,
                        syscalls::statx(**child, "", AT_EMPTY_PATH,
                                        kAttrMask | kMountIdMask));

  if (IsBoundary(dir_stx, stx)) {
    refused = true;
    LogRefusedBoundary(dir, name);
    on_read({.kind = events::Probe::kRefused});
    return std::nullopt;
  }
  on_read(ProbeOf(stx));

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
  // Model: a whole getattr fill of the root (nothing is in flight yet).
  ctx.events->RootRecorded(ctx);
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

absl::Status ReconcileAttrs(Context &ctx, cache::FillSnapshot snapshot,
                            InodeId id, const cache::CachedAttr &cached,
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
  // A mutation of `id` since `cached` was read (or still in flight) is not
  // an out-of-band change, and `fresh` may predate it: nothing to learn.
  if (!cache::CanFill(ctx, snapshot, id)) return absl::OkStatus();
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
  ABSL_RETURN_IF_ERROR(ctx.db.Transaction([&]() -> absl::Status {
    ABSL_RETURN_IF_ERROR(WriteAttrs(ctx, id, fresh));
    if (relist) {
      ABSL_RETURN_IF_ERROR(cache::ForgetNegativeDentries(ctx, id));
    }
    if (ctime_differs) {
      ABSL_RETURN_IF_ERROR(cache::MarkXattrsUnknown(ctx, id));
    }
    return absl::OkStatus();
  }));
  // Not a step of the model (it has no out-of-band changes).
  ctx.events->OutOfBandChange(ctx, id);
  return absl::OkStatus();
}

// Verifies that `fd` (opened for `id`, whose cache row is `attr`) still
// refers to the same backing object the row describes, and forgets it
// (ESTALE) otherwise: the handle can decode to a different object than
// intended if the backing filesystem reused the inode number since the row
// was cached. The statx asks for every attribute the cache stores (the same
// single syscall either way), so that a still-matching object's attributes
// are also checked against the cache for free (ReconcileAttrs).
absl::StatusOr<FileDescriptor> VerifyBackingIdentity(
    Context &ctx, cache::FillSnapshot snapshot, InodeId id,
    const cache::CachedAttr &attr, FileDescriptor fd) {
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
  ABSL_RETURN_IF_ERROR(ReconcileAttrs(ctx, snapshot, id, attr, stx));
  return fd;
}

absl::StatusOr<FileDescriptor> OpenNode(Context &ctx, InodeId id, int flags) {
  if (id == cache::kRootInode) return OpenRoot(ctx, flags);
  // ReconcileAttrs compares `attr` with a statx taken after the open's I/O.
  const cache::FillSnapshot snapshot = cache::BeginFill(ctx);
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
  return VerifyBackingIdentity(ctx, snapshot, id, attr, *std::move(fd));
}

absl::StatusOr<struct statx> StatNode(Context &ctx, InodeId id) {
  ABSL_ASSIGN_OR_RETURN(FileDescriptor fd,
                        OpenNode(ctx, id, O_PATH | O_NOFOLLOW));
  return syscalls::statx(*fd, "", AT_EMPTY_PATH, kAttrMask);
}

absl::Status RefreshAttrs(Context &ctx, InodeId id, struct statx *fetched) {
  events::Scope scope(*ctx.events, ctx, &ProtocolEvents::RefreshBegin,
                      &ProtocolEvents::RefreshEnd, id);
  // The frame's result is its End's status.
  return scope.Finish([&]() -> absl::Status {
    const cache::FillSnapshot snapshot = cache::BeginFill(ctx);
    ABSL_ASSIGN_OR_RETURN(struct statx stx, StatNode(ctx, id));
    // Model: the refresh's statx (GA_stat, ..., R_stat).
    ctx.events->AttrsStatted(ctx, id);
    if (fetched != nullptr) *fetched = stx;
    return FillAttrs(ctx, snapshot, id, stx);
  }());
}

absl::Status RefreshAttrsFromFd(Context &ctx, InodeId id, int fd,
                                struct statx *fetched) {
  events::Scope scope(*ctx.events, ctx, &ProtocolEvents::RefreshBegin,
                      &ProtocolEvents::RefreshEnd, id);
  // The frame's result is its End's status.
  return scope.Finish([&]() -> absl::Status {
    const cache::FillSnapshot snapshot = cache::BeginFill(ctx);
    ABSL_ASSIGN_OR_RETURN(
        struct statx stx, syscalls::statx(fd, "", AT_EMPTY_PATH, kAttrMask));
    // Model: as in RefreshAttrs.
    ctx.events->AttrsStatted(ctx, id);
    if (fetched != nullptr) *fetched = stx;
    return FillAttrs(ctx, snapshot, id, stx);
  }());
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

absl::StatusOr<std::vector<std::pair<std::string, std::string>>>
RefreshXattrs(Context &ctx, InodeId id) {
  const cache::FillSnapshot snapshot = cache::BeginFill(ctx);
  ABSL_ASSIGN_OR_RETURN(
      (std::vector<std::pair<std::string, std::string>> xattrs),
      ReadXattrs(ctx, id));
  ABSL_ASSIGN_OR_RETURN(bool filled,
                        cache::FillXattrs(ctx, snapshot, id, xattrs));
  if (!filled) {
    VLOG(1) << "inode " << id
            << ": not caching xattrs read concurrently with a mutation of it";
  }
  return xattrs;
}

namespace {

// Xattr `name` of the object `fd` (any fd, including O_PATH) refers to:
// its value, or nullopt if it does not exist (ENODATA; or ENOTSUP/
// EOPNOTSUPP, a filesystem without xattrs, as XattrsOf).
absl::StatusOr<std::optional<std::string>> XattrOf(int fd,
                                                   std::string_view name) {
  absl::StatusOr<std::string> value = syscalls::getxattr_opath(fd, name);
  if (!value.ok()) {
    int err = ErrnoOf(value.status());
    if (err == ENODATA || err == ENOTSUP || err == EOPNOTSUPP) {
      return std::nullopt;
    }
    return value.status();
  }
  return *std::move(value);
}

}  // namespace

absl::StatusOr<std::optional<std::string>> RefreshXattr(
    Context &ctx, InodeId id, std::string_view name,
    std::optional<int> open_fd) {
  const cache::FillSnapshot snapshot = cache::BeginFill(ctx);
  std::optional<FileDescriptor> opened;
  if (!open_fd.has_value()) {
    ABSL_ASSIGN_OR_RETURN(opened, OpenNode(ctx, id, O_PATH | O_NOFOLLOW));
    open_fd = **opened;
  }
  ABSL_ASSIGN_OR_RETURN(std::optional<std::string> value,
                        XattrOf(*open_fd, name));
  ABSL_ASSIGN_OR_RETURN(
      bool filled,
      cache::FillXattr(ctx, snapshot, id, name,
                       value.has_value()
                           ? std::optional<std::string_view>(*value)
                           : std::nullopt));
  if (!filled) {
    VLOG(1) << "inode " << id << ": not caching xattr " << EscapeBytes(name)
            << " read concurrently with a mutation of it";
  }
  return value;
}

namespace {

// Shared by SetXattr/RemoveXattr: an O_PATH fd on `id`, its type, and
// whichever real fd `apply` should use -- `open_fd` if given, else a fresh
// /proc reopen for a regular file/directory (freed automatically at the end
// of the calling statement). Special files use `opath_fd` itself via the
// *_opath syscalls instead of ever calling `apply`.
//
// If `read_back` is given, it is set, once the op has succeeded, to what
// the object now stores under `name` (XattrOf, through `opath_fd`, the fd
// the op itself used or one on the same object).
template <typename ApplyReal, typename ApplyOpath>
absl::Status ApplyXattrOp(Context &ctx, InodeId id, std::optional<int> open_fd,
                          std::string_view name, ApplyReal apply_real,
                          ApplyOpath apply_opath,
                          XattrReadBack *read_back = nullptr) {
  ABSL_ASSIGN_OR_RETURN(FileDescriptor opath_fd,
                        OpenNode(ctx, id, O_PATH | O_NOFOLLOW));
  ABSL_ASSIGN_OR_RETURN(
      struct statx stx, syscalls::statx(*opath_fd, "", AT_EMPTY_PATH, STATX_MODE));
  mode_t type = stx.stx_mode & S_IFMT;
  if (S_ISREG(type) || S_ISDIR(type)) {
    if (open_fd.has_value()) {
      ABSL_RETURN_IF_ERROR(apply_real(*open_fd));
    } else {
      ABSL_ASSIGN_OR_RETURN(
          FileDescriptor fd,
          syscalls::ReopenPathFd(*opath_fd, O_RDONLY | O_CLOEXEC));
      ABSL_RETURN_IF_ERROR(apply_real(*fd));
    }
  } else {
    ABSL_RETURN_IF_ERROR(apply_opath(*opath_fd));
  }
  if (read_back != nullptr) *read_back = XattrOf(*opath_fd, name);
  return absl::OkStatus();
}

}  // namespace

absl::StatusOr<XattrReadBack> SetXattr(Context &ctx, const Credentials &caller,
                                       InodeId id, std::string_view name,
                                       std::string_view value, int flags,
                                       std::optional<int> open_fd) {
  std::span<const uint8_t> value_bytes(
      reinterpret_cast<const uint8_t *>(value.data()), value.size());
  XattrReadBack read_back = std::nullopt;
  ABSL_RETURN_IF_ERROR(ApplyXattrOp(
      ctx, id, open_fd, name,
      [&](int fd) {
        return AsCaller(caller, [&] {
          return syscalls::fsetxattr(fd, name, value_bytes, flags);
        });
      },
      [&](int fd) {
        return AsCaller(caller, [&] {
          return syscalls::setxattr_opath(fd, name, value_bytes, flags);
        });
      },
      &read_back));
  return read_back;
}

absl::Status RemoveXattr(Context &ctx, const Credentials &caller, InodeId id,
                         std::string_view name, std::optional<int> open_fd) {
  return ApplyXattrOp(
      ctx, id, open_fd, name,
      [&](int fd) {
        return AsCaller(caller,
                        [&] { return syscalls::fremovexattr(fd, name); });
      },
      [&](int fd) {
        return AsCaller(caller,
                        [&] { return syscalls::removexattr_opath(fd, name); });
      });
}

absl::StatusOr<struct statvfs> StatFilesystem(Context &ctx, InodeId id) {
  ABSL_ASSIGN_OR_RETURN(cache::CachedAttr attr, cache::GetAttr(ctx, id));
  ABSL_ASSIGN_OR_RETURN(int mount_fd, ctx.mounts.Get(attr.device));
  return syscalls::fstatvfs(mount_fd);
}

namespace {

// Phase B of PopulateDirectory (and ResolveName) for one probed child of
// `dir`, inside the caller's transaction: upserts its row, recording its
// attributes, symlink target and xattrs only if it may be filled (see
// cache::CanFill; otherwise its attributes are left unknown), and links it
// into `dir` if `dir_ok`. Returns the child's row.
absl::StatusOr<InodeId> RecordChild(Context &ctx, cache::FillSnapshot snapshot,
                                    InodeId dir, const ChildRecord &child,
                                    bool dir_ok) {
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
      ABSL_RETURN_IF_ERROR(ReconcileAttrs(ctx, snapshot, cached_dentry.id,
                                          cached, child.stx));
    }
  }
  ABSL_ASSIGN_OR_RETURN(
      cache::UpsertResult row,
      cache::UpsertInode(ctx, child.handle, child.stx, child.backing_gen));
  const bool child_ok = cache::CanFill(ctx, snapshot, row.id);
  // UpsertInode marks the attributes current; a child that is open for
  // writing keeps them unknown (see WriteAttrs), as does one a mutation
  // may have changed since the probe.
  const bool filled = child_ok && !OpenForWrite(ctx, row.id);
  if (!filled) {
    ABSL_RETURN_IF_ERROR(cache::MarkAttrsUnknown(ctx, row.id));
  }
  // Model: a whole getattr fill of the child (its line once the caller's
  // transaction committed).
  ctx.events->ChildRowRecorded(ctx, dir, row.id, filled);
  if (S_ISDIR(child.stx.stx_mode)) {
    ABSL_RETURN_IF_ERROR(cache::EnsureDirectory(ctx, row.id));
  }
  if (dir_ok) {
    ABSL_RETURN_IF_ERROR(cache::LinkDentry(ctx, dir, child.name, row.id));
  }
  if (child_ok) {
    if (child.symlink_target.has_value()) {
      ABSL_RETURN_IF_ERROR(
          cache::SetSymlink(ctx, row.id, *child.symlink_target));
    }
    ABSL_RETURN_IF_ERROR(cache::ReplaceXattrs(ctx, row.id, child.xattrs));
  }
  return row.id;
}

}  // namespace

absl::StatusOr<Populated> PopulateDirectory(Context &ctx, InodeId dir) {
  // Phase A: all the I/O, no transaction.
  ABSL_ASSIGN_OR_RETURN(FileDescriptor dir_fd,
                        OpenNode(ctx, dir, O_RDONLY | O_DIRECTORY));
  // Taken after OpenNode, whose own ReconcileAttrs may legitimately clear
  // `dir`'s completeness first, and before the listing's I/O.
  const cache::FillSnapshot snapshot = cache::BeginFill(ctx);
  ABSL_ASSIGN_OR_RETURN(int64_t dir_epoch, cache::DirEpoch(ctx, dir));
  // Model: PopulateRead takes place here (the snapshot and the epoch; the
  // reads below return what the directory holds now: PopulateRead).
  ctx.events->PopulateStarted(ctx, dir);
  ABSL_ASSIGN_OR_RETURN(
      struct statx dir_stx,
      syscalls::statx(*dir_fd, "", AT_EMPTY_PATH, kMountIdMask));
  ABSL_ASSIGN_OR_RETURN(cache::CachedAttr dir_attr, cache::GetAttr(ctx, dir));
  ABSL_ASSIGN_OR_RETURN(std::vector<std::string> names, ReadDirNames(*dir_fd));

  // A child left out of `children` below is either a vanished dirent
  // (raced ENOENT), dropped entirely, or a refused mount/subvolume boundary
  // (see ProbeChild), collected into `refused_names` instead so phase B can
  // persist the refusal (cache::SetRefused) rather than just dropping the
  // name -- see this file's top comment on why a refused name must never be
  // cached as a plain negative entry.
  std::vector<ChildRecord> children;
  std::vector<std::string> refused_names;
  children.reserve(names.size());
  for (const std::string &name : names) {
    bool refused = false;
    ABSL_ASSIGN_OR_RETURN(
        std::optional<ChildRecord> child,
        ProbeChild(*dir_fd, dir_stx, dir_attr.device, dir, name, refused,
                   [](const events::Probe &) {}));
    if (child.has_value()) {
      children.push_back(*std::move(child));
    } else if (refused) {
      refused_names.push_back(name);
    }
  }
  VLOG(1) << "populating directory " << dir << ": " << children.size()
          << " entries, " << refused_names.size() << " refused";
  // Model: PopulateRead (the snapshot, the epoch, getdents64 and every
  // probe, as one step: see formal/README.md).
  ctx.events->PopulateRead(
      ctx, dir,
      [&](absl::FunctionRef<void(std::string_view, const events::Probe &)>
              each) {
        for (const ChildRecord &child : children) {
          each(child.name, ProbeOf(child.stx));
        }
        for (const std::string &name : refused_names) {
          each(name, {.kind = events::Probe::kRefused});
        }
      });

  // Phase B: one transaction, no syscalls. What it may record is decided
  // per inode (see cache::CanFill): `dir`'s dentries and completeness only
  // if no mutation of `dir` began, ended or is in flight since the snapshot
  // and nothing cleared its completeness since (its epoch), and each
  // child's attributes, symlink target and xattrs only if the same holds
  // for that child. Child rows are upserted regardless (their identity is
  // what the caller's reply needs; attributes that may be stale are left
  // unknown).
  Populated result;
  ABSL_RETURN_IF_ERROR(ctx.db.Transaction([&]() -> absl::Status {
    ABSL_ASSIGN_OR_RETURN(int64_t epoch_now, cache::DirEpoch(ctx, dir));
    const bool dir_ok =
        cache::CanFill(ctx, snapshot, dir) && epoch_now == dir_epoch;
    std::vector<std::string> seen;
    seen.reserve(children.size() + refused_names.size());
    for (const ChildRecord &child : children) {
      ABSL_ASSIGN_OR_RETURN(InodeId id,
                            RecordChild(ctx, snapshot, dir, child, dir_ok));
      result.entries[child.name] =
          cache::LookupResult{.kind = cache::LookupResult::kFound, .id = id};
      seen.push_back(child.name);
    }
    for (const std::string &name : refused_names) {
      if (dir_ok) {
        ABSL_RETURN_IF_ERROR(cache::SetRefused(ctx, dir, name));
      }
      result.entries[name] =
          cache::LookupResult{.kind = cache::LookupResult::kRefused, .id = 0};
      seen.push_back(name);
    }
    if (!dir_ok) {
      VLOG(1) << "directory " << dir
              << ": not caching a listing read concurrently with a mutation "
                 "of it";
      return absl::OkStatus();
    }
    ABSL_RETURN_IF_ERROR(cache::PruneDentriesNotIn(ctx, dir, seen));
    ABSL_RETURN_IF_ERROR(cache::MarkDirComplete(ctx, dir, true));
    result.cached = true;
    return absl::OkStatus();
  }));
  // Model: PopulateCommit.
  ctx.events->PopulateCommitted(ctx, dir, snapshot.seq, result.cached);
  return result;
}

namespace {

// The status LookupOrPopulate reports for a name cached as a refused
// mount/subvolume boundary (see cache::LookupResult::kRefused).
absl::Status ExdevBoundary() {
  return dcfs::ErrnoToStatus(
      EXDEV, "mount point or subvolume boundary (submounts are not "
             "supported; see README)");
}

}  // namespace

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
  events::Scope scope(*ctx.events, ctx, &ProtocolEvents::LookupBegin,
                      &ProtocolEvents::LookupEnd, parent, name);
  // The frame's result is its End's status.
  return scope.Finish([&]() -> absl::StatusOr<cache::LookupResult> {
    ABSL_ASSIGN_OR_RETURN(cache::LookupResult result,
                          cache::Lookup(ctx, parent, name));
    // `name` is a persisted refusal (see ProbeChild/cache::SetRefused): it
    // exists on the backing filesystem but dcfs will not cache across it, so
    // this must never be reported as absent -- answer EXDEV, straight from
    // the cache, every time. Checked before the kUnknown fast-out below since
    // kRefused is never kUnknown, but also never worth re-populating for.
    if (result.kind == cache::LookupResult::kRefused) {
      ctx.events->LookupDecided(ctx, parent, name,
                                events::LookupOutcome::kRefused, 0);
      return ExdevBoundary();
    }
    if (result.kind != cache::LookupResult::kUnknown) {
      // Model: LookupStep (or the request's Arrive) serving from the cache.
      ctx.events->LookupDecided(
          ctx, parent, name,
          result.kind == cache::LookupResult::kFound
              ? events::LookupOutcome::kFound
              : events::LookupOutcome::kNegative,
          result.id);
      return result;
    }

    // An unknown name in a complete listing (e.g. left by a mutation's phase
    // 1, or an invalidation): everything else is known, so resolve just it
    // (audit F7) rather than relisting the directory.
    ABSL_ASSIGN_OR_RETURN(bool listed, cache::ChildrenComplete(ctx, parent));
    // Model: LookupStep (or the request's Arrive) going to the backing
    // filesystem; no syscall since the cache read.
    ctx.events->LookupDecided(ctx, parent, name,
                              listed ? events::LookupOutcome::kResolve
                                     : events::LookupOutcome::kPopulate,
                              0);
    if (listed) return ResolveName(ctx, parent, name);

    ABSL_ASSIGN_OR_RETURN(Populated populated, PopulateDirectory(ctx, parent));
    if (!populated.cached) {
      // Not recorded (a concurrent mutation of `parent`): answer from the
      // listing itself, caching nothing about `name`.
      auto it = populated.entries.find(name);
      if (it == populated.entries.end()) {
        return cache::LookupResult{.kind = cache::LookupResult::kNegative,
                                   .id = 0};
      }
      if (it->second.kind == cache::LookupResult::kRefused) {
        return ExdevBoundary();
      }
      return it->second;
    }
    ABSL_ASSIGN_OR_RETURN(result, cache::Lookup(ctx, parent, name));
    if (result.kind == cache::LookupResult::kRefused) return ExdevBoundary();
    // A recorded listing names every child, and makes every other name
    // absent.
    RET_CHECK_NE(result.kind, cache::LookupResult::kUnknown)
        << "name " << EscapeBytes(name) << " of directory " << parent
        << " still unknown after its listing was recorded";
    return result;
  }());
}

absl::StatusOr<InodeId> ParentOf(Context &ctx, InodeId dir) {
  ABSL_ASSIGN_OR_RETURN(std::optional<InodeId> cached,
                        cache::ParentOf(ctx, dir));
  if (cached.has_value()) return *cached;

  // `dir`'s own dentry is unknown (audit F6): ask the backing filesystem.
  // A fill of the parent's row, as PopulateDirectory records a child.
  const cache::FillSnapshot snapshot = cache::BeginFill(ctx);
  ctx.events->ParentLookupStarted(ctx, dir);
  ABSL_ASSIGN_OR_RETURN(FileDescriptor dir_fd,
                        OpenNode(ctx, dir, O_PATH | O_DIRECTORY));
  ABSL_ASSIGN_OR_RETURN(cache::CachedAttr dir_attr, cache::GetAttr(ctx, dir));
  ABSL_ASSIGN_OR_RETURN(FileDescriptor up_fd,
                        syscalls::openat(*dir_fd, "..", O_PATH | O_DIRECTORY));
  ABSL_ASSIGN_OR_RETURN(struct statx stx,
                        syscalls::statx(*up_fd, "", AT_EMPTY_PATH, kAttrMask));
  // Submounts are refused, so ".." is on `dir`'s filesystem; `dir` is not
  // the root, so ".." is at most the root.
  ABSL_ASSIGN_OR_RETURN(cache::CachedAttr root_attr,
                        cache::GetAttr(ctx, cache::kRootInode));
  if (root_attr.device == dir_attr.device &&
      root_attr.backing_ino == stx.stx_ino) {
    return cache::kRootInode;
  }
  ABSL_ASSIGN_OR_RETURN(FileHandle handle,
                        FileHandle::FromFd(*up_fd, dir_attr.device));
  ABSL_ASSIGN_OR_RETURN(uint64_t gen, ReadGeneration(*up_fd, stx.stx_mode));
  VLOG(1) << "directory " << dir
          << ": dentry unknown, resolved its parent from the backing "
             "filesystem";
  InodeId parent = 0;
  bool filled = false;
  ABSL_RETURN_IF_ERROR(ctx.db.Transaction([&]() -> absl::Status {
    ABSL_ASSIGN_OR_RETURN(cache::UpsertResult row,
                          cache::UpsertInode(ctx, handle, stx, gen));
    filled = cache::CanFill(ctx, snapshot, row.id) && !OpenForWrite(ctx, row.id);
    if (!filled) {
      ABSL_RETURN_IF_ERROR(cache::MarkAttrsUnknown(ctx, row.id));
    }
    ABSL_RETURN_IF_ERROR(cache::EnsureDirectory(ctx, row.id));
    parent = row.id;
    return absl::OkStatus();
  }));
  // Model: a whole getattr fill of the parent (see formal/README.md).
  ctx.events->ParentRecorded(ctx, dir, parent, snapshot.seq, filled);
  return parent;
}

absl::StatusOr<cache::LookupResult> ResolveName(Context &ctx, InodeId parent,
                                                std::string_view name) {
  // As PopulateDirectory, for one name: open the parent, probe `name` in
  // it (openat O_PATH, statx, handle, generation, symlink target, xattrs),
  // and record it as present, absent (ENOENT) or refused (a boundary), as
  // a fill (cache::CanFill).
  ABSL_ASSIGN_OR_RETURN(FileDescriptor dir_fd,
                        OpenNode(ctx, parent, O_RDONLY | O_DIRECTORY));
  const cache::FillSnapshot snapshot = cache::BeginFill(ctx);
  ABSL_ASSIGN_OR_RETURN(
      struct statx dir_stx,
      syscalls::statx(*dir_fd, "", AT_EMPTY_PATH, kMountIdMask));
  ABSL_ASSIGN_OR_RETURN(cache::CachedAttr dir_attr,
                        cache::GetAttr(ctx, parent));
  bool refused = false;
  ABSL_ASSIGN_OR_RETURN(
      std::optional<ChildRecord> child,
      ProbeChild(*dir_fd, dir_stx, dir_attr.device, parent, name, refused,
                 [&](const events::Probe &probe) {
                   // Model: ResolveProbe.
                   ctx.events->ResolveProbed(ctx, parent, name, probe);
                 }));
  VLOG(1) << "resolving " << EscapeBytes(name) << " in directory " << parent;

  cache::LookupResult result;
  bool recorded = false;
  ABSL_RETURN_IF_ERROR(ctx.db.Transaction([&]() -> absl::Status {
    const bool dir_ok = cache::CanFill(ctx, snapshot, parent);
    recorded = dir_ok;
    if (child.has_value()) {
      ABSL_ASSIGN_OR_RETURN(InodeId id,
                            RecordChild(ctx, snapshot, parent, *child, dir_ok));
      result = {cache::LookupResult::kFound, id};
    } else if (refused) {
      if (dir_ok) {
        ABSL_RETURN_IF_ERROR(cache::SetRefused(ctx, parent, name));
      }
      result = {cache::LookupResult::kRefused, 0};
    } else {
      if (dir_ok) {
        ABSL_RETURN_IF_ERROR(cache::SetNegative(ctx, parent, name));
      }
      result = {cache::LookupResult::kNegative, 0};
    }
    if (!dir_ok) {
      VLOG(1) << "directory " << parent << ": not caching " << EscapeBytes(name)
              << ", read concurrently with a mutation of it";
    }
    return absl::OkStatus();
  }));
  // Model: ResolveCommit.
  ctx.events->ResolveCommitted(ctx, parent, name, snapshot.seq, recorded);
  if (result.kind == cache::LookupResult::kRefused) return ExdevBoundary();
  return result;
}

absl::StatusOr<NewChild> RecordNewChild(Context &ctx,
                                        const cache::Mutation &mutation,
                                        InodeId parent, int parent_fd,
                                        std::string_view name,
                                        bool open_for_write) {
  const cache::FillSnapshot snapshot = cache::BeginFill(ctx);
  // Phase A: I/O -- probe the object just created, exactly like ProbeChild
  // does for an existing directory entry, minus the mount-boundary check
  // (nothing can already be mounted on an object that did not exist a
  // moment ago).
  absl::StatusOr<FileDescriptor> child_fd =
      syscalls::openat(parent_fd, name, O_PATH | O_NOFOLLOW);
  if (!child_fd.ok()) {
    if (ErrnoOf(child_fd.status()) == ENOENT) {
      // Model: CreateProbe finding nothing.
      ctx.events->NewChildProbed(ctx, parent, name,
                                 {.kind = events::Probe::kAbsent});
    }
    return child_fd.status();
  }
  ABSL_ASSIGN_OR_RETURN(
      struct statx stx,
      syscalls::statx(**child_fd, "", AT_EMPTY_PATH, kAttrMask | kMountIdMask));
  // Model: CreateProbe (the read; the identity cannot change after it).
  ctx.events->NewChildProbed(ctx, parent, name, ProbeOf(stx));
  ABSL_ASSIGN_OR_RETURN(cache::CachedAttr parent_attr, cache::GetAttr(ctx, parent));
  ABSL_ASSIGN_OR_RETURN(
      ChildRecord record, ProbeObject(**child_fd, name, parent_attr.device, stx));

  // Phase B: one transaction, no syscalls.
  NewChild result;
  ABSL_RETURN_IF_ERROR(ctx.db.Transaction([&]() -> absl::Status {
    ABSL_ASSIGN_OR_RETURN(
        cache::UpsertResult row,
        cache::UpsertInode(ctx, record.handle, record.stx, record.backing_gen));
    // The new object is a fill (another request may already have reached
    // it, e.g. by listing `parent`); its dentry in `parent` is this
    // mutation's own phase-3 write.
    const bool child_ok = cache::CanFill(ctx, snapshot, row.id);
    if (S_ISDIR(record.stx.stx_mode)) {
      ABSL_RETURN_IF_ERROR(cache::EnsureDirectory(ctx, row.id));
    }
    if (S_ISDIR(record.stx.stx_mode) && child_ok) {
      // Unlike EnsureDirectory's usual caller (PopulateDirectory,
      // discovering a pre-existing, not-yet-listed subdirectory), a
      // directory RecordNewChild is recording was *just* created by this
      // same op: nothing has had a chance to put anything in it yet, so it
      // is already known to have zero children -- no separate populate is
      // needed before a listing of it can be served from the cache.
      ABSL_RETURN_IF_ERROR(cache::MarkDirComplete(ctx, row.id, true));
    }
    if (mutation.Owns(parent)) {
      ABSL_RETURN_IF_ERROR(cache::LinkDentry(ctx, parent, name, row.id));
    }
    if (child_ok) {
      if (record.symlink_target.has_value()) {
        ABSL_RETURN_IF_ERROR(
            cache::SetSymlink(ctx, row.id, *record.symlink_target));
      }
      ABSL_RETURN_IF_ERROR(cache::ReplaceXattrs(ctx, row.id, record.xattrs));
    }
    // The new row is dirty too: if a power loss keeps this transaction but
    // the backing filesystem loses the create, recovery must not serve its
    // attributes. (Its parent was made durably dirty by the caller's phase
    // 1; this insert cannot outlive the row, being in the same
    // transaction.)
    const InodeId new_id[] = {row.id};
    ABSL_RETURN_IF_ERROR(cache::MarkDirty(ctx, new_id));
    if (open_for_write || OpenForWrite(ctx, row.id) || !child_ok) {
      ABSL_RETURN_IF_ERROR(cache::MarkAttrsUnknown(ctx, row.id));
    }
    result = NewChild{.id = row.id, .fuse_gen = row.fuse_gen, .stx = record.stx};
    return absl::OkStatus();
  }));
  return result;
}

absl::Status MkdirAt(Context &ctx, const Credentials &caller, int parent_fd,
                     std::string_view name, mode_t mode) {
  return AsCaller(caller,
                  [&] { return syscalls::mkdirat(parent_fd, name, mode); });
}

absl::Status MknodAt(Context &ctx, const Credentials &caller, int parent_fd,
                     std::string_view name, mode_t mode, dev_t rdev) {
  return AsCaller(caller, [&] {
    return syscalls::mknodat(parent_fd, name, mode, rdev);
  });
}

absl::Status SymlinkAt(Context &ctx, const Credentials &caller, int parent_fd,
                       std::string_view name, std::string_view target) {
  return AsCaller(caller,
                  [&] { return syscalls::symlinkat(target, parent_fd, name); });
}

absl::StatusOr<FileDescriptor> CreateAt(Context &ctx, const Credentials &caller,
                                        int parent_fd, std::string_view name,
                                        int flags, mode_t mode) {
  return AsCaller(caller, [&] {
    return syscalls::openat(parent_fd, name, flags | O_CREAT, mode);
  });
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

absl::StatusOr<struct statx> RecordNewLink(Context &ctx,
                                           const cache::Mutation &mutation,
                                           InodeId src, InodeId newparent,
                                           std::string_view newname) {
  ABSL_ASSIGN_OR_RETURN(struct statx stx, StatNode(ctx, src));
  ABSL_RETURN_IF_ERROR(ctx.db.Transaction([&]() -> absl::Status {
    if (mutation.Owns(newparent)) {
      ABSL_RETURN_IF_ERROR(cache::LinkDentry(ctx, newparent, newname, src));
    }
    if (mutation.Owns(src)) return WriteAttrs(ctx, src, stx);
    return absl::OkStatus();
  }));
  return stx;
}

absl::Status UnlinkAt(Context &ctx, const Credentials &caller, InodeId parent,
                      std::string_view name, int flags) {
  ABSL_ASSIGN_OR_RETURN(FileDescriptor parent_fd,
                        OpenNode(ctx, parent, O_RDONLY | O_DIRECTORY));
  return AsCaller(caller,
                  [&] { return syscalls::unlinkat(*parent_fd, name, flags); });
}

absl::Status RenameAt(Context &ctx, const Credentials &caller, InodeId parent,
                      std::string_view name, InodeId newparent,
                      std::string_view newname, unsigned flags) {
  ABSL_ASSIGN_OR_RETURN(FileDescriptor parent_fd,
                        OpenNode(ctx, parent, O_RDONLY | O_DIRECTORY));
  ABSL_ASSIGN_OR_RETURN(FileDescriptor newparent_fd,
                        OpenNode(ctx, newparent, O_RDONLY | O_DIRECTORY));
  return AsCaller(caller, [&] {
    return syscalls::renameat2(*parent_fd, name, *newparent_fd, newname, flags);
  });
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

absl::StatusOr<struct statx> StatFd(int fd) {
  return syscalls::statx(fd, "", AT_EMPTY_PATH, kAttrMask);
}

absl::StatusOr<std::optional<std::string>> ReadXattrFd(int fd,
                                                       std::string_view name) {
  return XattrOf(fd, name);
}

absl::StatusOr<std::vector<std::pair<std::string, std::string>>> ReadXattrsFd(
    int fd) {
  return XattrsOf(fd);
}

absl::StatusOr<std::string> ReadSymlinkFd(int fd) {
  return syscalls::readlinkat(fd, "");
}

absl::Status StartupPurge(Context &ctx) {
  // Every non-source row left in the filesystems table is purged
  // unconditionally. Since amendment 12, ProbeChild/PopulateDirectory never
  // add one (a mount point or subvolume boundary is refused, not cached --
  // see LogRefusedBoundary/cache::SetRefused), so any row found here can
  // only be left over from a database built before that change, back when
  // such a boundary was cached like any other filesystem. Nothing recorded
  // there is ever
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
  events::Scope scope(*ctx.events, ctx, &ProtocolEvents::SyncBegin,
                      &ProtocolEvents::SyncEnd);
  // The frame's result is its End's status.
  return scope.Finish([&]() -> absl::Status {
    // Taken before the first syncfs: whatever is dirty now, is not mutated
    // again before ClearDirty and is not open for writing at either end is
    // covered by the syncfs calls below (see cache::BeginSync).
    ABSL_ASSIGN_OR_RETURN(cache::SyncSnapshot synced, cache::BeginSync(ctx));
    // Model: the sync request's first step (S1From), or StopSync.
    ctx.events->SyncSnapshotTaken(ctx);
    ctx.events->SyncfsStarting(ctx);
    for (int fd : ctx.mounts.Fds()) {
      ABSL_RETURN_IF_ERROR(syscalls::syncfs(fd));
    }
    ctx.events->SyncfsDone(ctx);
    std::vector<InodeId> keep;
    if (ctx.open_for_write != nullptr) {
      keep.assign(ctx.open_for_write->begin(), ctx.open_for_write->end());
    }
    ABSL_RETURN_IF_ERROR(cache::ClearDirty(ctx, synced, keep));
    // Model: SyncClearDirty, or StopClear.
    ctx.events->SyncCleared(ctx);
    return absl::OkStatus();
  }());
}

absl::Status StartRun(Context &ctx, std::string_view boot_id) {
  // Model: Crash (after an unclean shutdown) and Restart.
  ctx.events->RunStarting(ctx);
  ABSL_ASSIGN_OR_RETURN(bool clean, GetCleanShutdown(ctx.db));
  ABSL_ASSIGN_OR_RETURN(std::optional<std::string> last_boot_id,
                        GetBootId(ctx.db));
  const bool unclean = !clean;
  ABSL_ASSIGN_OR_RETURN(int64_t recovered, cache::RecoverDirty(ctx));
  // Model: Recover.
  ctx.events->Recovered(ctx);
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
  ABSL_RETURN_IF_ERROR(ctx.db.Transaction(
      [&]() -> absl::Status {
        ABSL_RETURN_IF_ERROR(SetCleanShutdown(ctx.db, false));
        return SetBootId(ctx.db, boot_id);
      },
      sqlite3::Durability::kSync));
  // Model: StartRun.
  ctx.events->RunStarted(ctx);
  return absl::OkStatus();
}

absl::Status FinishRun(Context &ctx) {
  // Model: BeginShutdown; the sync point below is StopSync and StopClear.
  ctx.events->ShutdownBegin(ctx);
  ABSL_RETURN_IF_ERROR(SyncBacking(ctx));
  ABSL_RETURN_IF_ERROR(ctx.db.Checkpoint());
  // Model: StopCkpt.
  ctx.events->Checkpointed(ctx);
  if (ctx.dirty.any) {
    return absl::FailedPreconditionError(
        "dirty cache entries remain (a writable open is still "
        "outstanding); leaving the clean-shutdown flag unset");
  }
  ABSL_RETURN_IF_ERROR(ctx.db.Transaction(
      [&] { return SetCleanShutdown(ctx.db, true); },
      sqlite3::Durability::kSync));
  // Model: StopFlag.
  ctx.events->CleanShutdownRecorded(ctx);
  return absl::OkStatus();
}

namespace {

// Bits SetAttr's mode-related branches (an explicit FUSE_SET_ATTR_MODE, or
// a KILL_SUID/KILL_SGID-only clear) share: reopening `opath_fd` as a real
// fd when the node is a regular file or directory (fchmod rejects O_PATH),
// else fchmod_opath -- except a symlink, which cannot be chmod'd at all on
// Linux (no lchmod).
//
// Runs as root, not AsCaller (see SetAttr's declaration comment): the
// kernel itself sends a mode change to kill setuid/setgid after a truncate
// or chown by a non-owner (fuse_setattr), which the caller could not make.
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
absl::Status ApplySize(const Credentials &caller, int opath_fd, mode_t type,
                       off_t size) {
  if (S_ISDIR(type)) {
    return dcfs::ErrnoToStatus(EISDIR, "truncate on a directory");
  }
  if (!S_ISREG(type)) {
    return dcfs::ErrnoToStatus(
        EINVAL, "truncate on a non-regular, non-directory file");
  }
  ABSL_ASSIGN_OR_RETURN(
      FileDescriptor fd, syscalls::ReopenPathFd(opath_fd, O_WRONLY | O_CLOEXEC));
  return AsCaller(caller, [&] { return syscalls::ftruncate(*fd, size); });
}

// FUSE_SET_ATTR_ATIME/MTIME(_NOW): the regular/dir-vs-other-types split as
// ApplyMode, but with no symlink exception -- futimens_opath is verified
// safe there (see syscalls.h).
absl::Status ApplyTimes(const Credentials &caller, int opath_fd, mode_t type,
                        const struct timespec times[2]) {
  if (S_ISREG(type) || S_ISDIR(type)) {
    ABSL_ASSIGN_OR_RETURN(
        FileDescriptor fd,
        syscalls::ReopenPathFd(
            opath_fd, O_RDONLY | O_NONBLOCK | O_NOCTTY | O_CLOEXEC));
    return AsCaller(caller, [&] { return syscalls::futimens(*fd, times); });
  }
  return AsCaller(caller,
                  [&] { return syscalls::futimens_opath(opath_fd, times); });
}

}  // namespace

absl::Status SetAttr(Context &ctx, const Credentials &caller, InodeId id,
                     const struct stat &attr, int to_set) {
  ABSL_ASSIGN_OR_RETURN(FileDescriptor fd, OpenNode(ctx, id, O_PATH | O_NOFOLLOW));
  ABSL_ASSIGN_OR_RETURN(
      struct statx stx, syscalls::statx(*fd, "", AT_EMPTY_PATH, STATX_MODE));
  mode_t type = stx.stx_mode & S_IFMT;

  if (to_set & FUSE_SET_ATTR_SIZE) {
    ABSL_RETURN_IF_ERROR(ApplySize(caller, *fd, type, attr.st_size));
  }

  if (to_set & (FUSE_SET_ATTR_UID | FUSE_SET_ATTR_GID)) {
    uid_t uid = (to_set & FUSE_SET_ATTR_UID) ? attr.st_uid
                                             : static_cast<uid_t>(-1);
    gid_t gid = (to_set & FUSE_SET_ATTR_GID) ? attr.st_gid
                                             : static_cast<gid_t>(-1);
    // fchownat works on an O_PATH fd via AT_EMPTY_PATH with an empty path.
    ABSL_RETURN_IF_ERROR(AsCaller(caller, [&] {
      return syscalls::fchownat(*fd, "", uid, gid, AT_EMPTY_PATH);
    }));
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
    ABSL_RETURN_IF_ERROR(ApplyTimes(caller, *fd, type, times));
  }

  // Nothing to set but (perhaps) ctime: a chown(path, -1, -1) (see
  // SetAttr's declaration comment). The ctime value is never used.
  if ((to_set & ~FUSE_SET_ATTR_CTIME) == 0) {
    ABSL_RETURN_IF_ERROR(AsCaller(caller, [&] {
      return syscalls::fchownat(*fd, "", static_cast<uid_t>(-1),
                                static_cast<gid_t>(-1), AT_EMPTY_PATH);
    }));
  }
  return absl::OkStatus();
}

}  // namespace dcfs::backing
