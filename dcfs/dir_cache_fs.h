#ifndef DCFS_DIR_CACHE_FS_H_
#define DCFS_DIR_CACHE_FS_H_

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <sys/types.h>

#include "absl/container/flat_hash_map.h"
#include "absl/container/flat_hash_set.h"
#include "absl/functional/function_ref.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/time/time.h"
#include "dcfs/backing.h"
#include "dcfs/context.h"
#include "dcfs/fd.h"
#include "dcfs/fuse_request.h"
#include "dcfs/metadata_cache.h"
#include "fuse_lowlevel.h"

namespace dcfs {

using cache::InodeId;

// DirCacheFS is the low-level FUSE filesystem: every op below reads and
// writes through cache::/backing:: against `ctx_`, never touching the
// backing filesystem or ctx_.mounts directly itself -- that is backing.cc's
// job. Every op has its own DirCacheFS method and an entry in the ops table
// (see fuse_ops.h); none reply ENOSYS any more as of step 4.4.
class DirCacheFS {
 public:
  // dcfs has exclusive access to the backing tree (nothing else is supposed
  // to modify it out from under the cache), so these default to long: the
  // cache is only invalidated by our own mutations (from Phase 4 on), not by
  // a timeout racing a change dcfs doesn't know about.
  struct Options {
    absl::Duration attr_timeout = absl::Hours(1);
    absl::Duration entry_timeout = absl::Hours(1);

    // The value of a "-o max_read=N" mount option, if the caller passed
    // one via --fuse_opt. libfuse's fuse_apply_conn_info_opts() does not
    // cover max_read (unlike max_write/max_readahead/etc.), and
    // fuse_session_new() never populates conn.max_read itself -- it stays
    // zero-initialized until Init() sets it. do_init() then requires it to
    // equal the max_read mount option it parsed independently, so Init()
    // must set conn.max_read to this same value or the mount fails with
    // "init() and fuse_session_new() requested different maximum read
    // size".
    std::optional<unsigned int> max_read;

    // How often, at most, a request finding the dirty set non-empty first
    // runs a sync point (backing::SyncBacking: syncfs of the backing
    // filesystems, then the dirty set is emptied). See MaybeSyncBacking().
    absl::Duration sync_interval = absl::Seconds(5);
  };

  // `ctx` must outlive this DirCacheFS, which points ctx.open_for_write at
  // its own set of inodes with a writable open outstanding.
  DirCacheFS(Context &ctx, Options opts);
  ~DirCacheFS();

  absl::Status Init(struct fuse_conn_info &conn);
  absl::Status Destroy();

  absl::Status Getattr(FuseRequest &req, fuse_ino_t ino, fuse_file_info *fi);
  absl::Status Setattr(
      FuseRequest &req, fuse_ino_t ino, struct stat *attr, int to_set,
      fuse_file_info *fi);

  absl::Status Lookup(
      FuseRequest &req, fuse_ino_t parent_ino, std::string_view name);
  // Inode rows persist across restarts (that is what keeps NFS handles
  // valid), so there is nothing to do when the kernel drops its reference.
  void Forget(FuseRequest &req, fuse_ino_t ino, uint64_t nlookup);
  void ForgetMulti(
      FuseRequest &req, std::span<const fuse_forget_data> forgets);

  absl::Status Readlink(FuseRequest &req, fuse_ino_t ino);
  absl::Status Mknod(
      FuseRequest &req, fuse_ino_t parent, std::string_view name,
      mode_t mode, dev_t rdev);
  absl::Status Mkdir(
      FuseRequest &req, fuse_ino_t parent, std::string_view name,
      mode_t mode);
  absl::Status Unlink(
      FuseRequest &req, fuse_ino_t parent, std::string_view name);
  absl::Status Rmdir(
      FuseRequest &req, fuse_ino_t parent, std::string_view name);
  absl::Status Symlink(
      FuseRequest &req, std::string_view link, fuse_ino_t parent,
      std::string_view name);
  absl::Status Rename(
      FuseRequest &req, fuse_ino_t parent, std::string_view name,
      fuse_ino_t newparent, std::string_view newname, unsigned int flags);
  absl::Status Link(
      FuseRequest &req, fuse_ino_t ino, fuse_ino_t newparent,
      std::string_view newname);

  absl::Status Open(FuseRequest &req, fuse_ino_t ino, fuse_file_info &fi);
  absl::Status Read(
      FuseRequest &req, fuse_ino_t ino, size_t size, off_t off,
      fuse_file_info &fi);
  absl::Status Write(
      FuseRequest &req, fuse_ino_t ino, std::span<const char> buf, off_t off,
      fuse_file_info &fi);
  absl::Status Flush(FuseRequest &req, fuse_ino_t ino, fuse_file_info &fi);
  absl::Status Release(FuseRequest &req, fuse_ino_t ino, fuse_file_info &fi);
  absl::Status Fsync(
      FuseRequest &req, fuse_ino_t ino, int datasync, fuse_file_info &fi);

  absl::Status Opendir(FuseRequest &req, fuse_ino_t ino, fuse_file_info &fi);
  absl::Status Readdir(
      FuseRequest &req, fuse_ino_t ino, size_t size, off_t off,
      fuse_file_info &fi);
  absl::Status Readdirplus(
      FuseRequest &req, fuse_ino_t ino, size_t size, off_t off,
      fuse_file_info &fi);
  absl::Status Releasedir(FuseRequest &req, fuse_ino_t ino, fuse_file_info &fi);
  absl::Status Fsyncdir(
      FuseRequest &req, fuse_ino_t ino, int datasync, fuse_file_info &fi);

  absl::Status Statfs(FuseRequest &req, fuse_ino_t ino);

  absl::Status Setxattr(
      FuseRequest &req, fuse_ino_t ino, std::string_view name,
      std::string_view value, int flags);
  absl::Status Getxattr(
      FuseRequest &req, fuse_ino_t ino, std::string_view name, size_t size);
  absl::Status Listxattr(FuseRequest &req, fuse_ino_t ino, size_t size);
  absl::Status Removexattr(
      FuseRequest &req, fuse_ino_t ino, std::string_view name);

  // The mount is started with -o default_permissions, so the kernel checks
  // permissions itself against the cached attributes Getattr/Lookup report;
  // by the time Access() is called the kernel has already decided to allow
  // the operation, so there is nothing left for the filesystem to check.
  absl::Status Access(FuseRequest &req, fuse_ino_t ino, int mask);

  absl::Status Create(
      FuseRequest &req, fuse_ino_t parent, std::string_view name,
      mode_t mode, fuse_file_info &fi);

  absl::Status Fallocate(
      FuseRequest &req, fuse_ino_t ino, int mode, off_t offset, off_t length,
      fuse_file_info &fi);

  // Called at the start of every request (see fuse_ops.cc): runs a sync
  // point if the dirty set may be non-empty and opts_.sync_interval has
  // passed since the last one. dcfs is single-threaded inside libfuse's
  // blocking session loop, which offers no idle hook, so there is no timer:
  // an idle daemon keeps a non-empty dirty set until its next request, its
  // next fsync, or a clean shutdown. That is safe; it only makes the
  // re-read after a crash larger.
  void MaybeSyncBacking();

  // Whether any Open()/Create() handle for `id` is still outstanding (has
  // not gone through Release()). A file whose last link is removed keeps
  // its row while this is true (see SettleUnlinkedFile).
  bool HasOpenFiles(InodeId id) const;

 private:
  // cache::GetAttr(ctx_, id), except that NotFound from a non-root id is
  // reported as ESTALE, not NotFound: that is what a nodeid the kernel is
  // still holding, but this cache no longer has a row for (e.g. its
  // backing inode number was recycled and invalidated the row, or the
  // kernel's automatic LOOKUP_REVAL retry of an open that already failed
  // ESTALE once), means to the kernel -- not ENOENT. Every op that starts
  // from an inode id should go through this rather than cache::GetAttr()
  // directly.
  absl::StatusOr<cache::CachedAttr> RequireAttr(InodeId id);

  // The fuse_entry_param for `id`: current cached attributes (refreshed
  // first if not valid), nodeid = id, generation = the row's fuse_gen, and
  // timeouts from opts_. See RequireAttr() for the NotFound -> ESTALE
  // conversion.
  absl::StatusOr<fuse_entry_param> EntryFor(InodeId id);

  // Refreshes `id`'s cached attributes when they are not valid: from the
  // shared backing fd if `id` is open (a statx on an fd already open -- no
  // reopen by handle, no disk access), else via backing::RefreshAttrs. An
  // inode with a writable open outstanding always takes the first path,
  // since its attributes stay unknown for as long as that open lasts.
  absl::Status RefreshAttrsOf(InodeId id);

  // Phase 3 for writes made through the shared backing fd `fd` of `id`
  // (Flush/Fsync/Release): backing::RefreshAttrsFromFd. A failure is logged
  // at WARNING and the attributes are left (re-marked) unknown, never
  // returned: none of those ops may skip its bookkeeping, or fail the
  // caller's close(2)/fsync(2), over a cache refresh. `op` names the caller
  // in the log line.
  void RecordWrittenAttrs(InodeId id, int fd, std::string_view op);

  // Phase 3 for the side-effect xattrs `names` of a mutation of `id` (see
  // XattrsChangedBySetattr in dir_cache_fs.cc): backing::RefreshXattr of
  // each, through `fd` if given. A failure is logged at WARNING and leaves
  // that name unknown (phase 1's state), never failing the op, whose
  // backing change already happened. `op` names the caller in the log line.
  void ResolveSideEffectXattrs(InodeId id,
                               std::span<const std::string_view> names,
                               std::optional<int> fd, std::string_view op);

  // Registers a writable open of `id` (phase 1 of the write-through rule
  // for writes the kernel makes through the passthrough fd, which dcfs
  // never sees): adds it to open_for_write_ and marks its cached attributes
  // and the xattrs a write changes as a side effect (kXattrsChangedByWrite)
  // unknown, before the open is replied to. Phase 3 is Release() of the
  // last writable open.
  absl::Status BeginWriting(InodeId id);

  // A sync point now (if the dirty set may be non-empty), logging a failure
  // at WARNING: nothing that calls this may fail over it. `why` names the
  // caller in the log line.
  void SyncBackingNow(std::string_view why);

  // Shared phase-1/2/3 wiring for Create/Mkdir/Mknod/Symlink (see
  // dir_cache_fs.cc's "write-through op" rule for what those phases are):
  // checks the parent row exists (NotFound -> ESTALE, the same pattern
  // EntryFor uses), runs cache::MarkUnknown(parent, {name}) (phase 1), opens
  // the parent directory once and hands its fd to `do_create` -- the
  // op-specific backing syscall (mkdirat/mknodat/symlinkat/openat), phase 2
  // -- and, if that succeeds, probes and records the newly created child in
  // one transaction via backing::RecordNewChild (phase 3). On any failure
  // the (parent, name) state is left unknown, to repopulate on the next
  // lookup, and the failing status (with its errno payload intact) is
  // returned unchanged. Link does not go through this: it links an
  // *existing* inode, so there is no new child to probe.
  // `open_for_write` is passed through to RecordNewChild (Create() with a
  // writable access mode).
  absl::StatusOr<backing::NewChild> CreateChild(
      InodeId parent, std::string_view name,
      absl::FunctionRef<absl::Status(int parent_fd)> do_create,
      bool open_for_write = false);

  // Unlink (is_dir false) / Rmdir (true) of (parent, name): the shared
  // phase-1/2/3 wiring, including the removed child's row lifetime.
  absl::Status RemoveChild(
      FuseRequest &req, InodeId parent, std::string_view name, bool is_dir);

  // After a failed phase 2 (the backing syscall) of Unlink/Rmdir/Rename:
  // re-resolves `names` in `parent` (LookupOrPopulate) so that the state
  // phase 1 made unknown is known again. Errors are ignored.
  void ReresolveAfterFailure(
      InodeId parent, std::span<const std::string> names);

  // cache::DeleteInode(id), treating an already-missing row as success.
  // For an object known to be gone from the backing filesystem.
  absl::Status ForgetRemoved(InodeId id);

  // After a backing unlink or rename-over removed one link to non-directory
  // `id`: applies the row-lifetime rule. With a dcfs open on it, refreshes
  // its attributes from that fd and keeps the row (Release deletes it once
  // the last open closes with nlink 0); otherwise deletes the row if the
  // backing object is gone or has nlink 0, and refreshes it if links remain.
  absl::Status SettleUnlinkedFile(InodeId id);

  // The fd of some outstanding open of `id`, if any.
  std::optional<int> OpenFdOf(InodeId id) const;

  // One shared backing descriptor per inode with at least one dcfs open on
  // it. The kernel refuses a second, different backing file for one inode
  // (fs/fuse/iomode.c fuse_inode_uncached_io_start: EBUSY if the fuse_backing
  // registered for a FUSE OPEN's fi.backing_id differs from the one already
  // associated with the inode), and fuse_passthrough_open() -- the ioctl
  // that registers a backing fd and hands back a fresh backing_id --
  // allocates a brand new kernel-side fuse_backing every time it is called,
  // even for the very same fd number (fs/fuse/backing.c fuse_backing_open).
  // So passthrough only works for two concurrent opens of one inode if both
  // report the *same* backing_id, which means fuse_passthrough_open() may be
  // called at most once per inode: this struct is that one registration,
  // shared by every OpenFile of the inode regardless of each open's own
  // access mode.
  struct BackingFile {
    FileDescriptor fd;
    // The passthrough backing id fuse_passthrough_open() returned when this
    // BackingFile was created (0 if the kernel did not grant
    // FUSE_CAP_PASSTHROUGH, or the open otherwise failed to register one --
    // every open of this inode then falls back to Read()/Write() serving
    // the data through `fd` themselves).
    int backing_id = 0;
    // Whether `fd` was opened O_RDWR (true) or MakeBackingFile had to fall
    // back to O_RDONLY (false: an immutable/append-only file, or a
    // read-only backing filesystem -- see MakeBackingFile). A write through
    // a read-only `fd` (fallback Write(), or Fallocate()) then fails with
    // whatever the real syscall reports (typically EBADF), exactly as it
    // would against the backing filesystem directly.
    bool writable = false;
    // Number of writable OpenFile handles currently referencing this
    // BackingFile (a subset of `refs`). While it is nonzero the inode is in
    // open_for_write_ and its cached attributes stay unknown (see
    // BeginWriting); the Release() that takes it to 0 records them for real.
    int writable_refs = 0;
    // Number of OpenFile handles currently referencing this inode's shared
    // fd. Reaches 0 exactly when the Release() of the last of them runs,
    // which is when this BackingFile itself is torn down.
    int refs = 0;
  };

  // One fi.fh handle: which inode's BackingFile it uses, and whether this
  // particular open asked for write access (Open() computes it from the
  // kernel's requested flags; Create() likewise). Flush/Fsync consult this
  // because they act at a specific open's close/sync time, and Release()
  // uses it to maintain BackingFile::writable_refs.
  struct OpenFile {
    InodeId ino;
    bool writable = false;
  };

  // Opens a fresh shared backing descriptor for `id` (not yet in
  // backing_files_) and registers it for passthrough: O_RDWR if possible,
  // falling back to O_RDONLY only for a failure that specifically means
  // "this object cannot be opened for writing" (EACCES/EROFS/EPERM) --
  // anything else (e.g. ESTALE) is returned unchanged, exactly as a single
  // OpenNode call would report it. The returned BackingFile has refs == 0
  // and writable_refs == 0; the caller (Open()/Create()) sets those.
  absl::StatusOr<BackingFile> MakeBackingFile(InodeId id, FuseRequest &req);

  Context &ctx_;
  Options opts_;

  // fi.fh handles: never a raw pointer (fi.fh crosses the kernel boundary
  // and outlives nothing we control), just a small monotonically
  // increasing counter indexing open_files_.
  uint64_t next_handle_ = 1;
  absl::flat_hash_map<uint64_t, OpenFile> open_files_;
  // Keyed by inode, not by fi.fh: see BackingFile's comment.
  absl::flat_hash_map<InodeId, BackingFile> backing_files_;
  // The inodes whose BackingFile has writable_refs > 0; ctx_.open_for_write
  // points here (see Context::open_for_write).
  absl::flat_hash_set<int64_t> open_for_write_;
  // When the last sync point ran (or was attempted); see MaybeSyncBacking.
  absl::Time last_sync_;
};

}  // namespace dcfs

#endif  // DCFS_DIR_CACHE_FS_H_
