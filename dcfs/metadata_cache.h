#ifndef DCFS_METADATA_CACHE_H_
#define DCFS_METADATA_CACHE_H_

#include <sys/stat.h>

#include <climits>
#include <cstdint>
#include <ctime>
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
//  - Each dentry is present (-> an inode), absent, unknown, or refused
//    (the name exists but dcfs refuses to cache it: a mount point or
//    subvolume boundary below --source, see amendment 12 and README.md's
//    Limitations; it must never be reported as absent). A name with no row
//    is absent if its directory's listing is complete (children_complete)
//    and unknown otherwise. Only a full listing (PopulateDirectory) sets
//    children_complete; no single-name operation clears it: phase 1 of a
//    mutation marks just its own names unknown.
//  - "Invalidating" a row deletes it; every dentry pointing at it becomes
//    unknown (schema.sql's inodes_delete_unknowns trigger), never absent,
//    whatever deleted it.
//  - The number of cached dentries for an inode is not its link count (only
//    some of its links may be cached), so removing a dentry never deletes
//    the inode row by itself; see DeleteInode().
namespace dcfs::cache {

using InodeId = int64_t;
inline constexpr InodeId kRootInode = 1;

// Boundary stubs (step 23.5; schema.sql's `stubs`): a refused dentry is
// served as a stub directory whose nodeid is at or above 2^63 as the
// kernel sees it (an unsigned 64-bit fuse_ino_t), which is a negative
// InodeId. inodes.id (AUTOINCREMENT) is always positive, so the two never
// meet, and backing.cc refuses backing inode numbers in that range (from
// Phase 14 on, nodeids are backing inode numbers).
inline constexpr InodeId kFirstStubId = INT64_MIN;
inline constexpr uint64_t kFirstStubNodeid = uint64_t{1} << 63;
inline bool IsStub(InodeId id) { return id < 0; }

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

// `attr` with its attributes (st and btime) replaced by `stx`'s, converted
// exactly as a row written from `stx` (UpdateAttr) would read back
// (GetAttr); `valid` is left alone. For a fill's caller to answer from what
// it read even when the cache did not record it.
CachedAttr WithStatx(CachedAttr attr, const struct statx &stx);

struct LookupResult {
  enum class Kind {
    kFound,     // A positive dentry; `id` is its inode.
    kNegative,  // A cached negative dentry: the name is known to be absent.
    kRefused,   // The name exists but is a refused mount/subvolume boundary
                // (amendment 12): it must never be reported absent. It is
                // served as a stub directory; `id` is the stub's (IsStub).
    kUnknown,   // Nothing is known about this name: an unknown row, or no
                // row while the listing is incomplete.
  };
  Kind kind = Kind::kUnknown;
  // The inode for kFound, the stub for kRefused (0 if no stub was
  // recorded: a listing that could not record its result); 0 otherwise.
  InodeId id = 0;
};

// A boundary stub (schema.sql's `stubs`): the refused dentry (parent, name)
// it stands for, and its attributes, as GetAttr reads an inode's: valid,
// st_ino the stub's nodeid, fuse_gen its generation, backing_ino the same
// nodeid (a stub has no backing inode of its own; its inode number is its
// nodeid).
struct StubRow {
  InodeId id = 0;
  InodeId parent = 0;
  std::string name;
  CachedAttr attr;
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

// What is cached about (parent, name); see the identity model above. A
// name with no row reads kNegative in a complete listing, kUnknown
// otherwise.
absl::StatusOr<LookupResult> Lookup(Context &ctx, InodeId parent,
                                    std::string_view name);

// NotFound if there is no row for `id` (e.g. it was invalidated; the FUSE
// layer answers ESTALE).
absl::StatusOr<CachedAttr> GetAttr(Context &ctx, InodeId id);

// The stub `id` (IsStub). NotFound if it has no row (its dentry is no
// longer refused, or never was).
absl::StatusOr<StubRow> GetStub(Context &ctx, InodeId id);

// NotFound if there is no row for `id`. No production caller reads the
// generation on its own today (GetAttr's CachedAttr::fuse_gen covers every
// current need); kept as the single-field counterpart to GetAttr, exercised
// by its own test (FuseGenerations).
absl::StatusOr<uint32_t> GetGeneration(Context &ctx, InodeId id);

// Callback for ListDir. `next_cursor` is the cursor to pass to ListDir to
// resume after this entry. Return true to continue, false to stop.
using ListDirCallback = absl::FunctionRef<absl::StatusOr<bool>(
    std::string_view name, InodeId child, int64_t next_cursor)>;

// As ListDirCallback, with `attr`: the child's row read in the same query as
// its dentry (GetAttr's result), or null when the row's attributes are not
// valid or the child is a stub (the caller then reads them its own way).
// Valid only during the call.
using ListDirAttrsCallback = absl::FunctionRef<absl::StatusOr<bool>(
    std::string_view name, InodeId child, int64_t next_cursor,
    const CachedAttr *attr)>;

// Calls `cb` for each present dentry of `dir` after `cursor` (0 starts
// from the beginning), and each refused one (with its stub as `child`), in
// a stable order: cursors are dentry rowids, and updating an existing
// dentry in place (LinkDentry/RenameDentry onto an existing name) keeps its
// rowid. Absent and unknown entries are skipped: the caller must make sure
// the listing is complete first (IsDirComplete). `cb` may call other cache
// functions, including writes: no statement is held open across a
// callback.
absl::Status ListDir(Context &ctx, InodeId dir, int64_t cursor,
                     ListDirCallback cb);

// ListDir, handing each entry's valid attributes to the callback too: one
// join instead of a GetAttr per entry.
//
// `batch_rows` (at most 64, the default) is how many rows each query
// reads: a caller that can use only a few (a reply that holds 25 entries)
// asks for about that many instead of paying for 64; more are read, a
// batch at a time, if the callback keeps going.
absl::Status ListDir(Context &ctx, InodeId dir, int64_t cursor,
                     ListDirAttrsCallback cb, int64_t batch_rows = 64);

// Whether `dir`'s listing is complete: every name without a row is absent
// (children_complete; a missing directories row counts as incomplete).
absl::StatusOr<bool> ChildrenComplete(Context &ctx, InodeId dir);

// Whether every entry of `dir` is known, so that a listing (ListDir) can be
// served from the cache: its listing is complete (ChildrenComplete) and no
// row is unknown -- a listing must neither list nor silently omit a name
// whose state is unknown.
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

// The directory containing directory `dir`: the parent of the one present
// dentry pointing at it. The root is its own parent. nullopt (unknown) if
// no present dentry for `dir` is cached (see backing::ParentOf, which then
// asks the backing filesystem); NotFound if there is no row for `dir`.
absl::StatusOr<std::optional<InodeId>> ParentOf(Context &ctx, InodeId dir);

// All filesystems, in the order they were added.
absl::StatusOr<std::vector<FilesystemRow>> ListFilesystems(Context &ctx);

// NotFound if `device` is not registered.
absl::StatusOr<FilesystemRow> GetFilesystem(Context &ctx,
                                            const DeviceId &device);

// --- Writes (each is one transaction) --------------------------------------

// Records the backing object identified by (handle.device, stx.stx_ino,
// backing_gen), with attributes from `stx` (now valid) and handle `handle`.
// If that identity already has a row describing the same object -- its
// generation equals backing_gen when both are known (nonzero: 0 means the
// generation could not be read), its stored handle (handle_type and bytes)
// equals `handle`, and its birth time equals stx's when both are known
// (nonzero) -- that row is updated.
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
// see LookupResult::Kind::kRefused. Replaces whatever was cached for that name,
// same as LinkDentry/SetNegative, and records the stub it is served as, in
// the same transaction, with `root` (a statx of the boundary's root
// directory) as its attributes: the stub it already had, if the name was
// refused already or forgotten since (same nodeid and generation,
// attributes refreshed), else a new one (the next nodeid up from the
// highest ever handed out, cache_state.last_stub_id, from kFirstStubId:
// never one a stub that went had; and a random nonzero generation from
// ctx.rng, as for inode rows).
// Returns the stub. NotFound if `parent` is missing.
absl::StatusOr<InodeId> SetRefused(Context &ctx, InodeId parent,
                                   std::string_view name,
                                   const struct statx &root);

// Marks (parent, name) unknown. Never deletes the inode row it pointed at,
// since other (possibly uncached) links may remain. NotFound if `parent`
// is missing.
//
// No production caller: DirCacheFS's own Unlink/Rmdir goes straight to
// SetNegative (it already knows the name is gone, not merely unknown).
// Kept as the general-purpose single-name primitive MarkUnknown's
// multi-name loop is built from the same way, exercised by its own test
// (UnlinkDentryLeavesInodeRow).
absl::Status UnlinkDentry(Context &ctx, InodeId parent, std::string_view name);

// Moves the positive entry (parent, name) to (newparent, newname),
// replacing any entry cached there. The source name becomes unknown (not
// absent). NotFound if no positive entry is cached for the source.
//
// No production caller: DirCacheFS's own Rename composes LinkDentry and
// SetNegative/LinkDentry directly, since it already knows the exact
// post-rename state of both names (see RefreshAfterRename) rather than
// just "moved, with the old name now unknown". Kept as the cache-level
// rename primitive, exercised by its own test (RenameAcrossParents).
absl::Status RenameDentry(Context &ctx, InodeId parent, std::string_view name,
                          InodeId newparent, std::string_view newname);

// Records whether every entry of `dir` is cached.
absl::Status MarkDirComplete(Context &ctx, InodeId dir, bool complete);

// `dir`'s completeness epoch (directories.epoch): bumped by every write
// that clears its children_complete. 0 if it has no directories row.
absl::StatusOr<int64_t> DirEpoch(Context &ctx, InodeId dir);


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

// Marks `names` in `parent` unknown, leaving every other name (and the
// listing's completeness) alone. Phase 1 of a two-phase mutation: done
// before the backing operation, so that a crash between the two leaves
// "unknown" rather than stale state. NotFound if `parent` is missing.
absl::Status MarkUnknown(Context &ctx, InodeId parent,
                         std::span<const std::string> names);

// Forgets every cached negative or refused dentry of `dir` (see
// LookupResult::Kind::kRefused: a refused one becomes unknown and keeps its
// stub, so that a boundary found again keeps its nodeid) and marks `dir`
// incomplete, leaving its positive dentries alone. For a directory found to
// have changed on the backing filesystem behind dcfs's back (see
// backing::ReconcileAttrs): a new name may have appeared that a negative entry
// would otherwise keep hiding, a refused boundary may no longer be one (or a
// new one may have appeared), but the names already cached positive still point
// at their objects (a repopulation corrects any that do not), and keeping them
// keeps a subdirectory's ".." resolvable meanwhile. Dropping a refused dentry
// here never causes a false "absent" reading (see the identity-model note
// above): `dir` is marked incomplete in the same transaction, so the next
// lookup or readdir repopulates it rather than ever answering from the gap.
// NotFound if no row for `dir`.
absl::Status ForgetNegativeDentries(Context &ctx, InodeId dir);

// Marks `id`'s cached attributes as not current. NotFound if no row.
absl::Status MarkAttrsUnknown(Context &ctx, InodeId id);

// Replaces `id`'s cached attributes with `stx` and marks them current.
// NotFound if no row.
absl::Status UpdateAttr(Context &ctx, InodeId id, const struct statx &stx);

// Step 23.3: `id` is being opened for reading at `now`. Reads go through
// passthrough, so the backing filesystem updates the file's access time
// without dcfs seeing it; this records in the cache, in one transaction
// with no syscall, the access time the backing filesystem gives the file
// for a read now, by ctx.atime (the kernel's rule for relatime: if the
// cached atime is not after mtime or ctime, or is a day old or more).
// Only current attributes are touched (unknown ones are re-read anyway,
// atime included); a fill's guard is not needed, since nothing is read
// from the backing filesystem. It reads first and takes a write
// transaction only when the atime changes (a compare-and-set on the atime
// it read). Returns whether it changed the atime. NotFound if no row.
absl::StatusOr<bool> TouchAtime(Context &ctx, InodeId id,
                                const struct timespec &now);

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

// Invalidates `id` (see the identity model above): deletes the row; every
// dentry pointing at it becomes unknown. Cascades remove its directories/symlinks/xattrs rows, its child dentries,
// and any filesystem mounted inside it. `id` must not be the root.
// NotFound if no row.
absl::Status InvalidateInode(Context &ctx, InodeId id);

// Removes `id` once its backing object is gone (nlink reached 0). The cache
// effect is identical to InvalidateInode(); this name exists so that the
// FUSE layer's call sites say which situation they are handling.
absl::Status DeleteInode(Context &ctx, InodeId id);

// --- Fills vs. concurrent mutations (audit-tristate F1) --------------------
//
// A fill reads the backing filesystem and then records what it read as
// present (backing::RefreshAttrs, RefreshXattrs, PopulateDirectory, ...).
// Under the planned coroutine model other requests run while it waits on
// its I/O, so by the time it commits, a mutation may have changed (or be
// changing) the very records it read. The rule: a fill takes a
// FillSnapshot before its first backing syscall, and may record something
// about inode `id` only if CanFill(ctx, snapshot, id) at commit time, in
// the commit's transaction -- i.e. no mutation of `id` began or ended since
// the snapshot, and none is in flight. Otherwise it caches nothing about
// `id` (the record stays as the mutation left it: unknown while in flight,
// its fresh result once done) and answers its own caller from what it read.
//
// A mutation is a cache::Mutation, from phase 1 (BeginMutation) to End().
// Its phase-3 writes that are not fills themselves (its own dentry
// changes, a read-back value) are made only if Owns(id): no other mutation
// of `id` began since its phase 1, and none other is in flight. Phase-3
// refreshes (RefreshAttrs etc.) run after End(), as ordinary fills.
//
// "Mutation of `id`": `id` is among the ids its phase 1 names (see the
// Begin* functions below), i.e. its attributes, dentries (as a parent),
// symlink target or xattrs change. The granularity is the inode, not the
// record: e.g. a Setxattr in flight also blocks a concurrent attribute fill
// of the same inode, which only costs that fill its caching.

struct FillSnapshot {
  uint64_t seq = 0;
};

// Snapshot for a fill, taken before its first backing syscall.
FillSnapshot BeginFill(const Context &ctx);

// Whether a fill that took `snapshot` may record something about `id` now.
bool CanFill(const Context &ctx, FillSnapshot snapshot, InodeId id);

// A mutation between its phase 1 and its end (see above). Move-only; the
// destructor calls End() if it has not been called.
class Mutation {
 public:
  Mutation(Mutation &&other) noexcept;
  Mutation &operator=(Mutation &&other) = delete;
  Mutation(const Mutation &) = delete;
  ~Mutation();

  // Whether this mutation's phase 3 may write `id`'s records itself (see
  // above). False after End(), and for an id it does not name.
  bool Owns(InodeId id) const;

  // The end of the mutation (after its phase-3 writes, before its phase-3
  // refreshes): it is no longer in flight. Idempotent.
  void End();

 private:
  friend absl::StatusOr<Mutation> BeginMutation(
      Context &ctx, std::span<const InodeId> ids,
      absl::FunctionRef<absl::Status()> body);
  explicit Mutation(Context *ctx) : ctx_(ctx) {}

  Context *ctx_;
  // Each (distinct) id with FillGuards::seq at this mutation's phase 1.
  std::vector<std::pair<InodeId, uint64_t>> ids_;
};

// The end of the writes the kernel made through a writable open of `id`
// (DirCacheFS::Release of the last one), which began with that open's
// phase 1 (DirCacheFS::BeginWriting, a BeginAttrChange that ends at once:
// while the open lasts, ctx.open_for_write rather than an in-flight
// mutation keeps the attributes unknown). A guard event like a mutation's
// End(): it advances the clock and touches `id`, so that a snapshot taken
// while the open was outstanding -- a fill's, which may have read the
// attributes before the last writes, or a sync point's (BeginSync), whose
// syncfs may have begun before them -- can no longer record or clear
// anything about `id`. Must come before the release records anything, with
// no suspension point between it and the removal of `id` from
// ctx.open_for_write.
void EndWrites(Context &ctx, InodeId id);

// Guarded fills: each writes (in one transaction) only if
// CanFill(ctx, snapshot, id), and returns whether it did.
// UpdateAttr:
absl::StatusOr<bool> FillAttr(Context &ctx, FillSnapshot snapshot, InodeId id,
                              const struct statx &stx);
// ReplaceXattrs:
absl::StatusOr<bool> FillXattrs(
    Context &ctx, FillSnapshot snapshot, InodeId id,
    std::span<const std::pair<std::string, std::string>> xattrs);
// SetXattr (a value) or RemoveXattr (nullopt):
absl::StatusOr<bool> FillXattr(Context &ctx, FillSnapshot snapshot, InodeId id,
                               std::string_view name,
                               std::optional<std::string_view> value);
// SetSymlink:
absl::StatusOr<bool> FillSymlink(Context &ctx, FillSnapshot snapshot,
                                 InodeId id, std::string_view target);

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
//
// On success the mutation is in flight on `ids` (see Mutation) until the
// returned Mutation ends.
absl::StatusOr<Mutation> BeginMutation(Context &ctx,
                                       std::span<const InodeId> ids,
                                       absl::FunctionRef<absl::Status()> body);

// The phase 1 of each mutation kind, as DirCacheFS runs it: each is one
// BeginMutation() naming exactly the inodes the mutation changes.
//
// Each marks only the names it changes unknown (MarkUnknown); every other
// name keeps its state, and so does the directory's completeness.
//
// Create/Mknod/Mkdir/Symlink of (parent, name): marks `name` unknown and
// `parent`'s attributes unknown (its mtime/ctime, and for a mkdir its
// nlink, are about to change). Dirty: parent. (The new child's row is
// created, and made dirty, by phase 3: backing::RecordNewChild.)
absl::StatusOr<Mutation> BeginCreate(Context &ctx, InodeId parent,
                                     std::string_view name);
// Unlink/Rmdir of (parent, name) -> child: marks `name` unknown, and both
// attribute sets unknown. Dirty: parent, child.
//
// `resolved` is a snapshot (BeginFill) the caller took before resolving
// `child`. The unlinkat removes whatever `name` holds when it runs, and
// phase 1 marks `child` unknown (and phase 3 settles its row), which is
// right only if `name` still holds `child`. So, as BeginRename does, this
// first verifies in phase 1's transaction that no mutation of parent or
// child began or ended since `resolved`, or is in flight (CanFill for
// each); if one did, it writes nothing, begins no mutation, and fails with
// kAborted: resolve again.
absl::StatusOr<Mutation> BeginRemove(Context &ctx, InodeId parent,
                                     std::string_view name, InodeId child,
                                     FillSnapshot resolved);
// Rename (parent, name) -> src over (newparent, newname) -> dst (nullopt if
// absent, or if it is src itself): marks both names unknown, and the
// attributes of both parents, src and dst unknown. Dirty: parent,
// newparent, src, dst.
//
// `resolved` is a snapshot (BeginFill) the caller took before resolving
// src and dst. Phase 3 links the names to src and dst, which is right only
// if they still are what the names hold, and Mutation::Owns sees overlaps
// only from phase 1 on. So this first verifies, in phase 1's transaction,
// that no mutation of parent, newparent, src or dst began or ended since
// `resolved`, or is in flight (CanFill for each); if one did, it writes
// nothing, begins no mutation, and fails with kAborted: resolve again.
absl::StatusOr<Mutation> BeginRename(Context &ctx, InodeId parent,
                                     std::string_view name, InodeId newparent,
                                     std::string_view newname, InodeId src,
                                     std::optional<InodeId> dst,
                                     FillSnapshot resolved);
// Link of src as (newparent, newname): marks `newname` unknown, and the
// attributes of newparent and src unknown. Dirty: newparent, src.
absl::StatusOr<Mutation> BeginLink(Context &ctx, InodeId src,
                                   InodeId newparent,
                                   std::string_view newname);
// Setattr, a writable open (DirCacheFS::BeginWriting), fallback Write and
// Fallocate of `id`: marks its attributes unknown, and ForgetXattr()s each
// of `xattrs` (the ones the backing filesystem may change as a side effect:
// see DirCacheFS's side-effect xattrs). Dirty: id.
absl::StatusOr<Mutation> BeginAttrChange(
    Context &ctx, InodeId id, std::span<const std::string_view> xattrs = {});
// The FORGET reconciliation of written files (DirCacheFS::ReconcileWritten)
// of several inodes at once: marks the attributes of each of `ids` unknown,
// in one phase 1, skipping any whose row is gone (no failure for it).
// Dirty: ids.
absl::StatusOr<Mutation> BeginAttrChanges(Context &ctx,
                                          std::span<const InodeId> ids);
// Setxattr/Removexattr of `name` on `id`: ForgetXattr(name) (only that
// name unknown) and marks the attributes unknown (the syscall bumps
// ctime). Dirty: id.
absl::StatusOr<Mutation> BeginXattrChange(Context &ctx, InodeId id,
                                          std::string_view name);

// Adds `ids` to the dirty set at the default durability, inside the
// caller's transaction if any. For phase 3 of a mutation that creates a row
// (the new row is dirty too, and cannot exist in any state of the database
// where this insert does not). Does not add to ctx.dirty.durable.
absl::Status MarkDirty(Context &ctx, std::span<const InodeId> ids);

// The dirty set, sorted.
absl::StatusOr<std::vector<InodeId>> ListDirty(Context &ctx);

// A sync point (backing::SyncBacking) in two halves, around its syncfs(2)
// calls. The rule it keeps (formal/ finding sync_during_mutation): a dirty
// row may be removed only by a sync point whose syncfs began after every
// backing syscall the row stands for had returned. A mutation's syscall
// comes after its phase 1 and before its End(), so a mutation of `id` that
// was in flight at any moment between the start of the syncfs and the
// clear may have issued its syscall too late for the syncfs to cover it;
// its row must stay (until the next sync point). Under today's single
// thread no mutation runs during a sync point; under coroutines one can
// run while the sync point waits on syncfs, or the sync point can run
// while a mutation waits on its syscall.
//
// The same holds for the writes the kernel makes through a writable open,
// which dcfs never sees: they lie between the open's phase 1 and the last
// writable release (EndWrites), so an inode open for writing at any moment
// between the start of the syncfs and the clear keeps its row too.
//
// BeginSync, just before the first syncfs: the fill guards' clock, the
// dirty set as it is now, and the inodes open for writing now
// (ctx.open_for_write).
struct SyncSnapshot {
  FillSnapshot fills;
  std::vector<InodeId> dirty;  // Sorted.
  std::vector<InodeId> open_for_write;
};
absl::StatusOr<SyncSnapshot> BeginSync(Context &ctx);

// ClearDirty, once every syncfs succeeded: removes, in one transaction, each
// row of `synced.dirty` unless its inode
//  - is in `keep` (the inodes open for writing now, which the kernel may
//    still be changing: see backing::SyncBacking),
//  - or was open for writing at BeginSync (synced.open_for_write: its last
//    writes may have come after the syncfs began; with EndWrites this is a
//    backstop, since a release after BeginSync also fails the next test),
//  - or a mutation of it began or ended since BeginSync or is in flight, or
//    its writable open ended since (!CanFill(ctx, synced.fills, id)).
// Rows added after BeginSync (by a phase 1, or by MarkDirty in a phase 3)
// are never in `synced.dirty`, so they stay too.
// If `cleared` is not null, it receives the number of rows removed (for the
// sync point's log line).
absl::Status ClearDirty(Context &ctx, const SyncSnapshot &synced,
                        std::span<const InodeId> keep,
                        int64_t *cleared = nullptr);

// Startup recovery after an unclean shutdown, in one transaction: for every
// inode in the dirty set, marks its attributes unknown, forgets its xattrs
// (rows deleted, set incomplete) and symlink target, forgets every dentry
// in it and marks its listing incomplete if it is a directory (a lost
// backing change may have added names nothing cached), and marks every
// dentry pointing at it unknown (its name may have changed). Inode rows are
// kept, so NFS handles still resolve (and are verified when next opened).
// The dirty set is kept until a sync point's syncfs and ClearDirty (step
// 12.6b): the crashed run's backing changes may not be durable yet, and the
// start probes the same rows (backing::Startup), so a crash during
// recovery leaves them all to the next start. Returns how many dirty
// entries there are.
absl::StatusOr<int64_t> RecoverDirty(Context &ctx);

// Every start (backing::StartRun), after RecoverDirty: deletes every
// non-directory row whose recorded link count is 0 and that no present
// dentry names -- an unnamed O_TMPFILE file, or an unlinked file dcfs still
// had open, whose last release never came (a crash, or a DESTROY with files
// still open: SIGTERM, a lazy unmount). Reads only the rows the partial
// index inodes_unlinked holds. The
// row-lifetime rule deletes such a row at that release; after a crash the
// object is gone (or unreachable through dcfs), and a row deleted wrongly
// only costs a re-probe -- which gives the object a new nodeid, so an NFS
// handle to the old one gets ESTALE. Directories are left (a removed one's
// row goes at once). Returns how many rows went. One transaction.
absl::StatusOr<int64_t> ForgetUnnamedRows(Context &ctx);

// Registers a filesystem. AlreadyExists if `device` is already registered;
// NotFound if `parent` is given but has no row.
absl::Status AddFilesystem(Context &ctx, const DeviceId &device,
                           int64_t fstype, std::optional<InodeId> parent,
                           std::optional<std::string> boundary_name);

// Forgets everything cached about filesystem `device`: its row, and by
// cascade its inodes (with their dentries, xattrs, symlinks, directories)
// and any filesystems mounted beneath it. Its boundary dentry becomes
// unknown (not absent), so the mount point is rediscovered on the next
// lookup. Must not be the source
// device. NotFound if `device` is not registered.
absl::Status PurgeFilesystem(Context &ctx, const DeviceId &device);

}  // namespace dcfs::cache

#endif  // DCFS_METADATA_CACHE_H_
