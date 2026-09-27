#ifndef DCFS_METADATA_CACHE_H_
#define DCFS_METADATA_CACHE_H_

#include <sys/stat.h>
#include <time.h>

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "absl/functional/function_ref.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "dcfs/context.h"
#include "dcfs/device_id.h"
#include "dcfs/file_handle.h"

// The metadata cache: typed reads and writes over the SQLite schema in
// dcfs/schema.sql. This layer is pure SQLite -- it performs no syscalls and
// never touches ctx.mounts; talking to the backing filesystem is the job of
// the layer above it.
//
// These are free functions rather than methods because the cache has no
// state of its own: all state lives in ctx.db (and statements are cached by
// the Connection, keyed by SQL text).
//
// Every write function runs as exactly one ctx.db.Transaction(), so it is
// atomic on its own, and nests (as a savepoint) inside a caller's
// transaction when the caller needs several writes to commit together.
//
// Identity model (see also schema.sql):
//  - An InodeId is the FUSE nodeid, never reused.
//  - A row's backing identity is (device, backing_ino, backing_gen); hard
//    links to one backing file share one row.
//  - "Invalidating" a row deletes it and every dentry pointing at it, and
//    marks each such dentry's parent incomplete. Its dentries are deleted
//    rather than made negative because what we know about those names
//    afterwards is "unknown", not "absent".
//  - A dentry whose inode is NULL is a cached negative entry.
//  - The number of cached dentries for an inode is not its link count (only
//    some of its links may be cached), so removing a dentry never deletes
//    the inode row by itself; see DeleteInode().
namespace dcfs::cache {

using InodeId = int64_t;
inline constexpr InodeId kRootInode = 1;

struct CachedAttr {
  // False when the row's attributes are not known to be current
  // (inodes.attrs_valid = 0); `st` then holds the last cached values, if
  // any, and should not be served.
  bool valid = false;
  // st_ino is the backing inode number and st_dev is 0 (the FUSE layer does
  // not report it); every other field comes from the row.
  struct stat st {};
  // struct stat has no birth time, so it is reported separately.
  struct timespec btime {};
  uint32_t fuse_gen = 0;
  DeviceId device;
  uint64_t backing_ino = 0;
  uint64_t backing_gen = 0;
};

struct LookupResult {
  enum Kind {
    kFound,     // A positive dentry; `id` is its inode.
    kNegative,  // A cached negative dentry: the name is known to be absent.
    kUnknown,   // Nothing cached for this name.
  };
  Kind kind = kUnknown;
  // Meaningful only for kFound; 0 otherwise.
  InodeId id = 0;
};

struct UpsertResult {
  InodeId id = 0;
  uint32_t fuse_gen = 0;
  // True iff a new row (and so a new nodeid) was created.
  bool created = false;
};

struct FilesystemRow {
  DeviceId device;
  int64_t fstype = 0;
  // The directory containing this filesystem's mount point, and the name of
  // the mount point within it. Both nullopt for the source filesystem.
  std::optional<InodeId> parent_inode;
  std::optional<std::string> boundary_name;
};

// --- Reads ----------------------------------------------------------------

absl::StatusOr<LookupResult> Lookup(Context &ctx, InodeId parent,
                                    std::string_view name);

// NotFound if there is no row for `id` (e.g. it was invalidated; the FUSE
// layer answers ESTALE).
absl::StatusOr<CachedAttr> GetAttr(Context &ctx, InodeId id);

// NotFound if there is no row for `id`.
absl::StatusOr<uint32_t> GetGeneration(Context &ctx, InodeId id);

// Callback for ListDir. `next_cursor` is the cursor to pass to ListDir to
// resume after this entry. Return true to continue, false to stop.
using ListDirCallback = absl::FunctionRef<absl::StatusOr<bool>(
    std::string_view name, InodeId child, int64_t next_cursor)>;

// Calls `cb` for each positive dentry of `dir` after `cursor` (0 starts
// from the beginning), in a stable order: cursors are dentry rowids, and
// updating an existing dentry in place (LinkDentry/RenameDentry onto an
// existing name) keeps its rowid. Negative entries are skipped. `cb` may
// call other cache functions, including writes: no statement is held open
// across a callback.
absl::Status ListDir(Context &ctx, InodeId dir, int64_t cursor,
                     ListDirCallback cb);

// Whether every entry of `dir` is cached (a missing directories row counts
// as incomplete).
absl::StatusOr<bool> IsDirComplete(Context &ctx, InodeId dir);

// NotFound if no target is cached for `id`.
absl::StatusOr<std::string> Readlink(Context &ctx, InodeId id);

// The names of all of `id`'s xattrs, sorted; nullopt if the set is not
// completely cached. NotFound if there is no row for `id`.
absl::StatusOr<std::optional<std::vector<std::string>>> ListXattrs(
    Context &ctx, InodeId id);

// The cached value of xattr `name`. If it is not cached: NotFound when the
// xattr set is complete (so it is known not to exist), nullopt when it is
// incomplete (unknown). Also NotFound if there is no row for `id`.
absl::StatusOr<std::optional<std::string>> GetXattr(Context &ctx, InodeId id,
                                                    std::string_view name);

// NotFound if there is no row for `id` or the row has no handle.
absl::StatusOr<FileHandle> GetHandle(Context &ctx, InodeId id);

// The directory containing directory `dir`: the parent of the one positive
// dentry pointing at it. The root is its own parent. NotFound if no dentry
// for `dir` is cached.
absl::StatusOr<InodeId> ParentOf(Context &ctx, InodeId dir);

// All filesystems, in the order they were added.
absl::StatusOr<std::vector<FilesystemRow>> ListFilesystems(Context &ctx);

// NotFound if `device` is not registered.
absl::StatusOr<FilesystemRow> GetFilesystem(Context &ctx,
                                            const DeviceId &device);

// --- Writes (each is one transaction) --------------------------------------

// Records the backing object identified by (handle.device, stx.stx_ino,
// backing_gen), with attributes from `stx` (now valid) and handle `handle`.
// If that identity already has a row, it is updated. Otherwise any row for
// the same (device, ino) with a different generation -- i.e. the backing
// filesystem recycled the inode number -- is invalidated, and a new row is
// created with a freshly minted fuse_gen. handle.device must already be
// registered via AddFilesystem(). Not for the root; see UpsertRoot().
absl::StatusOr<UpsertResult> UpsertInode(Context &ctx,
                                         const FileHandle &handle,
                                         const struct statx &stx,
                                         uint64_t backing_gen);

// Updates the root row (id 1) in place with the root's current backing
// identity, attributes and handle. Never creates a row and never changes
// its fuse_gen (always 0). handle.device must be the source device.
absl::Status UpsertRoot(Context &ctx, const FileHandle &handle,
                        const struct statx &stx, uint64_t backing_gen);

// Points (parent, name) at `child`, replacing whatever was cached for that
// name (including a negative entry). NotFound if either row is missing.
absl::Status LinkDentry(Context &ctx, InodeId parent, std::string_view name,
                        InodeId child);

// Caches (parent, name) as known-absent. NotFound if `parent` is missing.
absl::Status SetNegative(Context &ctx, InodeId parent, std::string_view name);

// Forgets (parent, name) if cached. Never deletes the inode row it pointed
// at, since other (possibly uncached) links may remain.
absl::Status UnlinkDentry(Context &ctx, InodeId parent, std::string_view name);

// Moves the positive entry (parent, name) to (newparent, newname),
// replacing any entry cached there. The source name is forgotten (not made
// negative). NotFound if no positive entry is cached for the source.
absl::Status RenameDentry(Context &ctx, InodeId parent, std::string_view name,
                          InodeId newparent, std::string_view newname);

// Records whether every entry of `dir` is cached.
absl::Status MarkDirComplete(Context &ctx, InodeId dir, bool complete);

// Forgets `names` in `parent` and marks `parent` incomplete. Phase 1 of a
// two-phase mutation: done before the backing operation, so that a crash
// between the two leaves "unknown" rather than stale state.
absl::Status MarkUnknown(Context &ctx, InodeId parent,
                         std::span<const std::string> names);

// Marks `id`'s cached attributes as not current. NotFound if no row.
absl::Status MarkAttrsUnknown(Context &ctx, InodeId id);

// Replaces `id`'s cached attributes with `stx` and marks them current.
// NotFound if no row.
absl::Status UpdateAttr(Context &ctx, InodeId id, const struct statx &stx);

// Caches `id`'s symlink target. NotFound if no row.
absl::Status SetSymlink(Context &ctx, InodeId id, std::string_view target);

// Replaces `id`'s cached xattrs with exactly `xattrs` and marks the set
// complete. NotFound if no row.
absl::Status ReplaceXattrs(
    Context &ctx, InodeId id,
    std::span<const std::pair<std::string, std::string>> xattrs);

// Caches one xattr. Does not change whether the set is complete. NotFound
// if no row.
absl::Status SetXattr(Context &ctx, InodeId id, std::string_view name,
                      std::string_view value);

// Forgets one xattr (a no-op if not cached). If the set was complete, it
// remains complete, now without `name`.
absl::Status RemoveXattr(Context &ctx, InodeId id, std::string_view name);

// Forgets all of `id`'s xattrs and marks the set incomplete.
absl::Status MarkXattrsUnknown(Context &ctx, InodeId id);

// Invalidates `id` (see the identity model above): deletes the row and every
// dentry pointing at it, and marks those dentries' parents incomplete.
// Cascades remove its directories/symlinks/xattrs rows, its child dentries,
// and any filesystem mounted inside it. `id` must not be the root.
// NotFound if no row.
absl::Status InvalidateInode(Context &ctx, InodeId id);

// Removes `id` once its backing object is gone (nlink reached 0). The cache
// effect is identical to InvalidateInode(); this name exists so that the
// FUSE layer's call sites say which situation they are handling.
absl::Status DeleteInode(Context &ctx, InodeId id);

// Registers a filesystem. AlreadyExists if `device` is already registered;
// NotFound if `parent` is given but has no row.
absl::Status AddFilesystem(Context &ctx, const DeviceId &device,
                           int64_t fstype, std::optional<InodeId> parent,
                           std::optional<std::string> boundary_name);

// Forgets everything cached about filesystem `device`: its row, and by
// cascade its inodes (with their dentries, xattrs, symlinks, directories)
// and any filesystems mounted beneath it. Its boundary dentry is forgotten
// (not left negative) and the boundary's parent marked incomplete, so the
// mount point is rediscovered on the next lookup. Must not be the source
// device. NotFound if `device` is not registered.
absl::Status PurgeFilesystem(Context &ctx, const DeviceId &device);

}  // namespace dcfs::cache

#endif  // DCFS_METADATA_CACHE_H_
