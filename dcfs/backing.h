#ifndef DCFS_BACKING_H_
#define DCFS_BACKING_H_

#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/types.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "dcfs/context.h"
#include "dcfs/fd.h"
#include "dcfs/metadata_cache.h"
#include "dcfs/migrate.h"

// The backing layer: the only code that talks to the backing filesystems
// (dcfs::syscalls::) or uses ctx.mounts for I/O, and the place where the
// population policy lives. Everything above it reads the metadata cache;
// everything here does its I/O first and then records the result in ONE
// short cache transaction, never issuing a syscall inside a transaction.
//
// After startup no paths are used: objects are reopened from their cached
// file handles (FileHandle::Open) and children are reached with openat()
// relative to an fd on their directory. Filesystem identity always comes
// from ctx.device_id_fn.
namespace dcfs::backing {

using cache::InodeId;

// Probes the source root `source_fd` (any fd on the source directory,
// including O_PATH) for the identity Migrate() seeds a fresh cache with.
absl::StatusOr<RootIdentity> ProbeRoot(Context &ctx, int source_fd);

// Binds an already-migrated cache to the source root: checks that the
// database was built for this filesystem (FailedPrecondition otherwise),
// refreshes the root row's handle and attributes, and registers
// `source_fd` as the source filesystem's mount fd. `source_fd` must be a
// real (non-O_PATH) fd on the source directory: open_by_handle_at's mount
// fd argument is resolved via the kernel's non-raw fd class (fs/fhandle.c
// get_path_from_fd()), which rejects O_PATH descriptors with EBADF.
absl::Status InitRoot(Context &ctx, FileDescriptor source_fd);

// The inode generation (FS_IOC_GETVERSION) of the object `opath_fd` refers
// to, whose file type is `mode & S_IFMT`. 0 means "unknown": the
// filesystem has no generations, or the object cannot be reopened to ask
// (no read permission). Only regular files and directories are asked;
// symlinks, FIFOs, sockets and devices cannot be safely reopened for an
// ioctl, so they are always 0 and recycled-inode detection does not cover
// them.
absl::StatusOr<uint64_t> ReadGeneration(int opath_fd, mode_t mode);

// Reopens inode `id` with open(2) `flags`, after checking that the object
// reached is still the one the row describes (same inode number, and same
// generation when both are known). If it is not, or the kernel reports the
// handle stale, the row is invalidated and the result is an ESTALE status.
absl::StatusOr<FileDescriptor> OpenNode(Context &ctx, InodeId id, int flags);

// Fresh attributes of `id` from the backing filesystem (STATX_BASIC_STATS
// and STATX_BTIME). Does not update the cache.
absl::StatusOr<struct statx> StatNode(Context &ctx, InodeId id);

// Refreshes `id`'s cached attributes from the backing filesystem (StatNode
// followed by cache::UpdateAttr). Called when CachedAttr.valid is false.
absl::Status RefreshAttrs(Context &ctx, InodeId id);

// Reads up to `size` bytes at `offset` from `fd` (a real, non-O_PATH fd
// already open on the node -- see DirCacheFS::Open's OpenNode call),
// looping over short reads until `size` bytes have been read or EOF. The
// fallback DirCacheFS::Read uses when the kernel did not grant
// FUSE_CAP_PASSTHROUGH for this open, so `fd` is not reopened here: unlike
// every other backing:: function this one takes an fd instead of an
// InodeId, because the caller already has one open and identity-verified.
absl::StatusOr<std::string> ReadFile(int fd, size_t size, off_t offset);

// The target of symlink `id`, read from the backing filesystem.
absl::StatusOr<std::string> ReadSymlink(Context &ctx, InodeId id);

// All extended attributes of `id` as (name, value), read from the backing
// filesystem; empty if the filesystem does not support xattrs.
absl::StatusOr<std::vector<std::pair<std::string, std::string>>> ReadXattrs(
    Context &ctx, InodeId id);

// Refreshes `id`'s cached xattr set from the backing filesystem (ReadXattrs
// followed by cache::ReplaceXattrs). Called when the cached set is unknown
// (ListXattrs/GetXattr returned nullopt).
absl::Status RefreshXattrs(Context &ctx, InodeId id);

// The statvfs of the filesystem `id` lives on, from that filesystem's mount
// fd -- no handle open of `id` itself is needed.
absl::StatusOr<struct statvfs> StatFilesystem(Context &ctx, InodeId id);

// Lists directory `dir` on the backing filesystem and caches all of it:
// every child's inode row (attributes, handle, generation, symlink target,
// xattrs), its dentry, and any filesystem mounted on a child; forgets
// cached names that no longer exist; and marks `dir` complete. This is the
// one point where a directory's disk is read, once, so later lookups and
// listings are answered from the cache.
absl::Status PopulateDirectory(Context &ctx, InodeId dir);

// Looks `name` up in `parent`, populating `parent` first if the cache
// cannot answer. Never returns kUnknown: a name absent after a complete
// listing is cached as negative.
absl::StatusOr<cache::LookupResult> LookupOrPopulate(Context &ctx,
                                                     InodeId parent,
                                                     std::string_view name);

// Run at startup, after InitRoot: forgets every non-source filesystem that
// is no longer mounted where it was found (or whose mount point is gone),
// and registers a mount fd for each one that still is.
absl::Status StartupPurge(Context &ctx);

}  // namespace dcfs::backing

#endif  // DCFS_BACKING_H_
