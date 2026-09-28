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
// from GetDeviceId (dcfs requires a kernel with FS_IOC_GETFSUUID and runs
// as root; see the project's root/kernel design decision).
namespace dcfs::backing {

using cache::InodeId;

// What RecordNewChild learns about (and records for) a freshly created
// child: its nodeid, the fuse_gen of the row it now has (0 if it already
// existed -- see UpsertInode -- which a truly fresh child never does, but
// RecordNewChild does not assume that), and its fresh attributes.
struct NewChild {
  InodeId id = 0;
  uint32_t fuse_gen = 0;
  struct statx stx {};
};

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

// As RefreshAttrs, but statx's an fd the caller already has open on `id`
// (a real, non-O_PATH fd is not required -- AT_EMPTY_PATH works on O_PATH
// too) instead of reopening it via OpenNode. Used by DirCacheFS::Release
// when the open being released was writable, to pick up the passthrough
// writes' effect on size/mtime/etc. before the fd closes.
absl::Status RefreshAttrsFromFd(Context &ctx, InodeId id, int fd);

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

// --- Create-family ops (step 4.2) ------------------------------------------
//
// Each pairs with a DirCacheFS write-through op: the op's phase 2 (the
// backing syscall(s), below) followed by its phase 3 (RecordNewChild /
// RecordNewLink). Every one of these does its own I/O directly -- callers
// never issue a syscall inside a cache transaction (see backing.cc's file
// comment).

// Probes the child `parent_fd`/`name` names -- which the caller has just
// created via MkdirAt/MknodAt/SymlinkAt/CreateAt -- exactly like ProbeChild
// probes an existing directory entry (open O_PATH|O_NOFOLLOW, statx
// including mount id, handle, inode generation, symlink target, xattrs),
// skipping only the mount-boundary check (a freshly created object cannot
// already have something mounted on it), and then, in ONE transaction:
// UpsertInode, EnsureDirectory (if a directory), LinkDentry, SetSymlink (if
// a symlink), and ReplaceXattrs. `parent_fd` must already be open on
// `parent` (O_RDONLY|O_DIRECTORY is enough); reusing it here, rather than
// reopening `parent`, is why every MkdirAt/MknodAt/SymlinkAt/CreateAt below
// takes a `parent_fd` instead of resolving `parent` itself.
absl::StatusOr<NewChild> RecordNewChild(Context &ctx, InodeId parent,
                                        int parent_fd, std::string_view name);

// mkdirat(2)/mknodat(2)/symlinkat(2) of `name` inside the already-open
// `parent_fd`. The kernel applies umask to `mode` before it reaches us, so
// it is passed straight through.
absl::Status MkdirAt(Context &ctx, int parent_fd, std::string_view name,
                     mode_t mode);
absl::Status MknodAt(Context &ctx, int parent_fd, std::string_view name,
                     mode_t mode, dev_t rdev);
absl::Status SymlinkAt(Context &ctx, int parent_fd, std::string_view name,
                       std::string_view target);

// openat(2) of `name` inside the already-open `parent_fd`, with O_CREAT
// added to whatever the kernel sent in `flags` (which already carries
// O_EXCL when the caller asked for it); the returned fd is the new file,
// open exactly as the caller requested, ready for DirCacheFS::Create to
// hand to PassthroughOpen.
absl::StatusOr<FileDescriptor> CreateAt(Context &ctx, int parent_fd,
                                        std::string_view name, int flags,
                                        mode_t mode);

// linkat(2) of `src` as `newname` inside `newparent`: opens `src` O_PATH and
// `newparent` real/O_RDONLY|O_DIRECTORY internally (both via OpenNode) and
// calls linkat(src_fd, "", newparent_fd, newname, AT_EMPTY_PATH) -- dcfs
// runs as root (CAP_DAC_READ_SEARCH), which is what permits the
// AT_EMPTY_PATH/oldpath="" form on an O_PATH fd. Whatever linkat(2) itself
// returns is returned unchanged, including EXDEV for a cross-filesystem
// link.
absl::Status LinkAt(Context &ctx, InodeId src, InodeId newparent,
                    std::string_view newname);

// After LinkAt(src, newparent, newname) has succeeded: re-statx's `src`
// (I/O, no transaction -- its nlink just changed) and then, in one
// transaction, LinkDentry(newparent, newname, src) followed by
// UpdateAttr(src, <the fresh statx>), so the new dentry and the bumped
// nlink land together. Returns the fresh attributes.
absl::StatusOr<struct statx> RecordNewLink(Context &ctx, InodeId src,
                                           InodeId newparent,
                                           std::string_view newname);

// Run at startup, after InitRoot: forgets every non-source filesystem that
// is no longer mounted where it was found (or whose mount point is gone),
// and registers a mount fd for each one that still is.
absl::Status StartupPurge(Context &ctx);

// Applies `attr`'s `to_set` fields (the FUSE_SET_ATTR_* bitmask from a
// setattr request) to `id` on the backing filesystem, via an O_PATH fd
// from OpenNode (so `id`'s identity is verified first, as everywhere
// else here). Does not touch the cache -- the caller runs this between
// its own MarkAttrsUnknown and RefreshAttrs (see the write-through rule
// in this file's top comment), and no cache write may happen in here
// since a transaction may never span a syscall.
//
// Applied in the order size, owner (uid/gid), mode, times: a failing
// truncate must not leave mode/owner already changed, and chown (which on
// some filesystems silently clears the setuid/setgid bits) is done before
// mode so an explicit FUSE_SET_ATTR_MODE always wins. Returns the first
// failing status (with its errno payload intact); anything already
// applied before that point stays applied on the backing filesystem -- the
// cache is left "unknown" by the caller's phase 1 regardless, so the next
// access re-reads the true (possibly partial) result rather than ever
// reporting stale data.
//
// FUSE_SET_ATTR_CTIME is ignored: ctime cannot be set directly, and the
// kernel only ever sends it alongside another flag. FUSE_SET_ATTR_MODE
// dispatches on the node's current type: regular files and directories
// are chmod'd through a reopened non-O_PATH fd (fchmod rejects O_PATH);
// FIFOs, sockets and devices go through fchmod_opath (reopening one of
// those for a real fd could block or have a side effect); a symlink's
// mode cannot be changed at all on Linux (there is no lchmod) and this
// fails with EOPNOTSUPP. FUSE_SET_ATTR_KILL_SUID/KILL_SGID without
// FUSE_SET_ATTR_MODE clears S_ISUID/S_ISGID from the node's *current*
// mode instead (the kernel already cleared them in attr.st_mode whenever
// it also sent MODE, so that combination needs no extra handling here);
// it is a no-op on a symlink, which has no meaningful mode bits to clear.
// Owner and times use the same regular/dir-vs-other-types split as mode,
// except that both are unremarkable on a symlink (unlike mode); size is
// only ever valid for a regular file -- EISDIR/EINVAL otherwise, as the
// kernel itself would report.
absl::Status SetAttr(
    Context &ctx, InodeId id, const struct stat &attr, int to_set);

}  // namespace dcfs::backing

#endif  // DCFS_BACKING_H_
