#ifndef DCFS_BACKING_H_
#define DCFS_BACKING_H_

#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/types.h>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "absl/container/flat_hash_map.h"
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

// Out-of-band change detection (see the README's "Coherence"): if `cached`
// (id's cache row, as read before `fresh` was taken) is valid and disagrees
// with `fresh` -- a statx of the same backing object the caller already
// had in hand -- in mode, uid, gid, nlink, size, mtime or ctime, logs one
// WARNING naming the differing fields and adopts `fresh` (as RefreshAttrs
// would record it). A directory whose mtime or ctime changed additionally
// loses its negative dentries and is marked incomplete, so the next lookup
// or readdir relists it; any object whose ctime changed has its cached
// xattrs marked unknown. Issues no syscall of its own; a no-op when
// `cached` is not valid, or when a mutation of `id` may explain the
// difference (!cache::CanFill(ctx, snapshot, id), `snapshot` taken before
// `cached` was read).
absl::Status ReconcileAttrs(Context &ctx, cache::FillSnapshot snapshot,
                            InodeId id, const cache::CachedAttr &cached,
                            const struct statx &fresh);

// Reopens inode `id` with open(2) `flags`, after checking that the object
// reached is still the one the row describes (same inode number, and same
// generation when both are known). If it is not, or the kernel reports the
// handle stale, the row is invalidated and the result is an ESTALE status.
// The identity check's statx fetches every cached attribute (same single
// syscall), so a still-matching object is also passed through
// ReconcileAttrs.
absl::StatusOr<FileDescriptor> OpenNode(Context &ctx, InodeId id, int flags);

// Fresh attributes of `id` from the backing filesystem (STATX_BASIC_STATS
// and STATX_BTIME). Does not update the cache.
absl::StatusOr<struct statx> StatNode(Context &ctx, InodeId id);

// Refreshes `id`'s cached attributes from the backing filesystem (StatNode
// followed by cache::UpdateAttr). Called when CachedAttr.valid is false.
// If `id` is in ctx.open_for_write (a writable open is outstanding), the
// fresh values are stored but stay marked unknown, in the same transaction
// (true of RefreshAttrsFromFd, RecordNewLink and PopulateDirectory too).
//
// A fill (see cache::CanFill): if a mutation of `id` began, ended or is in
// flight while the statx was under way, nothing is recorded. Either way,
// if `fetched` is given it receives the statx, for the caller to answer
// from (so it never depends on the cache having recorded it).
absl::Status RefreshAttrs(Context &ctx, InodeId id,
                          struct statx *fetched = nullptr);

// As RefreshAttrs, but statx's an fd the caller already has open on `id`
// (a real, non-O_PATH fd is not required -- AT_EMPTY_PATH works on O_PATH
// too) instead of reopening it via OpenNode. Used by DirCacheFS::Release
// when the open being released was writable, to pick up the passthrough
// writes' effect on size/mtime/etc. before the fd closes.
absl::Status RefreshAttrsFromFd(Context &ctx, InodeId id, int fd,
                                struct statx *fetched = nullptr);

// Reads up to `size` bytes at `offset` from `fd` (a real, non-O_PATH fd
// already open on the node -- see DirCacheFS::Open's OpenNode call),
// looping over short reads until `size` bytes have been read or EOF. The
// fallback DirCacheFS::Read uses when the kernel did not grant
// FUSE_CAP_PASSTHROUGH for this open, so `fd` is not reopened here: unlike
// every other backing:: function this one takes an fd instead of an
// InodeId, because the caller already has one open and identity-verified.
absl::StatusOr<std::string> ReadFile(int fd, size_t size, off_t offset);

// As ReadFile, for the write side: writes all of `buf` to `fd` at `offset`,
// looping over short writes. The fallback DirCacheFS::Write uses; with
// FUSE_CAP_PASSTHROUGH granted the kernel writes directly against `fd` and
// this is never called. Returns the number of bytes written (always
// `buf.size()` on success; a short write only happens on an error, which is
// then returned instead).
absl::StatusOr<size_t> WriteFile(int fd, std::span<const char> buf,
                                 off_t offset);

// fallocate(2) on `fd` (as ReadFile/WriteFile, an already-open, identity
// verified real fd -- DirCacheFS::Fallocate's shared per-inode backing fd).
absl::Status FallocateFd(int fd, int mode, off_t offset, off_t length);

// fsync(2) (datasync false) or fdatasync(2) (true) on `fd`. Backing
// durability is the backing filesystem's own job; this simply passes the
// request through.
absl::Status FsyncFd(int fd, bool datasync);

// As FsyncFd, for a directory: opens `id` O_RDONLY|O_DIRECTORY via OpenNode
// (a plain read is enough; fsync needs no write access) and syncs it. dcfs
// tracks no separate directory state that would need flushing first.
absl::Status FsyncDir(Context &ctx, InodeId id, bool datasync);

// The target of symlink `id`, read from the backing filesystem.
absl::StatusOr<std::string> ReadSymlink(Context &ctx, InodeId id);

// All extended attributes of `id` as (name, value), read from the backing
// filesystem; empty if the filesystem does not support xattrs.
absl::StatusOr<std::vector<std::pair<std::string, std::string>>> ReadXattrs(
    Context &ctx, InodeId id);

// Refreshes `id`'s cached xattr set from the backing filesystem (ReadXattrs
// followed by cache::ReplaceXattrs, as a fill: see cache::CanFill). Called
// when the cached set is unknown (ListXattrs returned nullopt). Returns
// what it read, for the caller to answer from.
absl::StatusOr<std::vector<std::pair<std::string, std::string>>>
RefreshXattrs(Context &ctx, InodeId id);

// Reads xattr `name` of `id` from the backing filesystem and records it in
// the cache as present or absent (cache::SetXattr/RemoveXattr), resolving
// just that one name; returns what it read (nullopt: absent, including on
// a filesystem without xattrs). Reads through `open_fd` if given (any fd
// on `id`, O_PATH included), else through an O_PATH OpenNode. Used when
// the cached state of `name` is unknown, and by phase 3 of the mutations
// that change `name` as a side effect (DirCacheFS's side-effect xattrs).
absl::StatusOr<std::optional<std::string>> RefreshXattr(
    Context &ctx, InodeId id, std::string_view name,
    std::optional<int> open_fd = std::nullopt);

// What the backing filesystem stores under an xattr's name right after a
// successful setxattr, read back through the same object: its value,
// nullopt if it stored nothing, or the read-back's own error. The two can
// differ: e.g. ext4 (like xfs and btrfs) stores a system.posix_acl_access
// ACL that is exactly equivalent to the file mode as no xattr at all, and
// only updates the mode (posix_acl_update_mode).
using XattrReadBack = absl::StatusOr<std::optional<std::string>>;

// Applies setxattr(2)/removexattr(2) for `id` -- phase 2 of
// DirCacheFS::Setxattr/Removexattr's write-through rule, run between their
// own cache::ForgetXattr (phase 1) and cache::SetXattr/RemoveXattr (phase
// 3); does not touch the cache. If `open_fd` is given (an already-open,
// identity-verified fd on `id`, e.g. DirCacheFS's shared per-inode backing
// fd), it is used directly -- any access mode works, fsetxattr/fremovexattr
// need no particular open mode on the fd. Otherwise: a regular file or
// directory is reopened via /proc (syscalls::ReopenPathFd); a symlink or
// other special file -- which cannot be safely reopened for a real fd, and
// which fsetxattr/fremovexattr reject as an O_PATH fd regardless -- goes
// through setxattr_opath/removexattr_opath instead, following the magic
// link the same way ReadXattrs's XattrsOf already does for reads. The
// kernel itself rejects a "user." xattr on a symlink or special file with
// EPERM; that happens inside the real syscall here and needs no special
// casing.
//
// SetXattr's error is the setxattr's; on success it returns the read-back
// (see XattrReadBack), which phase 3 records instead of `value`.
absl::StatusOr<XattrReadBack> SetXattr(Context &ctx, InodeId id,
                                       std::string_view name,
                                       std::string_view value, int flags,
                                       std::optional<int> open_fd);
absl::Status RemoveXattr(Context &ctx, InodeId id, std::string_view name,
                         std::optional<int> open_fd);

// The statvfs of the filesystem `id` lives on, from that filesystem's mount
// fd -- no handle open of `id` itself is needed.
absl::StatusOr<struct statvfs> StatFilesystem(Context &ctx, InodeId id);

// Lists directory `dir` on the backing filesystem and caches all of it:
// every child's inode row (attributes, handle, generation, symlink target,
// xattrs), its dentry, and any filesystem mounted on a child; forgets
// cached names that no longer exist; and marks `dir` complete. This is the
// one point where a directory's disk is read, once, so later lookups and
// listings are answered from the cache.
//
// A fill (see cache::CanFill): the listing (dentries, completeness) is
// recorded only if no mutation of `dir` began, ended or is in flight during
// the I/O and nothing cleared `dir`'s completeness meanwhile; each child's
// attributes, symlink target and xattrs only if the same holds for that
// child (its row is upserted regardless, attributes unknown). The result
// says whether the listing was recorded, and what it found.
struct Populated {
  // Whether `dir`'s listing was recorded (and `dir` is now complete).
  bool cached = false;
  // Every name listed: kFound with the child's row, or kRefused.
  absl::flat_hash_map<std::string, cache::LookupResult> entries;
};
absl::StatusOr<Populated> PopulateDirectory(Context &ctx, InodeId dir);

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
//
// `open_for_write` (Create with a writable access mode): the new row's
// attributes are marked unknown again in that same transaction, since the
// caller is about to hand the kernel a writable passthrough fd on it.
//
// `mutation` is the create's (from cache::BeginCreate): the dentry is
// linked only if it Owns(parent); the new row's own state is a fill (see
// cache::CanFill).
absl::StatusOr<NewChild> RecordNewChild(Context &ctx,
                                        const cache::Mutation &mutation,
                                        InodeId parent, int parent_fd,
                                        std::string_view name,
                                        bool open_for_write = false);

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
// nlink land together -- each only if `mutation` (the link's, from
// cache::BeginLink) Owns the inode it writes. Returns the fresh attributes.
absl::StatusOr<struct statx> RecordNewLink(Context &ctx,
                                           const cache::Mutation &mutation,
                                           InodeId src, InodeId newparent,
                                           std::string_view newname);

// --- Remove/rename ops (step 4.3) -------------------------------------------
//
// Phase 2 of DirCacheFS::Unlink/Rmdir/Rename. None of these touches the
// cache (beyond OpenNode's own stale-row invalidation); the caller brackets
// each with its phase-1 MarkUnknown and phase-3 record transactions.

// unlinkat(2) of `name` inside `parent` (opened O_RDONLY|O_DIRECTORY via
// OpenNode). `flags` is 0 or AT_REMOVEDIR. Whatever unlinkat(2) returns is
// returned unchanged (ENOTEMPTY, EBUSY for a mount point, ...).
absl::Status UnlinkAt(Context &ctx, InodeId parent, std::string_view name,
                      int flags);

// renameat2(2) of `parent`/`name` to `newparent`/`newname`, with both
// parents opened O_RDONLY|O_DIRECTORY via OpenNode. `flags` is passed
// through unchanged (0, RENAME_NOREPLACE or RENAME_EXCHANGE -- the caller
// validates it). Whatever renameat2(2) returns is returned unchanged,
// including EXDEV when the two parents are on different filesystems.
absl::Status RenameAt(Context &ctx, InodeId parent, std::string_view name,
                      InodeId newparent, std::string_view newname,
                      unsigned flags);

// The backing link count of `id` (OpenNode O_PATH|O_NOFOLLOW + statx), or
// nullopt if the object no longer exists at all: its handle no longer
// decodes (ESTALE/ENOENT), i.e. its last link was removed and nothing holds
// it open. In that case the row has ALREADY been invalidated (by OpenNode
// for ESTALE, here for ENOENT), so the caller must not touch it again.
// Does not update the cache otherwise.
absl::StatusOr<std::optional<uint64_t>> BackingNlink(Context &ctx,
                                                     InodeId id);

// Run at startup, after InitRoot: forgets every non-source filesystem that
// is no longer mounted where it was found (or whose mount point is gone),
// and registers a mount fd for each one that still is.
absl::Status StartupPurge(Context &ctx);

// --- Durability: sync points and unclean-shutdown recovery (step 4.10) -----
//
// See the README's "Crash robustness" and schema.sql's `dirty` table.

// A sync point: syncfs(2) on every backing filesystem (every fd in
// ctx.mounts), then, once all of them succeeded, empties the dirty set in
// one transaction -- except for inodes in ctx.open_for_write, which the
// kernel may still be writing to through a passthrough fd, so a later
// crash could still leave the backing file ahead of the attributes the
// last Release records. On a syncfs failure nothing is cleared (the dirty
// entries only cost a larger re-read after a crash) and the error is
// returned.
absl::Status SyncBacking(Context &ctx);

// Startup, after Migrate() and before InitRoot()/StartupPurge(): if the
// last run did not shut down cleanly (cache_state.clean_shutdown is 0), or
// the dirty set is not empty for any other reason, runs
// cache::RecoverDirty and logs at WARNING how many entries it recovered
// and whether the machine rebooted meanwhile (cache_state.boot_id differs
// from `boot_id`, the current /proc/sys/kernel/random/boot_id). Then
// records clean_shutdown 0 and `boot_id`, durably (Durability::kSync),
// so a crash from here on is detected at the next start.
absl::Status StartRun(Context &ctx, std::string_view boot_id);

// Clean shutdown, once no more requests can arrive: SyncBacking, a WAL
// checkpoint, and, if the dirty set is then empty, records clean_shutdown
// 1 durably. Any failure is returned, and leaves clean_shutdown 0, so
// the next start recovers.
absl::Status FinishRun(Context &ctx);

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
