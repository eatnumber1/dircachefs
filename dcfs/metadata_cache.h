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
//  - A dentry whose inode is NULL is a cached negative entry, UNLESS its
//    `refused` bit is set: that means the opposite -- the name exists but
//    dcfs refuses to cache it (a mount point or subvolume boundary below
//    --source, see amendment 12 and README.md's Limitations) -- so it must
//    never be reported as absent. See LookupResult::kRefused and
//    SetRefused().
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
    kRefused,   // The name exists but is a refused mount/subvolume boundary
                // (amendment 12): it must never be reported absent.
                // backing::LookupOrPopulate turns this into EXDEV.
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

// Xattrs: each name is present (with a value), absent, or unknown. A name
// with an xattrs row has the row's state; a name without one is absent if
// the set is complete (xattrs_complete: the name set is known) and unknown
// otherwise. Only a full listing (ReplaceXattrs) makes the set complete,
// and only MarkXattrsUnknown/RecoverDirty make it incomplete; single-name
// writes (ForgetXattr/SetXattr/RemoveXattr) leave it alone.

// The names of all of `id`'s present xattrs, sorted; nullopt if the set is
// not complete or any name in it is unknown (a listing must not omit a
// name that may exist). NotFound if there is no row for `id`.
absl::StatusOr<std::optional<std::vector<std::string>>> ListXattrs(
    Context &ctx, InodeId id);

// The value of xattr `name` if present; NotFound if absent; nullopt if
// unknown (see above). Also NotFound if there is no row for `id`.
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
// If that identity already has a row describing the same object -- its
// stored handle (handle_type and bytes) equals `handle`, and its birth time
// equals stx's when both are known (nonzero) -- that row is updated.
// Otherwise every row for the same (device, ino) -- the backing filesystem
// recycled the inode number, possibly with the same or no generation -- is
// invalidated, and a new row is
// created with a fresh fuse_gen drawn from ctx.rng (uniformly random, never
// 0). handle.device must already be
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

// Caches (parent, name) as a refused mount/subvolume boundary (amendment
// 12): the name exists on the backing filesystem but dcfs will not cache
// across it. Unlike SetNegative, this must never be read back as "absent" --
// see LookupResult::kRefused and backing::LookupOrPopulate, which turns it
// into EXDEV. Replaces whatever was cached for that name, same as
// LinkDentry/SetNegative. NotFound if `parent` is missing.
absl::Status SetRefused(Context &ctx, InodeId parent, std::string_view name);

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

// Gives directory `dir` a directories row (children_complete 0) if it has
// none; an existing row, and so its completeness, is left alone. Used when
// a directory is discovered, so that re-discovering an already-populated
// one does not throw its listing away. NotFound if no row for `dir`.
absl::Status EnsureDirectory(Context &ctx, InodeId dir);

// Forgets every dentry of `dir`, positive, negative or refused, whose name
// is not in `names`: after a full listing of the backing directory, those
// names are known not to exist any more. The caller is responsible for
// including a still-refused boundary's name in `names` (it still exists,
// just uncached), so that only a genuinely vanished name is pruned. Inode
// rows are left alone (see UnlinkDentry).
absl::Status PruneDentriesNotIn(Context &ctx, InodeId dir,
                                std::span<const std::string> names);

// Forgets `names` in `parent` and marks `parent` incomplete. Phase 1 of a
// two-phase mutation: done before the backing operation, so that a crash
// between the two leaves "unknown" rather than stale state.
absl::Status MarkUnknown(Context &ctx, InodeId parent,
                         std::span<const std::string> names);

// Forgets every cached negative or refused dentry of `dir` (see
// LookupResult::kRefused) and marks `dir` incomplete, leaving its positive
// dentries alone. For a directory found to have changed on the backing
// filesystem behind dcfs's back (see backing::ReconcileAttrs): a new name
// may have appeared that a negative entry would otherwise keep hiding, a
// refused boundary may no longer be one (or a new one may have appeared),
// but the names already cached positive still point at their objects (a
// repopulation corrects any that do not), and keeping them keeps a
// subdirectory's ".." resolvable meanwhile. Dropping a refused dentry here
// never causes a false "absent" reading (see the identity-model note
// above): `dir` is marked incomplete in the same transaction, so the next
// lookup or readdir repopulates it rather than ever answering from the
// gap. NotFound if no row for `dir`.
absl::Status ForgetNegativeDentries(Context &ctx, InodeId dir);

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

// Records xattr `name` as present with `value`: phase 3 of Setxattr, with
// the value read back from the backing filesystem. Does not change whether
// the set is complete. NotFound if no row.
absl::Status SetXattr(Context &ctx, InodeId id, std::string_view name,
                      std::string_view value);

// Records xattr `name` as absent: phase 3 of Removexattr (or of a
// Setxattr whose read-back found nothing stored). Does not change whether
// the set is complete. NotFound if no row.
absl::Status RemoveXattr(Context &ctx, InodeId id, std::string_view name);

// Records xattr `name` as unknown (inserting a row if it has none, so this
// holds even while the set is complete): phase 1 of a mutation that is
// about to change it, done before the backing syscall, so that a reader
// meanwhile, a crash, or a failed phase 2 never sees `name`'s old value
// (or its absence) as authoritative. Every other name keeps its state;
// ListXattrs returns nullopt until the name is resolved (by phase 3 or a
// refresh). NotFound if there is no row for `id`.
absl::Status ForgetXattr(Context &ctx, InodeId id, std::string_view name);

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

// --- The durable dirty set (see schema.sql's `dirty` table) --------------

// Phase 1 of a mutation: runs `body` (the "mark unknown" writes) and records
// `ids` -- every inode whose cached attributes, dentries (as a parent),
// symlink target or xattrs the mutation is about to change -- in the dirty
// set, in ONE transaction that is durable (sqlite3::Durability::kSync, a WAL
// fsync) before this returns, so before the caller's backing syscall. After
// a power loss, whatever the backing filesystems kept of the syscall, the
// database then either has these ids dirty (startup recovery forgets their
// cached state) or the syscall had not been issued yet.
//
// If every id is already in ctx.dirty.durable (already durably dirty since
// the last sync point), the transaction commits at the default durability
// instead: recovery would forget all of those ids' state anyway, so nothing
// `body` writes needs to survive a power loss. Must not be called inside a
// transaction (a kSync commit cannot nest).
absl::Status BeginMutation(Context &ctx, std::span<const InodeId> ids,
                           absl::FunctionRef<absl::Status()> body);

// The phase 1 of each mutation kind, as DirCacheFS runs it: each is one
// BeginMutation() naming exactly the inodes the mutation changes.
//
// Create/Mknod/Mkdir/Symlink of (parent, name): forgets `name`, marks
// `parent` incomplete and its attributes unknown (its mtime/ctime, and for
// a mkdir its nlink, are about to change). Dirty: parent. (The new child's row is created, and
// made dirty, by phase 3: backing::RecordNewChild.)
absl::Status BeginCreate(Context &ctx, InodeId parent, std::string_view name);
// Unlink/Rmdir of (parent, name) -> child: forgets `name`, marks `parent`
// incomplete, and both attribute sets unknown. Dirty: parent, child.
absl::Status BeginRemove(Context &ctx, InodeId parent, std::string_view name,
                         InodeId child);
// Rename (parent, name) -> src over (newparent, newname) -> dst (nullopt if
// absent, or if it is src itself): forgets both names, marks both parents
// incomplete, and the attributes of both parents, src and dst unknown.
// Dirty: parent, newparent, src, dst.
absl::Status BeginRename(Context &ctx, InodeId parent, std::string_view name,
                         InodeId newparent, std::string_view newname,
                         InodeId src, std::optional<InodeId> dst);
// Link of src as (newparent, newname): forgets `newname`, marks newparent
// incomplete, and the attributes of newparent and src unknown. Dirty:
// newparent, src.
absl::Status BeginLink(Context &ctx, InodeId src, InodeId newparent,
                       std::string_view newname);
// Setattr, a writable open (DirCacheFS::BeginWriting), fallback Write and
// Fallocate of `id`: marks its attributes unknown, and ForgetXattr()s each
// of `xattrs` (the ones the backing filesystem may change as a side effect:
// see DirCacheFS's side-effect xattrs). Dirty: id.
absl::Status BeginAttrChange(Context &ctx, InodeId id,
                             std::span<const std::string_view> xattrs = {});
// Setxattr/Removexattr of `name` on `id`: ForgetXattr(name) (only that
// name unknown) and marks the attributes unknown (the syscall bumps
// ctime). Dirty: id.
absl::Status BeginXattrChange(Context &ctx, InodeId id, std::string_view name);

// Adds `ids` to the dirty set at the default durability, inside the
// caller's transaction if any. For phase 3 of a mutation that creates a row
// (the new row is dirty too, and cannot exist in any state of the database
// where this insert does not). Does not add to ctx.dirty.durable.
absl::Status MarkDirty(Context &ctx, std::span<const InodeId> ids);

// The dirty set, sorted.
absl::StatusOr<std::vector<InodeId>> ListDirty(Context &ctx);

// Empties the dirty set except for `keep` (inodes with a writable open
// outstanding, which the kernel may still be changing: see
// backing::SyncBacking), in one transaction. Only after the backing
// filesystems have been synced.
absl::Status ClearDirty(Context &ctx, std::span<const InodeId> keep);

// Startup recovery after an unclean shutdown, in one transaction: for every
// inode in the dirty set, marks its attributes unknown, forgets its xattrs
// (rows deleted, set incomplete) and symlink target, forgets every dentry
// in it (positive and negative) and marks it incomplete if it is a
// directory, and forgets every dentry pointing at it, marking those
// dentries' parents incomplete (its name may have changed). Inode rows are
// kept, so NFS handles still resolve (and are verified when next opened).
// Then empties the dirty set. Returns how many dirty entries there were.
absl::StatusOr<int64_t> RecoverDirty(Context &ctx);

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
