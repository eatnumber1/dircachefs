#include "dcfs/dir_cache_fs.h"

#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <cstdio>  // RENAME_NOREPLACE, RENAME_EXCHANGE
#include <fcntl.h>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <utility>
#include <vector>

#include "absl/log/check.h"
#include "absl/log/log.h"
#include "absl/status/status.h"
#include "absl/status/status_macros.h"
#include "absl/status/statusor.h"
#include "absl/strings/str_cat.h"
#include "absl/time/time.h"
#include "dcfs/backing.h"
#include "dcfs/context.h"
#include "dcfs/credentials.h"
#include "dcfs/fuse_request.h"
#include "dcfs/metadata_cache.h"
#include "dcfs/ret_check.h"
#include "dcfs/status.h"
#include "fuse_lowlevel.h"

namespace dcfs {

namespace {

// --- Side-effect xattrs ------------------------------------------------------
//
// The one place that lists the xattrs a backing filesystem changes as a
// side effect of an operation other than setxattr/removexattr. Phase 1 of
// each such operation marks them unknown (cache::BeginAttrChange), and its
// phase 3 reads them back (ResolveSideEffectXattrs), exactly as it does the
// attributes the operation changes.
//
//  - system.posix_acl_access: any chmod of a file with an access ACL
//    rewrites the ACL to match the new mode (posix_acl_chmod: ext4, xfs,
//    btrfs). dcfs chmods for FUSE_SET_ATTR_MODE and also for
//    KILL_SUID/KILL_SGID (backing::SetAttr). system.posix_acl_default is
//    never touched by a chmod.
//  - security.capability: removed by chown/chgrp (even chown(-1, -1)) and
//    truncate (ATTR_KILL_PRIV, fs/open.c, fs/attr.c) and by any write or
//    fallocate
//    (file_remove_privs). Note the kernel, since dcfs does not ask for
//    FUSE_CAP_HANDLE_KILLPRIV(_V2), also removes it itself through the
//    mount (a FUSE REMOVEXATTR ahead of the SETATTR or write), so this is
//    the backstop for what the backing filesystem does on its own.
//
// Not covered (not verified, and not something dcfs changes itself): LSM
// relabeling, security.evm/security.ima rewrites.
constexpr std::string_view kAclAccessXattr = "system.posix_acl_access";
constexpr std::string_view kCapabilityXattr = "security.capability";
constexpr std::string_view kXattrsChangedByWrite[] = {kCapabilityXattr};

// The side-effect xattrs of a Setattr with `to_set`.
std::vector<std::string_view> XattrsChangedBySetattr(int to_set) {
  std::vector<std::string_view> names;
  if (to_set & (FUSE_SET_ATTR_MODE | FUSE_SET_ATTR_KILL_SUID |
                FUSE_SET_ATTR_KILL_SGID)) {
    names.push_back(kAclAccessXattr);
  }
  // An otherwise empty setattr is a chown(path, -1, -1) (see
  // backing::SetAttr).
  if ((to_set & (FUSE_SET_ATTR_UID | FUSE_SET_ATTR_GID | FUSE_SET_ATTR_SIZE)) ||
      (to_set & ~FUSE_SET_ATTR_CTIME) == 0) {
    names.push_back(kCapabilityXattr);
  }
  return names;
}

}  // namespace

DirCacheFS::DirCacheFS(Context &ctx, Options opts)
    : ctx_(ctx), opts_(opts), last_sync_(absl::Now()) {
  ctx_.open_for_write = &open_for_write_;
}

DirCacheFS::~DirCacheFS() {
  if (ctx_.open_for_write == &open_for_write_) ctx_.open_for_write = nullptr;
}

absl::Status DirCacheFS::Init(struct fuse_conn_info &conn) {
  // conn.max_read is not one of the fields fuse_apply_conn_info_opts() sets
  // and fuse_session_new() leaves it zero-initialized, so a "-o
  // max_read=N" mount option (see Options::max_read) must be copied here
  // explicitly: do_init() (fuse_lowlevel.c) rejects the mount otherwise,
  // since it requires this to equal the max_read it parsed from the mount
  // options independently.
  if (opts_.max_read.has_value()) conn.max_read = *opts_.max_read;

  // fuse_set_feature_flag() only actually sets the flag (and returns true)
  // when the kernel's capable_ext says it supports it, so every one of
  // these is a no-op rather than a hard failure on an older kernel --
  // except FUSE_CAP_ATTR_GENERATION, which needs this build's libfuse patch
  // (see fuse_reply_attr_with_generation) as well as kernel support, so its
  // outcome is worth logging explicitly.
  fuse_set_feature_flag(&conn, FUSE_CAP_EXPORT_SUPPORT);
  bool attr_generation = fuse_set_feature_flag(&conn, FUSE_CAP_ATTR_GENERATION);
  fuse_set_feature_flag(&conn, FUSE_CAP_READDIRPLUS);
  fuse_set_feature_flag(&conn, FUSE_CAP_CACHE_SYMLINKS);
  // fuse_set_feature_flag() only sets want_ext (and returns true) when the
  // kernel's capable_ext -- populated from the FUSE_INIT request the
  // kernel sent before calling us -- already has the flag, so this return
  // value is the definitive "did the kernel grant passthrough" answer,
  // known here at startup rather than only per-open.
  bool passthrough = fuse_set_feature_flag(&conn, FUSE_CAP_PASSTHROUGH);
  // Unset, not left alone: do_init() (fuse_lowlevel.c) turns this on by
  // default -- before this callback ever runs -- whenever the kernel
  // offered it (LL_SET_DEFAULT(1, FUSE_CAP_ATOMIC_O_TRUNC)). With it on, the
  // kernel folds an O_TRUNC open into the OPEN request itself (O_TRUNC
  // arrives in fi.flags, with no separate SETATTR), which would need
  // Open() to apply the truncate itself. Turning it off instead makes the
  // kernel strip O_TRUNC from the OPEN (fuse_file_open) and truncate with a
  // plain SETATTR(size=0) right after it (do_open: vfs_open, then
  // handle_truncate), which Setattr's existing write-through handling of
  // FUSE_SET_ATTR_SIZE already does exactly right -- simpler than teaching
  // Open() a second, atomic-truncate code path for the same effect.
  fuse_unset_feature_flag(&conn, FUSE_CAP_ATOMIC_O_TRUNC);
  // Unset for the same reason (do_init() turns it on by default whenever
  // the kernel offers it: LL_SET_DEFAULT(1, FUSE_CAP_OVER_IO_URING)), and
  // this one is not optional: with FUSE_URING_ENABLE=1 in the environment
  // or -o io_uring (which --fuse_opt would pass through), libfuse would
  // then serve requests from one io_uring queue thread per CPU, calling
  // these handlers concurrently. dcfs is single-threaded by design: its
  // state (the open-file maps, open_for_write_, the SQLite connection, the
  // fill guards) has no locking (audit-races F4). libfuse checks this after
  // Init() returns, so unsetting it here is enough.
  fuse_unset_feature_flag(&conn, FUSE_CAP_OVER_IO_URING);
  // FUSE_BACKING_STACKED_OVER (1), not the default FUSE_BACKING_STACKED_UNDER
  // (0): dcfs's source directory is arbitrary and may itself be on a
  // stacked filesystem (e.g. overlayfs), which the default forbids
  // passthrough to (see the max_backing_stack_depth comment in
  // fuse_common.h). Note this is *not* what makes the kernel grant
  // passthrough at all -- fs/fuse/inode.c only requires the resulting
  // max_stack_depth (max_backing_stack_depth + 1, sent to the kernel) to
  // be > 0, which the default 0 already satisfies -- it only widens what
  // a backing file is allowed to be.
  conn.max_backing_stack_depth = FUSE_BACKING_STACKED_OVER;

  // POSIX ACLs. Without FUSE_CAP_POSIX_ACL the kernel checks permissions
  // on the mount (default_permissions) against the mode bits alone, and
  // on a file with an ACL the group bits are the ACL mask: a named-user
  // entry denying someone would be ignored, a named entry granting
  // someone would not be honoured. With it, the kernel fetches
  // system.posix_acl_access/default through GETXATTR (answered from the
  // cache, like any xattr) and enforces them. The kernel leaves the rest
  // to the filesystem: keeping the mode in sync with an ACL that is set,
  // and an ACL in sync with a chmod (the backing filesystem does both, and
  // dcfs reads the result back: see the side-effect xattrs above), and
  // default ACL inheritance on create, which the backing filesystem does
  // when the backing mkdirat/mknodat/openat runs.
  //
  // Inheritance is also why FUSE_CAP_DONT_MASK is needed with it: the
  // umask must not be applied where the parent has a default ACL, so it
  // must not be applied before the request reaches dcfs. With DONT_MASK
  // the kernel sends the requested mode unmasked plus the caller's umask
  // (fuse_ctx::umask), and the backing syscall runs with that umask (see
  // backing.cc's AsCaller), so the backing filesystem applies it exactly
  // where a local create would.
  //
  // Both are required, not optional: running without them would enforce
  // permissions differently from the backing filesystem. Every kernel
  // with FUSE_ATTR_GENERATION has both (protocol 7.26 and 7.12); should one
  // ever be missing, the flag is wanted anyway, which makes libfuse refuse
  // the INIT (want_flags_valid: EPROTO) and dcfs exit, rather than serve
  // the mount with the wrong permission checks.
  for (auto [flag, name] :
       {std::pair{FUSE_CAP_POSIX_ACL, "FUSE_CAP_POSIX_ACL"},
        std::pair{FUSE_CAP_DONT_MASK, "FUSE_CAP_DONT_MASK"}}) {
    if (!fuse_set_feature_flag(&conn, flag)) {
      LOG(ERROR) << "the kernel does not offer " << name
                 << ", which dcfs requires; refusing to mount";
      conn.want_ext |= flag;
    }
  }

  LOG(INFO) << "FUSE kernel protocol " << conn.proto_major << "."
            << conn.proto_minor << "; FUSE_CAP_ATTR_GENERATION "
            << (attr_generation ? "granted" : "NOT granted")
            << "; FUSE_CAP_PASSTHROUGH " << (passthrough ? "granted" : "NOT granted")
            << "; FUSE_CAP_POSIX_ACL and FUSE_CAP_DONT_MASK requested"
            << "; FUSE_CAP_ATOMIC_O_TRUNC and FUSE_CAP_OVER_IO_URING "
               "intentionally not requested";
  return absl::OkStatus();
}

absl::Status DirCacheFS::Destroy() {
  // The kernel has let go of every nodeid (it sends no FORGETs at
  // unmount).
  removed_.clear();
  lookups_.clear();
  return absl::OkStatus();
}

absl::StatusOr<cache::CachedAttr> DirCacheFS::RequireAttr(InodeId id) {
  absl::StatusOr<cache::CachedAttr> attr = cache::GetAttr(ctx_, id);
  if (!attr.ok() && absl::IsNotFound(attr.status()) &&
      id != cache::kRootInode) {
    // The kernel is still holding a nodeid we no longer have a row for
    // (e.g. its backing inode number was recycled and the old row
    // invalidated, or -- as with DirCacheFS::Open's second, LOOKUP_REVAL
    // retry after the first FUSE OPEN on a stale nodeid already replied
    // ESTALE and dropped the row -- a stale nodeid the kernel is about to
    // forget anyway): that is what ESTALE means to the kernel, not ENOENT.
    return dcfs::ErrnoToStatus(
        ESTALE, absl::StrCat("no cached row for nodeid ", id));
  }
  return attr;
}

absl::StatusOr<cache::CachedAttr> DirCacheFS::RequireAttrOrRemoved(
    InodeId id) {
  auto it = removed_.find(id);
  if (it == removed_.end()) return RequireAttr(id);
  ABSL_ASSIGN_OR_RETURN(struct statx stx, backing::StatFd(*it->second.fd));
  cache::CachedAttr attr = cache::WithStatx(it->second.row, stx);
  attr.valid = true;
  return attr;
}

absl::Status DirCacheFS::ReplyEntry(FuseRequest &req,
                                    const fuse_entry_param &entry) {
  RET_CHECK_NE(entry.ino, 0u);
  ABSL_RETURN_IF_ERROR(req.ReplyEntry(
      entry.ino, entry.generation, entry.attr,
      AttrTimeoutFor(static_cast<InodeId>(entry.ino)), opts_.entry_timeout));
  ++lookups_[static_cast<InodeId>(entry.ino)];
  return absl::OkStatus();
}

std::optional<FileDescriptor> DirCacheFS::HoldForRemoval(InodeId id) {
  if (!lookups_.contains(id)) return std::nullopt;
  absl::StatusOr<FileDescriptor> fd =
      backing::OpenNode(ctx_, id, O_PATH | O_NOFOLLOW);
  if (!fd.ok()) {
    LOG(WARNING) << "inode " << id << " is about to be removed but cannot "
                 << "be held for the kernel's remaining references to it, "
                 << "which will get ESTALE: " << fd.status();
    return std::nullopt;
  }
  return *std::move(fd);
}

absl::Status DirCacheFS::RetireRemoved(InodeId id,
                                       std::optional<FileDescriptor> held) {
  RET_CHECK(!HasOpenFiles(id)) << "retiring inode " << id
                               << " with an open file";
  absl::StatusOr<cache::CachedAttr> row = cache::GetAttr(ctx_, id);
  if (!row.ok() && !absl::IsNotFound(row.status())) return row.status();
  ABSL_RETURN_IF_ERROR(ForgetRemoved(id));
  // A row that was already gone (invalidated meanwhile) leaves nothing to
  // answer from.
  if (held.has_value() && row.ok() && lookups_.contains(id)) {
    removed_.insert_or_assign(id,
                              Removed{.row = *row, .fd = *std::move(held)});
  }
  return absl::OkStatus();
}

absl::StatusOr<fuse_entry_param> DirCacheFS::EntryFor(InodeId id) {
  ABSL_ASSIGN_OR_RETURN(cache::CachedAttr attr, RequireAttr(id));
  ABSL_ASSIGN_OR_RETURN(attr, FreshAttr(id, attr));

  fuse_entry_param entry{};
  entry.ino = static_cast<fuse_ino_t>(id);
  entry.generation = attr.fuse_gen;
  entry.attr = attr.st;
  entry.attr_timeout = absl::ToDoubleSeconds(AttrTimeoutFor(id));
  entry.entry_timeout = absl::ToDoubleSeconds(opts_.entry_timeout);
  return entry;
}

absl::Duration DirCacheFS::AttrTimeoutFor(InodeId id) const {
  // While a writable open is outstanding the kernel may change the file
  // behind dcfs's back in a way that does not invalidate its own attribute
  // cache either: a store through a MAP_SHARED mapping (a passthrough
  // write(2) does invalidate it). A zero timeout makes the kernel ask
  // again, and dcfs answers from a statx of the open fd (audit-races F1b).
  if (open_for_write_.contains(id)) return absl::ZeroDuration();
  return opts_.attr_timeout;
}

absl::Status DirCacheFS::RefreshAttrsOf(InodeId id, struct statx *fetched) {
  // Always the case for an inode open for writing (its attributes stay
  // unknown while the kernel may be writing to it): the fd is already open,
  // so this is one statx that touches no disk, not a reopen by handle.
  if (std::optional<int> fd = OpenFdOf(id); fd.has_value()) {
    return backing::RefreshAttrsFromFd(ctx_, id, *fd, fetched);
  }
  return backing::RefreshAttrs(ctx_, id, fetched);
}

void DirCacheFS::LogPhase3Failure(std::string_view op,
                                  const absl::Status &status) {
  if (status.ok()) return;
  LOG(WARNING) << op << ": the backing change happened, but recording it "
               << "in the cache failed; leaving it unknown: " << status;
}

absl::StatusOr<fuse_entry_param> DirCacheFS::EntryAfterPhase2(
    InodeId id, const struct statx &fetched) {
  if (fetched.stx_mask != 0) {
    // What phase 3 read, whether or not it could record it.
    ABSL_ASSIGN_OR_RETURN(cache::CachedAttr attr, RequireAttr(id));
    attr = cache::WithStatx(attr, fetched);
    fuse_entry_param entry{};
    entry.ino = static_cast<fuse_ino_t>(id);
    entry.generation = attr.fuse_gen;
    entry.attr = attr.st;
    entry.attr_timeout = absl::ToDoubleSeconds(AttrTimeoutFor(id));
    entry.entry_timeout = absl::ToDoubleSeconds(opts_.entry_timeout);
    return entry;
  }
  absl::StatusOr<fuse_entry_param> entry = EntryFor(id);
  if (entry.ok()) return entry;
  LogPhase3Failure("reply", entry.status());
  // Last resort: the row's last known attributes, marked unknown already
  // (phase 1), rather than failing an operation that happened.
  ABSL_ASSIGN_OR_RETURN(cache::CachedAttr attr, RequireAttr(id));
  fuse_entry_param fallback{};
  fallback.ino = static_cast<fuse_ino_t>(id);
  fallback.generation = attr.fuse_gen;
  fallback.attr = attr.st;
  fallback.attr_timeout = 0;
  fallback.entry_timeout = absl::ToDoubleSeconds(opts_.entry_timeout);
  return fallback;
}

absl::StatusOr<cache::CachedAttr> DirCacheFS::FreshAttr(
    InodeId id, cache::CachedAttr attr) {
  if (attr.valid) return attr;
  struct statx stx {};
  ABSL_RETURN_IF_ERROR(RefreshAttrsOf(id, &stx));
  // The row itself (fuse_gen, identity) may have changed meanwhile, e.g.
  // an invalidation by the open's identity check: re-read it.
  ABSL_ASSIGN_OR_RETURN(attr, RequireAttr(id));
  return cache::WithStatx(attr, stx);
}

absl::Status DirCacheFS::BeginWriting(InodeId id) {
  open_for_write_.insert(id);
  // Ends right away: for as long as the open lasts, open_for_write_ (not an
  // in-flight mutation) is what keeps the attributes unknown (see
  // backing::RefreshAttrs), and the kernel itself removes
  // security.capability through the mount before any write.
  return cache::BeginAttrChange(ctx_, id, kXattrsChangedByWrite).status();
}

void DirCacheFS::ResolveSideEffectXattrs(
    InodeId id, std::span<const std::string_view> names,
    std::optional<int> fd, std::string_view op) {
  for (std::string_view name : names) {
    absl::Status status = backing::RefreshXattr(ctx_, id, name, fd).status();
    // NotFound: the row is gone (invalidated meanwhile); nothing to record.
    if (!status.ok() && !absl::IsNotFound(status)) {
      LOG(WARNING) << op << ": could not read xattr " << name << " of inode "
                   << id << " back, leaving it unknown: " << status;
    }
  }
}

void DirCacheFS::MaybeSyncBacking() {
  if (!ctx_.dirty.any) return;
  absl::Time now = absl::Now();
  if (now - last_sync_ < opts_.sync_interval) return;
  SyncBackingNow("periodic");
}

void DirCacheFS::SyncBackingNow(std::string_view why) {
  if (!ctx_.dirty.any) return;
  last_sync_ = absl::Now();
  if (absl::Status status = backing::SyncBacking(ctx_); !status.ok()) {
    // Safe to carry on: the dirty entries stay, and only cost a larger
    // re-read after a crash. The next request past the interval retries.
    LOG(WARNING) << why << " sync of the backing filesystems failed, "
                 << "keeping the dirty set: " << status;
  }
}

absl::StatusOr<backing::NewChild> DirCacheFS::CreateChild(
    InodeId parent, std::string_view name,
    absl::FunctionRef<absl::Status(int parent_fd)> do_create,
    bool open_for_write) {
  // Missing row -> ESTALE; see RequireAttr().
  ABSL_RETURN_IF_ERROR(RequireAttr(parent).status());

  // Phase 1: mark (parent, name) unknown before touching the backing
  // filesystem, so a crash between here and phase 3 leaves "unknown"
  // (resolved on the next lookup) rather than stale; every other name of
  // `parent` keeps its state. Also marks `parent`'s attributes unknown
  // (refreshed below).
  std::vector<std::string> names = {std::string(name)};
  ABSL_ASSIGN_OR_RETURN(cache::Mutation mutation,
                        cache::BeginCreate(ctx_, parent, name));

  // Phase 2: the op-specific backing syscall, against a parent fd opened
  // once and shared with phase 3 below. On failure (EEXIST, ENOENT, ...)
  // `name` is re-resolved from the backing filesystem before the error is
  // returned, so phase 1's "unknown" doesn't linger on a name nothing else
  // is going to change -- e.g. a failed mkdir of an already-existing
  // directory must not leave that directory's own ".." unresolvable.
  ABSL_ASSIGN_OR_RETURN(
      FileDescriptor parent_fd,
      backing::OpenNode(ctx_, parent, O_RDONLY | O_DIRECTORY));
  if (absl::Status status = do_create(*parent_fd); !status.ok()) {
    mutation.End();
    ReresolveAfterFailure(parent, names);
    return status;
  }

  // Phase 3: probe and record the new child. Its dentry only if no other
  // mutation of `parent` overlapped this one (cache::Mutation::Owns);
  // otherwise `name` stays unknown.
  //
  // The object now exists, but the reply must name it by a row, so a
  // failure to record it is the one phase-3 failure still replied (the
  // name stays unknown; the next lookup finds the object).
  absl::StatusOr<backing::NewChild> child = backing::RecordNewChild(
      ctx_, mutation, parent, *parent_fd, name, open_for_write);
  mutation.End();
  if (!child.ok()) {
    LOG(WARNING) << "created " << name << " in directory " << parent
                 << " but could not record it: " << child.status();
    return child.status();
  }
  // Creating `name` changed `parent` itself too (mtime/ctime always; nlink
  // as well, if `name` is a new subdirectory -- its own ".." bumps
  // parent's link count), so its cached attributes are now stale. `fd` is
  // already open on it, so this is a free-standing statx, not a reopen.
  // From here on, failures are logged, not replied (audit-races F7).
  LogPhase3Failure("create",
                   backing::RefreshAttrsFromFd(ctx_, parent, *parent_fd));
  return child;
}

absl::Status DirCacheFS::Getattr(
    FuseRequest &req, fuse_ino_t ino, fuse_file_info *fi) {
  if (InodeId id = static_cast<InodeId>(ino); removed_.contains(id)) {
    ABSL_ASSIGN_OR_RETURN(cache::CachedAttr attr, RequireAttrOrRemoved(id));
    return req.ReplyAttr(attr.st, AttrTimeoutFor(id), attr.fuse_gen);
  }
  ABSL_ASSIGN_OR_RETURN(fuse_entry_param entry, EntryFor(static_cast<InodeId>(ino)));
  return req.ReplyAttr(entry.attr,
      AttrTimeoutFor(static_cast<InodeId>(entry.ino)), entry.generation);
}

absl::Status DirCacheFS::Setattr(
    FuseRequest &req, fuse_ino_t ino, struct stat *attr, int to_set,
    fuse_file_info *fi) {
  InodeId id = static_cast<InodeId>(ino);

  // Confirm this nodeid still has a row before changing anything (a
  // missing row is a stale nodeid: ESTALE, see RequireAttr). `fi` is not
  // consulted: identity here is by inode, not by whichever handle the
  // kernel happened to pass, and backing::SetAttr reopens the inode by
  // handle for whatever access each change needs (even when this inode has
  // a shared, possibly O_RDWR, backing fd open).
  ABSL_RETURN_IF_ERROR(RequireAttr(id).status());
  ABSL_ASSIGN_OR_RETURN(Credentials caller, req.Caller());

  // Phase 1 of the write-through rule (see backing.cc's file comment):
  // mark the cached attributes, and the xattrs the backing filesystem
  // changes as a side effect, unknown before the syscall(s) below, so a
  // crash before phase 3 leaves "unknown" -- repopulated on the next
  // access -- rather than ever reporting stale data as current.
  const std::vector<std::string_view> side_effects =
      XattrsChangedBySetattr(to_set);
  ABSL_ASSIGN_OR_RETURN(cache::Mutation mutation,
                        cache::BeginAttrChange(ctx_, id, side_effects));

  // Phase 2: the syscall(s) themselves.
  absl::Status set_status =
      backing::SetAttr(ctx_, caller, id, *attr, to_set);
  // Phase 3 is refreshes only, which run as ordinary fills.
  mutation.End();
  if (!set_status.ok()) {
    // Best effort: refresh right away rather than leaving the row
    // "unknown" until whatever the next access happens to be, but a
    // failure here must never shadow `set_status`, which is what actually
    // gets reported below.
    // (A side-effect xattr stays unknown, for its next reader.)
    backing::RefreshAttrs(ctx_, id).IgnoreError();
    return set_status;
  }

  // Phase 3: write the new state, then reply with it. The change has
  // happened: a failure from here on is logged, never replied
  // (audit-races F7).
  struct statx stx {};
  LogPhase3Failure("Setattr", backing::RefreshAttrs(ctx_, id, &stx));
  ResolveSideEffectXattrs(id, side_effects, OpenFdOf(id), "Setattr");
  ABSL_ASSIGN_OR_RETURN(fuse_entry_param entry, EntryAfterPhase2(id, stx));
  return req.ReplyAttr(entry.attr,
      AttrTimeoutFor(static_cast<InodeId>(entry.ino)), entry.generation);
}

absl::Status DirCacheFS::Lookup(
    FuseRequest &req, fuse_ino_t parent_ino, std::string_view name) {
  InodeId parent = static_cast<InodeId>(parent_ino);

  if (name == ".") {
    ABSL_ASSIGN_OR_RETURN(fuse_entry_param entry, EntryFor(parent));
    return ReplyEntry(req, entry);
  }
  if (name == "..") {
    ABSL_ASSIGN_OR_RETURN(InodeId up, backing::ParentOf(ctx_, parent));
    ABSL_ASSIGN_OR_RETURN(fuse_entry_param entry, EntryFor(up));
    return ReplyEntry(req, entry);
  }

  ABSL_ASSIGN_OR_RETURN(
      cache::LookupResult result, backing::LookupOrPopulate(ctx_, parent, name));
  if (result.kind == cache::LookupResult::kNegative) {
    return req.ReplyNegativeEntry(opts_.entry_timeout);
  }
  // LookupOrPopulate never returns kUnknown -- it always resolves to either
  // a positive or a (possibly freshly-cached) negative entry.
  RET_CHECK_EQ(result.kind, cache::LookupResult::kFound);
  ABSL_ASSIGN_OR_RETURN(fuse_entry_param entry, EntryFor(result.id));
  return ReplyEntry(req, entry);
}

namespace {

// Takes `n` of the kernel's lookups of `id` off `lookups`; returns whether
// that was its last one.
bool DropLookups(absl::flat_hash_map<InodeId, uint64_t> &lookups, InodeId id,
                 uint64_t n) {
  auto it = lookups.find(id);
  if (it == lookups.end() || it->second < n) {
    // dcfs counted fewer lookups than the kernel holds: some reply that
    // handed out `id` was not counted. A removed record for it may then
    // go too early (its last references get ESTALE), never too late.
    LOG(ERROR) << "FORGET of " << n << " lookups of nodeid " << id
               << ", but only "
               << (it == lookups.end() ? 0 : it->second) << " counted";
    if (it != lookups.end()) lookups.erase(it);
    return true;
  }
  it->second -= n;
  if (it->second > 0) return false;
  lookups.erase(it);
  return true;
}

}  // namespace

void DirCacheFS::Forget(FuseRequest &req, fuse_ino_t ino, uint64_t nlookup) {
  InodeId id = static_cast<InodeId>(ino);
  // The last FORGET of a removed object closes its descriptor, which lets
  // the backing filesystem free it.
  if (DropLookups(lookups_, id, nlookup)) removed_.erase(id);
  // forget/forget_multi have no error reply: fuse_reply_none is the only
  // valid one.
  req.ReplyNone();
}

void DirCacheFS::ForgetMulti(
    FuseRequest &req, std::span<const fuse_forget_data> forgets) {
  for (const fuse_forget_data &forget : forgets) {
    InodeId id = static_cast<InodeId>(forget.ino);
    if (DropLookups(lookups_, id, forget.nlookup)) removed_.erase(id);
  }
  req.ReplyNone();
}

absl::Status DirCacheFS::Readlink(FuseRequest &req, fuse_ino_t ino) {
  InodeId id = static_cast<InodeId>(ino);
  if (auto it = removed_.find(id); it != removed_.end()) {
    ABSL_ASSIGN_OR_RETURN(std::string target,
                          backing::ReadSymlinkFd(*it->second.fd));
    return req.ReplyReadlink(target);
  }
  absl::StatusOr<std::string> target = cache::Readlink(ctx_, id);
  if (target.ok()) return req.ReplyReadlink(*target);
  if (!absl::IsNotFound(target.status())) return target.status();

  // Not cached yet. Confirm this really is a symlink (rather than, say, a
  // caller racing a stale nodeid) before reading the backing filesystem.
  ABSL_ASSIGN_OR_RETURN(cache::CachedAttr attr, RequireAttr(id));
  ABSL_ASSIGN_OR_RETURN(attr, FreshAttr(id, attr));
  RET_CHECK(S_ISLNK(attr.st.st_mode))
      << "Readlink on non-symlink inode " << id;

  const cache::FillSnapshot snapshot = cache::BeginFill(ctx_);
  ABSL_ASSIGN_OR_RETURN(std::string real_target, backing::ReadSymlink(ctx_, id));
  ABSL_RETURN_IF_ERROR(
      cache::FillSymlink(ctx_, snapshot, id, real_target).status());
  return req.ReplyReadlink(real_target);
}

absl::Status DirCacheFS::Mknod(
    FuseRequest &req, fuse_ino_t parent_ino, std::string_view name,
    mode_t mode, dev_t rdev) {
  InodeId parent = static_cast<InodeId>(parent_ino);
  ABSL_ASSIGN_OR_RETURN(Credentials caller, req.Caller());
  ABSL_ASSIGN_OR_RETURN(
      backing::NewChild child,
      CreateChild(parent, name, [&](int parent_fd) {
        return backing::MknodAt(ctx_, caller, parent_fd, name, mode, rdev);
      }));
  ABSL_ASSIGN_OR_RETURN(fuse_entry_param entry, EntryFor(child.id));
  return ReplyEntry(req, entry);
}

absl::Status DirCacheFS::Mkdir(
    FuseRequest &req, fuse_ino_t parent_ino, std::string_view name,
    mode_t mode) {
  InodeId parent = static_cast<InodeId>(parent_ino);
  ABSL_ASSIGN_OR_RETURN(Credentials caller, req.Caller());
  ABSL_ASSIGN_OR_RETURN(
      backing::NewChild child,
      CreateChild(parent, name, [&](int parent_fd) {
        return backing::MkdirAt(ctx_, caller, parent_fd, name, mode);
      }));
  ABSL_ASSIGN_OR_RETURN(fuse_entry_param entry, EntryFor(child.id));
  return ReplyEntry(req, entry);
}

// Unlink, Rmdir and Rename never call fuse_lowlevel_notify_inval_entry():
// the kernel itself drops its dentries for every name these ops touch
// (d_delete/d_move on success), and picks up the changed nlink/ctime of the
// inodes involved from the attributes our subsequent LOOKUP/GETATTR replies
// carry. Under the exclusive-access model nothing else can have changed
// those names behind the kernel's back, so there is nothing left for dcfs
// to invalidate in the kernel.

absl::Status DirCacheFS::Unlink(
    FuseRequest &req, fuse_ino_t parent_ino, std::string_view name) {
  return RemoveChild(req, static_cast<InodeId>(parent_ino), name,
                     /*is_dir=*/false);
}

absl::Status DirCacheFS::Rmdir(
    FuseRequest &req, fuse_ino_t parent_ino, std::string_view name) {
  return RemoveChild(req, static_cast<InodeId>(parent_ino), name,
                     /*is_dir=*/true);
}

absl::Status DirCacheFS::RemoveChild(
    FuseRequest &req, InodeId parent, std::string_view name, bool is_dir) {
  // Missing row -> ESTALE; see RequireAttr().
  ABSL_RETURN_IF_ERROR(RequireAttr(parent).status());
  ABSL_ASSIGN_OR_RETURN(Credentials caller, req.Caller());

  // The child's id is needed for phase 3 (its row's fate depends on its
  // remaining link count), so an uncached name is resolved first.
  ABSL_ASSIGN_OR_RETURN(cache::LookupResult child,
                        backing::LookupOrPopulate(ctx_, parent, name));
  if (child.kind == cache::LookupResult::kNegative) {
    return req.ReplyErrno(ENOENT);
  }
  RET_CHECK_EQ(child.kind, cache::LookupResult::kFound);

  // Phase 1: mark (parent, name) unknown and mark the attributes that are about
  // to change (the parent's mtime/ctime/nlink, the child's nlink/ctime)
  // unknown, in one transaction.
  std::vector<std::string> names = {std::string(name)};
  ABSL_ASSIGN_OR_RETURN(cache::Mutation mutation,
                        cache::BeginRemove(ctx_, parent, name, child.id));
  // The kernel will keep asking about the child if something still refers
  // to it (a working directory, an O_PATH descriptor); once removed, it is
  // reachable only through a descriptor taken now.
  std::optional<FileDescriptor> held = HoldForRemoval(child.id);

  // Phase 2: the backing unlinkat. On failure (ENOTEMPTY, EBUSY, ...) the
  // error is returned as is, after a best-effort re-resolve (see
  // ReresolveAfterFailure).
  if (absl::Status status =
          backing::UnlinkAt(ctx_, caller, parent, name,
                            is_dir ? AT_REMOVEDIR : 0);
      !status.ok()) {
    mutation.End();
    ReresolveAfterFailure(parent, names);
    return status;
  }

  // Phase 3. The unlink has happened: from here on each failure is logged
  // and leaves what it did not record unknown, never replied
  // (audit-races F7). The name is now known absent -- unless another
  // mutation of `parent` overlapped this one (then it stays unknown).
  if (mutation.Owns(parent)) {
    LogPhase3Failure("Unlink/Rmdir", cache::SetNegative(ctx_, parent, name));
  }
  mutation.End();
  LogPhase3Failure("Unlink/Rmdir", backing::RefreshAttrs(ctx_, parent));
  if (is_dir) {
    // A directory has exactly one link that matters here and no open-file
    // state in dcfs (Opendir keeps nothing), so the backing rmdir removed
    // it for good; its (necessarily empty) children cascade away with it.
    LogPhase3Failure("Rmdir", RetireRemoved(child.id, std::move(held)));
  } else {
    LogPhase3Failure("Unlink", SettleUnlinkedFile(child.id, std::move(held)));
  }
  return req.ReplyErrno(0);
}

absl::Status DirCacheFS::Symlink(
    FuseRequest &req, std::string_view link, fuse_ino_t parent_ino,
    std::string_view name) {
  InodeId parent = static_cast<InodeId>(parent_ino);
  ABSL_ASSIGN_OR_RETURN(Credentials caller, req.Caller());
  ABSL_ASSIGN_OR_RETURN(
      backing::NewChild child,
      CreateChild(parent, name, [&](int parent_fd) {
        return backing::SymlinkAt(ctx_, caller, parent_fd, name, link);
      }));
  ABSL_ASSIGN_OR_RETURN(fuse_entry_param entry, EntryFor(child.id));
  return ReplyEntry(req, entry);
}

absl::Status DirCacheFS::Rename(
    FuseRequest &req, fuse_ino_t parent_ino, std::string_view name,
    fuse_ino_t newparent_ino, std::string_view newname, unsigned int flags) {
  InodeId parent = static_cast<InodeId>(parent_ino);
  InodeId newparent = static_cast<InodeId>(newparent_ino);
  // RENAME_WHITEOUT (overlayfs's) and anything unknown are not supported.
  if (flags != 0 && flags != RENAME_NOREPLACE && flags != RENAME_EXCHANGE) {
    return req.ReplyErrno(EINVAL);
  }
  const bool exchange = flags == RENAME_EXCHANGE;

  // Missing row -> ESTALE for both parents; see RequireAttr().
  ABSL_RETURN_IF_ERROR(RequireAttr(parent).status());
  ABSL_RETURN_IF_ERROR(RequireAttr(newparent).status());
  ABSL_ASSIGN_OR_RETURN(Credentials caller, req.Caller());

  ABSL_ASSIGN_OR_RETURN(cache::LookupResult src,
                        backing::LookupOrPopulate(ctx_, parent, name));
  if (src.kind == cache::LookupResult::kNegative) {
    return req.ReplyErrno(ENOENT);
  }
  RET_CHECK_EQ(src.kind, cache::LookupResult::kFound);
  // The destination is resolved (populating newparent if need be) rather
  // than merely looked up: if the rename replaces an existing object, its
  // row -- which may be cached through another hard link, or an NFS
  // handle, even when this name is not -- must learn its new link count
  // (or be deleted) in phase 3, and that needs its id.
  ABSL_ASSIGN_OR_RETURN(cache::LookupResult dst,
                        backing::LookupOrPopulate(ctx_, newparent, newname));
  RET_CHECK_NE(dst.kind, cache::LookupResult::kUnknown);
  const bool dst_exists = dst.kind == cache::LookupResult::kFound;
  if (exchange && !dst_exists) return req.ReplyErrno(ENOENT);
  // Two links to one inode: the kernel's vfs_rename() treats this as a
  // successful no-op and never sends it, but dcfs handles it the same way
  // should one arrive (e.g. through a stale kernel dentry).
  const bool same_inode = dst_exists && dst.id == src.id;

  // Phase 1, one transaction: mark both names unknown, and every attribute
  // set the rename changes unknown.
  std::vector<std::string> names = {std::string(name)};
  std::vector<std::string> newnames = {std::string(newname)};
  ABSL_ASSIGN_OR_RETURN(
      cache::Mutation mutation,
      cache::BeginRename(ctx_, parent, name, newparent, newname, src.id,
                         dst_exists && !same_inode
                             ? std::optional<InodeId>(dst.id)
                             : std::nullopt));
  // A replaced dst is removed as an unlink would remove it (see
  // RemoveChild).
  std::optional<FileDescriptor> held_dst;
  if (dst_exists && !same_inode && !exchange) {
    held_dst = HoldForRemoval(dst.id);
  }

  // Phase 2: the backing renameat2. On failure (EXDEV, ENOTEMPTY, EEXIST
  // for RENAME_NOREPLACE, ...) the error is returned unchanged, after a
  // best-effort re-resolve (see ReresolveAfterFailure).
  if (absl::Status status =
          backing::RenameAt(ctx_, caller, parent, name, newparent, newname,
                            flags);
      !status.ok()) {
    mutation.End();
    ReresolveAfterFailure(parent, names);
    ReresolveAfterFailure(newparent, newnames);
    return status;
  }

  // The rename has happened: from here on each failure is logged and
  // leaves what it did not record unknown, never replied (audit-races F7).
  //
  // Phase 3, one transaction: the dentries as they now are. Moving a
  // directory moves only its own dentry; its cached subtree hangs off its
  // (unchanged) id and so stays valid as is. Each parent's dentry only if
  // no other mutation of it overlapped this one (cache::Mutation::Owns);
  // otherwise that name stays unknown.
  LogPhase3Failure("Rename", ctx_.db.Transaction([&]() -> absl::Status {
    const bool own_parent = mutation.Owns(parent);
    const bool own_newparent = mutation.Owns(newparent);
    if (own_newparent) {
      ABSL_RETURN_IF_ERROR(
          cache::LinkDentry(ctx_, newparent, newname, src.id));
    }
    if (own_parent) {
      if (exchange || same_inode) {
        ABSL_RETURN_IF_ERROR(cache::LinkDentry(ctx_, parent, name, dst.id));
      } else {
        ABSL_RETURN_IF_ERROR(cache::SetNegative(ctx_, parent, name));
      }
    }
    return absl::OkStatus();
  }));
  mutation.End();

  RefreshAfterRename(parent, newparent, src, dst, dst_exists, same_inode,
                     exchange, std::move(held_dst));
  return req.ReplyErrno(0);
}

void DirCacheFS::RefreshAfterRename(
    InodeId parent, InodeId newparent, cache::LookupResult src,
    cache::LookupResult dst, bool dst_exists, bool same_inode,
    bool exchange, std::optional<FileDescriptor> held_dst) {
  // These need syscalls: both parents' mtime (and nlink, when a directory
  // moved between them), and the ctime of every inode the rename touched.
  LogPhase3Failure("Rename", backing::RefreshAttrs(ctx_, parent));
  if (newparent != parent) {
    LogPhase3Failure("Rename", backing::RefreshAttrs(ctx_, newparent));
  }
  LogPhase3Failure("Rename", backing::RefreshAttrs(ctx_, src.id));
  if (dst_exists && !same_inode) {
    if (exchange) {
      LogPhase3Failure("Rename", backing::RefreshAttrs(ctx_, dst.id));
    } else if (absl::StatusOr<cache::CachedAttr> dst_attr = RequireAttr(dst.id);
               !dst_attr.ok()) {
      LogPhase3Failure("Rename", dst_attr.status());
    } else if (S_ISDIR(dst_attr->st.st_mode)) {
      // Replaced. A directory can only have been replaced if it was empty,
      // and is gone for good; a file follows Unlink's row-lifetime rule.
      LogPhase3Failure("Rename", RetireRemoved(dst.id, std::move(held_dst)));
    } else {
      LogPhase3Failure("Rename",
                       SettleUnlinkedFile(dst.id, std::move(held_dst)));
    }
  }
}

void DirCacheFS::ReresolveAfterFailure(
    InodeId parent, std::span<const std::string> names) {
  // Phase 1 left `names` unknown, which is safe but not free: e.g. a
  // directory whose own dentry is unknown has no cached parent, so its ".."
  // costs a trip to the backing filesystem (backing::ParentOf) until the
  // name is resolved. Re-reading just these names from the backing
  // filesystem now (one probe each, backing::ResolveName; not assuming the
  // failed syscall changed nothing) restores that. Best effort: the op's
  // own error is what gets replied, whatever happens here.
  for (const std::string &name : names) {
    backing::LookupOrPopulate(ctx_, parent, name).IgnoreError();
  }
}

absl::Status DirCacheFS::ForgetRemoved(InodeId id) {
  absl::Status status = cache::DeleteInode(ctx_, id);
  // Already gone is fine: nothing else can be relying on the row.
  if (absl::IsNotFound(status)) return absl::OkStatus();
  return status;
}

absl::Status DirCacheFS::SettleUnlinkedFile(
    InodeId id, std::optional<FileDescriptor> held) {
  // Row lifetime: a row is deleted once its backing nlink reaches 0 AND
  // dcfs holds no open file on it. While dcfs has one open, its fd is the
  // reliable way to stat the object (the kernel and NFS clients may keep
  // using the nodeid, and passthrough keeps the backing file alive); Release
  // retires the row when the last open closes with nlink 0.
  if (std::optional<int> fd = OpenFdOf(id); fd.has_value()) {
    return backing::RefreshAttrsFromFd(ctx_, id, *fd);
  }
  if (held.has_value()) {
    // The descriptor held across the unlink still reaches the object
    // whether or not links remain.
    ABSL_ASSIGN_OR_RETURN(struct statx stx, backing::StatFd(**held));
    if (stx.stx_nlink == 0) return RetireRemoved(id, std::move(held));
    return backing::RefreshAttrsFromFd(ctx_, id, **held);
  }
  ABSL_ASSIGN_OR_RETURN(std::optional<uint64_t> nlink,
                        backing::BackingNlink(ctx_, id));
  // nullopt: the last link went and nothing held it open, so the handle no
  // longer decodes and BackingNlink has already invalidated the row.
  if (!nlink.has_value()) return absl::OkStatus();
  // 0 with no dcfs open: something outside dcfs still pins the backing
  // inode, but nothing can reach it through dcfs any more.
  if (*nlink == 0) return RetireRemoved(id, std::nullopt);
  return backing::RefreshAttrs(ctx_, id);
}

absl::Status DirCacheFS::Link(
    FuseRequest &req, fuse_ino_t ino, fuse_ino_t newparent_ino,
    std::string_view newname) {
  InodeId src = static_cast<InodeId>(ino);
  InodeId newparent = static_cast<InodeId>(newparent_ino);

  // Missing row -> ESTALE for both ends; see RequireAttr().
  ABSL_RETURN_IF_ERROR(RequireAttr(src).status());
  ABSL_RETURN_IF_ERROR(RequireAttr(newparent).status());

  // Phase 1: mark (newparent, newname) unknown, and the attributes the
  // link changes (src's nlink/ctime, newparent's mtime/ctime/size).
  std::vector<std::string> names = {std::string(newname)};
  ABSL_ASSIGN_OR_RETURN(cache::Mutation mutation,
                        cache::BeginLink(ctx_, src, newparent, newname));

  // Phase 2: the backing linkat. On failure (EEXIST, EXDEV, ...)
  // (newparent, newname) is re-resolved from the backing filesystem (see
  // CreateChild's identical handling) before the error (errno payload
  // intact) is returned.
  if (absl::Status status = backing::LinkAt(ctx_, src, newparent, newname);
      !status.ok()) {
    mutation.End();
    ReresolveAfterFailure(newparent, names);
    // Best effort, as Setattr: the op's own error is what gets replied.
    RefreshAttrsOf(src).IgnoreError();
    backing::RefreshAttrs(ctx_, newparent).IgnoreError();
    return status;
  }

  // Phase 3: record the new dentry and the bumped nlink together. The link
  // exists: failures from here on are logged, never replied
  // (audit-races F7).
  LogPhase3Failure(
      "Link", backing::RecordNewLink(ctx_, mutation, src, newparent, newname)
                  .status());
  mutation.End();
  // Adding a dentry changed newparent's own mtime/ctime (and, on some
  // filesystems, its on-disk size); no fd on it is already open here (only
  // LinkAt, inside backing.cc, opened one, and briefly), so this is a full
  // reopen+statx rather than the fd-based refresh CreateChild uses.
  LogPhase3Failure("Link", backing::RefreshAttrs(ctx_, newparent));

  ABSL_ASSIGN_OR_RETURN(fuse_entry_param entry, EntryAfterPhase2(src));
  return ReplyEntry(req, entry);
}

absl::StatusOr<DirCacheFS::BackingFile> DirCacheFS::MakeBackingFile(
    InodeId id, FuseRequest &req) {
  absl::StatusOr<FileDescriptor> fd =
      backing::OpenNode(ctx_, id, O_RDWR | O_CLOEXEC);
  bool writable = true;
  if (!fd.ok()) {
    int err = GetErrnoFromStatus(fd.status()).value_or(0);
    // Only an error that specifically means "this object cannot be opened
    // for writing" (an immutable or append-only file, or a read-only
    // backing filesystem) falls back to O_RDONLY; anything else (ESTALE,
    // ENOENT, ...) is returned exactly as a single OpenNode call would
    // report it, unchanged.
    if (err != EACCES && err != EROFS && err != EPERM) return fd.status();
    writable = false;
    ABSL_ASSIGN_OR_RETURN(fd, backing::OpenNode(ctx_, id, O_RDONLY | O_CLOEXEC));
  }

  // Ask the kernel to serve reads/writes directly against `fd`. A 0
  // backing_id means either the kernel never granted FUSE_CAP_PASSTHROUGH
  // (logged once at Init) or this registration otherwise failed; either way
  // it is not a dcfs-level error -- Read()/Write() below fall back to
  // serving the data themselves. This is called at most once per inode
  // (see BackingFile's comment): every later Open()/Create() of the same
  // inode reuses the backing_id this call returns.
  ABSL_ASSIGN_OR_RETURN(int backing_id, req.PassthroughOpen(**fd));

  return BackingFile{
      .fd = *std::move(fd), .backing_id = backing_id, .writable = writable};
}

absl::Status DirCacheFS::Open(
    FuseRequest &req, fuse_ino_t ino, fuse_file_info &fi) {
  InodeId id = static_cast<InodeId>(ino);

  // RequireAttr(), not cache::GetAttr(): the kernel's generic open path
  // (do_file_open_root, fs/namei.c) automatically retries a failed open
  // once with LOOKUP_REVAL after -ESTALE, and that second FUSE OPEN hits
  // this same call against the now-invalidated row -- which must come
  // back ESTALE again, not ENOENT.
  ABSL_ASSIGN_OR_RETURN(cache::CachedAttr attr, RequireAttr(id));
  ABSL_ASSIGN_OR_RETURN(attr, FreshAttr(id, attr));
  // The kernel calls opendir(), not open(), on a directory, so this would
  // only trip on a row that changed type out from under a stale nodeid.
  RET_CHECK(!S_ISDIR(attr.st.st_mode)) << "Open on directory inode " << id;
  // See Init()'s comment: FUSE_CAP_ATOMIC_O_TRUNC is deliberately not
  // granted, so the kernel strips O_TRUNC from the OPEN and truncates with
  // its own SETATTR(size=0) right after it, instead of folding the
  // truncate into this call.
  RET_CHECK(!(fi.flags & O_TRUNC))
      << "O_TRUNC open reached DirCacheFS::Open for inode " << id;

  // One shared backing fd serves every open of this inode, whatever access
  // mode each one asks for (see BackingFile's comment): reuse it if some
  // other open of `id` is already outstanding, else create it fresh.
  auto backing_it = backing_files_.find(id);
  int backing_id;
  if (backing_it == backing_files_.end()) {
    ABSL_ASSIGN_OR_RETURN(BackingFile backing_file, MakeBackingFile(id, req));
    backing_id = backing_file.backing_id;
    backing_it = backing_files_.emplace(id, std::move(backing_file)).first;
  } else {
    backing_id = backing_it->second.backing_id;
  }
  backing_it->second.refs++;
  bool writable = (fi.flags & O_ACCMODE) != O_RDONLY;
  if (writable) {
    backing_it->second.writable_refs++;
    // Phase 1 for every write the kernel will make through the passthrough
    // fd, before the open is replied to: a crash while it is open leaves
    // "unknown", never the pre-write size/mtime marked current.
    if (absl::Status status = BeginWriting(id); !status.ok()) {
      // Undo the registration above (the open is failing, so no Release
      // will ever come for it).
      if (--backing_it->second.writable_refs == 0) open_for_write_.erase(id);
      if (--backing_it->second.refs == 0) {
        if (backing_id > 0) req.PassthroughClose(backing_id).IgnoreError();
        backing_files_.erase(backing_it);
      }
      return status;
    }
  }

  if (backing_id > 0) fi.backing_id = backing_id;
  // A passthrough open must drop any stale page cache for this file
  // (fi.keep_cache = 0 is also correct, if redundant, on the fallback
  // path). Leave fi.direct_io at its default 0 -- passthrough requires the
  // default cache mode, and the fallback path benefits from the normal
  // page cache like any other file.
  fi.keep_cache = 0;

  uint64_t handle = next_handle_++;
  open_files_.emplace(handle, OpenFile{.ino = id, .writable = writable});
  fi.fh = handle;

  return req.ReplyOpen(fi);
}

absl::Status DirCacheFS::Read(
    FuseRequest &req, fuse_ino_t ino, size_t size, off_t off,
    fuse_file_info &fi) {
  // The fallback path: only reached when Open() did not get passthrough
  // for this inode (or, per fuse_passthrough_open()'s contract, in the
  // unlikely case the kernel sends READ anyway despite passthrough).
  auto it = open_files_.find(fi.fh);
  RET_CHECK(it != open_files_.end()) << "Read on unknown handle " << fi.fh;
  auto backing_it = backing_files_.find(it->second.ino);
  RET_CHECK(backing_it != backing_files_.end())
      << "Read on inode " << it->second.ino << " with no BackingFile";
  ABSL_ASSIGN_OR_RETURN(
      std::string buf, backing::ReadFile(*backing_it->second.fd, size, off));
  return req.ReplyBuf(buf);
}

absl::Status DirCacheFS::Write(
    FuseRequest &req, fuse_ino_t ino, std::span<const char> buf, off_t off,
    fuse_file_info &fi) {
  // The fallback path: with passthrough granted, writes go straight from
  // the kernel to the shared backing fd and WRITE is never sent for this
  // inode; only reached when Open() did not get passthrough for it.
  auto it = open_files_.find(fi.fh);
  RET_CHECK(it != open_files_.end()) << "Write on unknown handle " << fi.fh;
  InodeId id = it->second.ino;
  auto backing_it = backing_files_.find(id);
  RET_CHECK(backing_it != backing_files_.end())
      << "Write on inode " << id << " with no BackingFile";

  // Phase 1: the size/mtime/ctime (and side-effect xattrs) this write is
  // about to change. (A writable open already made `id` durably dirty, so
  // this commits without a WAL fsync unless a sync point has cleared that
  // meanwhile.) Phase 3 of the xattrs is the last writable Release().
  ABSL_ASSIGN_OR_RETURN(
      cache::Mutation mutation,
      cache::BeginAttrChange(ctx_, id, kXattrsChangedByWrite));

  // Phase 2: the write itself, against the shared fd (EBADF if it is
  // O_RDONLY -- see MakeBackingFile -- exactly as the kernel would report
  // for a write against a read-only fd).
  ABSL_ASSIGN_OR_RETURN(
      size_t n, backing::WriteFile(*backing_it->second.fd, buf, off));

  // Phase 3: nothing yet -- Flush/Release/Fsync (below) pick up the fresh
  // size/mtime/ctime, not every individual write.
  return req.ReplyWrite(n);
}

void DirCacheFS::RecordWrittenAttrs(InodeId id, int fd, std::string_view op) {
  absl::Status status = backing::RefreshAttrsFromFd(ctx_, id, fd);
  if (status.ok()) return;
  // Never fatal to the op (see this method's declaration): the data is on
  // the backing filesystem regardless, and "unknown" attributes simply
  // repopulate on the next access. BeginWriting already marked them
  // unknown; marking again only matters if the failure came after a
  // partial write of the row, and is itself best effort.
  LOG(WARNING) << op << ": could not refresh the attributes of inode " << id
               << " from its open fd, leaving them unknown: " << status;
  absl::Status marked = cache::MarkAttrsUnknown(ctx_, id);
  if (!marked.ok() && !absl::IsNotFound(marked)) {
    LOG(WARNING) << op << ": could not mark the attributes of inode " << id
                 << " unknown either: " << marked;
  }
}

absl::Status DirCacheFS::Flush(
    FuseRequest &req, fuse_ino_t ino, fuse_file_info &fi) {
  auto it = open_files_.find(fi.fh);
  RET_CHECK(it != open_files_.end()) << "Flush on unknown handle " << fi.fh;
  if (it->second.writable) {
    // The passthrough (or fallback Write()) writes this open may have made
    // are invisible to the cache until now; record their effect on
    // size/mtime/ctime/etc. right away. The attributes nevertheless stay
    // marked unknown (see BeginWriting) until the Release() of the last
    // writable open -- this open's own Release() is still to come -- and
    // meanwhile every attribute read is served by a statx of the shared fd
    // (RefreshAttrsOf), so a stat() right after close() sees the writes.
    // A failure is logged, not replied: Flush has no bookkeeping to skip,
    // and failing close(2) over a cache refresh would be wrong.
    InodeId id = it->second.ino;
    auto backing_it = backing_files_.find(id);
    RET_CHECK(backing_it != backing_files_.end())
        << "Flush on inode " << id << " with no BackingFile";
    RecordWrittenAttrs(id, *backing_it->second.fd, "Flush");
  }
  return req.ReplyErrno(0);
}

absl::Status DirCacheFS::Release(
    FuseRequest &req, fuse_ino_t ino, fuse_file_info &fi) {
  // The kernel ignores RELEASE errors, so nothing below may skip any of
  // this open's bookkeeping: the open-file entry is always dropped, the
  // refcounts always decremented, and on the last reference the
  // passthrough registration and our fd are always closed. Cache
  // failures along the way are logged at WARNING and leave the
  // attributes unknown (to repopulate on the next access); the reply is
  // always 0. Only a broken internal invariant (RET_CHECK) returns early.
  auto it = open_files_.find(fi.fh);
  RET_CHECK(it != open_files_.end()) << "Release on unknown handle " << fi.fh;
  InodeId id = it->second.ino;
  bool writable = it->second.writable;
  open_files_.erase(it);

  auto backing_it = backing_files_.find(id);
  RET_CHECK(backing_it != backing_files_.end())
      << "Release on inode " << id << " with no BackingFile";
  BackingFile &backing_file = backing_it->second;
  RET_CHECK_GT(backing_file.refs, 0)
      << "Release: refs underflow for inode " << id;
  RET_CHECK(!writable || backing_file.writable_refs > 0)
      << "Release: writable_refs underflow for inode " << id;
  --backing_file.refs;
  if (writable && --backing_file.writable_refs == 0) {
    // Phase 3 of the passthrough writes (see BeginWriting): the last
    // writable open of this inode is gone, so the kernel can no longer
    // write to it behind our back and the attributes can be recorded as
    // current -- from the still-open shared fd, not a reopen.
    open_for_write_.erase(id);
    RecordWrittenAttrs(id, *backing_file.fd, "Release");
    ResolveSideEffectXattrs(id, kXattrsChangedByWrite, *backing_file.fd,
                            "Release");
  }
  if (backing_file.refs > 0) return req.ReplyErrno(0);

  // Row lifetime (see SettleUnlinkedFile): the last dcfs open of a file
  // whose last link is gone takes the row with it. A row whose attributes
  // are current has nlink > 0 (backing::RecordAttrs never marks nlink 0
  // current), so only an unknown one needs a look: its attributes are
  // refreshed from the still-open fd, and the row deleted if that fresh
  // statx says nlink 0. If the refresh fails the row is kept (a later
  // access re-stats it; a leftover row for a vanished file is harmless,
  // deleting a live one is not). A row that is already gone (invalidated
  // meanwhile) has nothing left to delete.
  bool delete_row = false;
  absl::StatusOr<cache::CachedAttr> attr = cache::GetAttr(ctx_, id);
  if (attr.ok() && !attr->valid) {
    struct statx stx {};
    absl::Status refreshed =
        backing::RefreshAttrsFromFd(ctx_, id, *backing_file.fd, &stx);
    if (refreshed.ok()) {
      delete_row = stx.stx_nlink == 0;
    } else {
      LOG(WARNING) << "Release: could not refresh the attributes of inode "
                   << id << " from its open fd, keeping its row: "
                   << refreshed;
    }
  }
  if (!attr.ok() && !absl::IsNotFound(attr.status())) {
    LOG(WARNING) << "Release: could not read the cached attributes of inode "
                 << id << ", keeping its row: " << attr.status();
  }

  if (backing_file.backing_id > 0) {
    if (absl::Status closed = req.PassthroughClose(backing_file.backing_id);
        !closed.ok()) {
      LOG(WARNING) << "Release: closing passthrough backing id "
                   << backing_file.backing_id << " of inode " << id
                   << " failed: " << closed;
    }
  }
  // Erasing drops the BackingFile, whose FileDescriptor closes our own fd
  // (the kernel took its own reference to the backing file back in
  // PassthroughOpen, independent of this one) -- unless the file is gone
  // and the kernel still holds its nodeid: then the fd is kept for the
  // removed record (RetireRemoved).
  std::optional<FileDescriptor> held;
  if (delete_row) held = std::move(backing_file.fd);
  backing_files_.erase(backing_it);
  if (delete_row) {
    if (absl::Status retired = RetireRemoved(id, std::move(held));
        !retired.ok()) {
      LOG(WARNING) << "Release: could not delete the row of unlinked inode "
                   << id << ": " << retired;
    }
  }
  return req.ReplyErrno(0);
}

absl::Status DirCacheFS::Fsync(
    FuseRequest &req, fuse_ino_t ino, int datasync, fuse_file_info &fi) {
  auto it = open_files_.find(fi.fh);
  RET_CHECK(it != open_files_.end()) << "Fsync on unknown handle " << fi.fh;
  InodeId id = it->second.ino;
  auto backing_it = backing_files_.find(id);
  RET_CHECK(backing_it != backing_files_.end())
      << "Fsync on inode " << id << " with no BackingFile";
  int fd = *backing_it->second.fd;
  if (it->second.writable) {
    // As Flush(): pick up this open's writes before syncing them out. A
    // failure is only logged, so it can never skip the sync itself.
    RecordWrittenAttrs(id, fd, "Fsync");
  }
  // Backing durability is the backing filesystem's own job; passing the
  // sync through is still correct (and cheap) regardless of `writable`.
  ABSL_RETURN_IF_ERROR(backing::FsyncFd(fd, datasync != 0));
  // The caller wants what it did durable, and that includes what dcfs
  // cached about it: a sync point makes the backing filesystems durable
  // and then empties the dirty set (a no-op if it is already empty).
  SyncBackingNow("fsync");
  return req.ReplyErrno(0);
}

absl::Status DirCacheFS::Opendir(
    FuseRequest &req, fuse_ino_t ino, fuse_file_info &fi) {
  InodeId id = static_cast<InodeId>(ino);
  // A removed directory can still be opened (by a process whose working
  // directory it was); the kernel itself then answers its reads with
  // ENOENT, as for any removed directory.
  ABSL_ASSIGN_OR_RETURN(cache::CachedAttr attr, RequireAttrOrRemoved(id));
  ABSL_ASSIGN_OR_RETURN(attr, FreshAttr(id, attr));
  if (!S_ISDIR(attr.st.st_mode)) return req.ReplyErrno(ENOTDIR);
  return req.ReplyOpen(fi);
}

namespace {

// Builds the "." entry (ino == dir's own) or the ".." entry (ino == the
// parent's), shared by Readdir and Readdirplus.
struct stat DotStat(InodeId id) {
  struct stat st = {};
  st.st_ino = static_cast<ino_t>(id);
  st.st_mode = S_IFDIR;
  return st;
}

// The ListDir cursor a readdir offset resumes from: offsets 0 and 1 are
// reserved for "." and ".."; real entries start at offset 2 (cursor 0).
int64_t CursorFromOffset(off_t off) {
  return off >= 2 ? off - 2 : 0;
}

// The buffer space a readdir entry named `name` would need, per
// fuse_add_direntry(): passing a null buffer (and so bufsize 0, which is
// otherwise meaningless) makes it just return that size without writing
// anything or dereferencing `stbuf` -- see AppendDirEntries, which relies on
// the same trick to trim entries that don't fit. Used to stop asking the
// cache (ListDir/GetAttr) for more entries than a reply can possibly hold,
// rather than collecting the whole directory and letting ReplyDirs() alone
// discard what doesn't fit -- that would cost one cache read per entry of
// the directory on every single readdir call, quadratic in directory size.
size_t DirEntrySize(std::string_view name) {
  struct stat dummy = {};
  return fuse_add_direntry(
      nullptr, nullptr, 0, std::string(name).c_str(), &dummy, 0);
}

// As DirEntrySize, for a readdirplus entry via fuse_add_direntry_plus().
size_t DirEntryPlusSize(std::string_view name) {
  fuse_entry_param dummy{};
  return fuse_add_direntry_plus(
      nullptr, nullptr, 0, std::string(name).c_str(), &dummy, 0);
}

}  // namespace

absl::Status DirCacheFS::EnsureListed(InodeId dir) {
  // A readdir reply is built from the cached dentries (its offsets are
  // their rowids), so it needs the listing recorded, not just read. A
  // PopulateDirectory that could not record it (a mutation of `dir` ran
  // concurrently: see cache::CanFill) is retried; one still in flight
  // blocks every attempt.
  // TODO(coroutines): wait for the in-flight mutation instead.
  constexpr int kAttempts = 3;
  for (int attempt = 0; attempt < kAttempts; ++attempt) {
    ABSL_ASSIGN_OR_RETURN(bool complete, cache::IsDirComplete(ctx_, dir));
    if (complete) return absl::OkStatus();
    ABSL_ASSIGN_OR_RETURN(backing::Populated populated,
                          backing::PopulateDirectory(ctx_, dir));
    if (populated.cached) return absl::OkStatus();
  }
  return dcfs::ErrnoToStatus(
      EAGAIN, absl::StrCat("directory ", dir,
                           " kept changing while being listed"));
}

absl::Status DirCacheFS::Readdir(
    FuseRequest &req, fuse_ino_t ino, size_t size, off_t off,
    fuse_file_info &fi) {
  InodeId dir = static_cast<InodeId>(ino);
  ABSL_RETURN_IF_ERROR(EnsureListed(dir));

  std::vector<FuseDirEntry> entries;
  size_t used = 0;
  if (off < 1) {
    entries.push_back({.name = ".", .stbuf = DotStat(dir), .off = 1});
    used += DirEntrySize(entries.back().name);
  }
  if (off < 2) {
    ABSL_ASSIGN_OR_RETURN(InodeId parent, backing::ParentOf(ctx_, dir));
    entries.push_back({.name = "..", .stbuf = DotStat(parent), .off = 2});
    used += DirEntrySize(entries.back().name);
  }

  ABSL_RETURN_IF_ERROR(cache::ListDir(
      ctx_, dir, CursorFromOffset(off),
      [&](std::string_view name, InodeId child,
          int64_t next_cursor) -> absl::StatusOr<bool> {
        size_t entry_size = DirEntrySize(name);
        // Stop once this reply is full -- the kernel will call again with
        // the resume cursor `next_cursor` already encodes as this entry's
        // offset.
        if (used + entry_size > size) return false;
        ABSL_ASSIGN_OR_RETURN(cache::CachedAttr attr, RequireAttr(child));
        struct stat st = {};
        st.st_ino = attr.backing_ino;
        st.st_mode = attr.st.st_mode;
        entries.push_back(
            {.name = std::string(name), .stbuf = st, .off = next_cursor + 2});
        used += entry_size;
        return true;
      }));
  return req.ReplyDirs(entries, size);
}

absl::Status DirCacheFS::Readdirplus(
    FuseRequest &req, fuse_ino_t ino, size_t size, off_t off,
    fuse_file_info &fi) {
  InodeId dir = static_cast<InodeId>(ino);
  ABSL_RETURN_IF_ERROR(EnsureListed(dir));

  std::vector<FuseDirEntryPlus> entries;
  size_t used = 0;
  if (off < 1) {
    ABSL_ASSIGN_OR_RETURN(fuse_entry_param entry, EntryFor(dir));
    entries.push_back({.name = ".", .entry = entry, .off = 1});
    used += DirEntryPlusSize(entries.back().name);
  }
  if (off < 2) {
    ABSL_ASSIGN_OR_RETURN(InodeId parent, backing::ParentOf(ctx_, dir));
    ABSL_ASSIGN_OR_RETURN(fuse_entry_param entry, EntryFor(parent));
    entries.push_back({.name = "..", .entry = entry, .off = 2});
    used += DirEntryPlusSize(entries.back().name);
  }

  ABSL_RETURN_IF_ERROR(cache::ListDir(
      ctx_, dir, CursorFromOffset(off),
      [&](std::string_view name, InodeId child,
          int64_t next_cursor) -> absl::StatusOr<bool> {
        size_t entry_size = DirEntryPlusSize(name);
        // Checked (and, on failure, EntryFor -- which can itself write to
        // the cache -- skipped) before touching the cache at all, same as
        // Readdir above.
        if (used + entry_size > size) return false;
        ABSL_ASSIGN_OR_RETURN(fuse_entry_param entry, EntryFor(child));
        entries.push_back(
            {.name = std::string(name), .entry = entry,
             .off = next_cursor + 2});
        used += entry_size;
        return true;
      }));
  ABSL_RETURN_IF_ERROR(req.ReplyDirsPlus(entries, size));
  // The kernel counts a lookup for every entry in the reply with a nonzero
  // nodeid except "." and ".." (fuse_direntplus_link), whether or not it
  // fits the caller's buffer (it sends FORGET for those it cannot link).
  // Every entry collected above fits the reply (checked as it was added),
  // so all of them were sent.
  for (const FuseDirEntryPlus &e : entries) {
    if (e.name != "." && e.name != ".." && e.entry.ino != 0) {
      ++lookups_[static_cast<InodeId>(e.entry.ino)];
    }
  }
  return absl::OkStatus();
}

absl::Status DirCacheFS::Releasedir(
    FuseRequest &req, fuse_ino_t ino, fuse_file_info &fi) {
  return req.ReplyErrno(0);
}

absl::Status DirCacheFS::Fsyncdir(
    FuseRequest &req, fuse_ino_t ino, int datasync, fuse_file_info &fi) {
  InodeId id = static_cast<InodeId>(ino);
  // Missing row -> ESTALE; see RequireAttr(). There is no phase 1/3 here:
  // the syscall, then a sync point, as in Fsync().
  ABSL_RETURN_IF_ERROR(RequireAttr(id).status());
  ABSL_RETURN_IF_ERROR(backing::FsyncDir(ctx_, id, datasync != 0));
  SyncBackingNow("fsyncdir");
  return req.ReplyErrno(0);
}

absl::Status DirCacheFS::Statfs(FuseRequest &req, fuse_ino_t ino) {
  InodeId id = static_cast<InodeId>(ino);
  // backing::StatFilesystem() calls cache::GetAttr() itself (it isn't a
  // DirCacheFS method and so can't use RequireAttr()); check here first so
  // a stale nodeid comes back ESTALE rather than whatever plain NotFound
  // maps to.
  ABSL_RETURN_IF_ERROR(RequireAttrOrRemoved(id).status());
  // Every object dcfs serves is on the source filesystem (submounts are
  // refused), so the root answers for a removed one.
  if (removed_.contains(id)) id = cache::kRootInode;
  ABSL_ASSIGN_OR_RETURN(struct statvfs st, backing::StatFilesystem(ctx_, id));
  return req.ReplyStatfs(st);
}

absl::Status DirCacheFS::Setxattr(
    FuseRequest &req, fuse_ino_t ino, std::string_view name,
    std::string_view value, int flags) {
  InodeId id = static_cast<InodeId>(ino);
  // Missing row -> ESTALE; see RequireAttr().
  ABSL_RETURN_IF_ERROR(RequireAttr(id).status());
  ABSL_ASSIGN_OR_RETURN(Credentials caller, req.Caller());

  // Phase 1: mark this one xattr unknown, not the whole set (see
  // cache::ForgetXattr), and the attributes (setxattr(2) bumps ctime).
  ABSL_ASSIGN_OR_RETURN(cache::Mutation mutation,
                        cache::BeginXattrChange(ctx_, id, name));

  // Phase 2: the backing syscall, reusing this inode's shared backing fd
  // (see the BackingFile map) if one is already open. XATTR_CREATE/
  // XATTR_REPLACE in `flags`, and EPERM for "user." on a symlink or
  // special file, pass straight through from the real syscall.
  absl::StatusOr<backing::XattrReadBack> stored =
      backing::SetXattr(ctx_, caller, id, name, value, flags, OpenFdOf(id));
  if (!stored.ok()) {
    mutation.End();
    // `name` stays unknown (the syscall may or may not have changed it),
    // for the next reader to resolve; the attributes are refreshed as a
    // best effort, as Setattr does on its own phase-2 failure.
    RefreshAttrsOf(id).IgnoreError();
    return stored.status();
  }

  // Phase 3: what the backing filesystem actually stored, read back right
  // after the set -- not `value`, which it may have stored differently or
  // not at all (see backing::XattrReadBack) -- and the ctime (and, for an
  // ACL, mode) change setxattr(2) causes (phase 1 marked the attributes
  // unknown for it, above). If the read-back failed, `name` stays unknown.
  // Also left unknown if another mutation of `id` overlapped this one.
  if (!stored->ok()) {
    LOG(WARNING) << "Setxattr: could not read xattr " << name
                 << " of inode " << id << " back, leaving it unknown: "
                 << stored->status();
  } else if (!mutation.Owns(id)) {
    VLOG(1) << "Setxattr: inode " << id << " changed concurrently, leaving "
            << name << " unknown";
  } else if ((*stored)->has_value()) {
    LogPhase3Failure("Setxattr", cache::SetXattr(ctx_, id, name, ***stored));
  } else {
    LogPhase3Failure("Setxattr", cache::RemoveXattr(ctx_, id, name));
  }
  mutation.End();
  LogPhase3Failure("Setxattr", backing::RefreshAttrs(ctx_, id));
  return req.ReplyErrno(0);
}

absl::Status DirCacheFS::Getxattr(
    FuseRequest &req, fuse_ino_t ino, std::string_view name, size_t size) {
  InodeId id = static_cast<InodeId>(ino);
  // Establishes the row exists (ESTALE via RequireAttr() if not) before
  // treating a NotFound from cache::GetXattr() below as "no such xattr"
  // (ENODATA): that call's own NotFound doesn't distinguish a missing row
  // from a present row with no such xattr.
  ABSL_RETURN_IF_ERROR(RequireAttrOrRemoved(id).status());
  absl::StatusOr<std::optional<std::string>> value;
  if (auto it = removed_.find(id); it != removed_.end()) {
    // Read through the held descriptor (the kernel asks for the ACLs of a
    // removed directory, say, when checking an open of it).
    ABSL_ASSIGN_OR_RETURN(std::optional<std::string> fresh,
                          backing::ReadXattrFd(*it->second.fd, name));
    if (!fresh.has_value()) return req.ReplyErrno(ENODATA);
    value = std::move(fresh);
  } else {
    value = cache::GetXattr(ctx_, id, name);
  }
  if (!value.ok()) {
    if (absl::IsNotFound(value.status())) return req.ReplyErrno(ENODATA);
    return value.status();
  }
  if (!value->has_value()) {
    // Unknown: resolve just this name (one getxattr), and answer from what
    // was read rather than from a re-read of the cache.
    ABSL_ASSIGN_OR_RETURN(std::optional<std::string> fresh,
                          backing::RefreshXattr(ctx_, id, name, OpenFdOf(id)));
    if (!fresh.has_value()) return req.ReplyErrno(ENODATA);
    value = std::move(fresh);
  }
  if (size == 0) return req.ReplyXattrSize((*value)->size());
  if (size < (*value)->size()) return req.ReplyErrno(ERANGE);
  return req.ReplyBuf(**value);
}

absl::Status DirCacheFS::Listxattr(
    FuseRequest &req, fuse_ino_t ino, size_t size) {
  InodeId id = static_cast<InodeId>(ino);
  // See Getxattr(): establishes the row exists (ESTALE via RequireAttr()
  // if not) before relying on cache::ListXattrs()'s own NotFound, which
  // only ever means a missing row here (unlike GetXattr(), it has no
  // "not found" outcome of its own to conflate it with).
  ABSL_RETURN_IF_ERROR(RequireAttrOrRemoved(id).status());
  std::optional<std::vector<std::string>> names;
  if (auto it = removed_.find(id); it != removed_.end()) {
    ABSL_ASSIGN_OR_RETURN(
        (std::vector<std::pair<std::string, std::string>> xattrs),
        backing::ReadXattrsFd(*it->second.fd));
    names.emplace();
    for (auto &[name, value] : xattrs) names->push_back(std::move(name));
    std::sort(names->begin(), names->end());
  } else {
    ABSL_ASSIGN_OR_RETURN(names, cache::ListXattrs(ctx_, id));
  }
  if (!names.has_value()) {
    // Answered from what the refresh read, whether or not the cache could
    // record it (see cache::CanFill), in ListXattrs' (sorted) order.
    ABSL_ASSIGN_OR_RETURN(
        (std::vector<std::pair<std::string, std::string>> xattrs),
        backing::RefreshXattrs(ctx_, id));
    names.emplace();
    for (auto &[name, value] : xattrs) names->push_back(std::move(name));
    std::sort(names->begin(), names->end());
  }
  std::string buf;
  for (const std::string &name : *names) {
    buf.append(name);
    buf.push_back('\0');
  }
  if (size == 0) return req.ReplyXattrSize(buf.size());
  if (size < buf.size()) return req.ReplyErrno(ERANGE);
  return req.ReplyBuf(buf);
}

absl::Status DirCacheFS::Removexattr(
    FuseRequest &req, fuse_ino_t ino, std::string_view name) {
  InodeId id = static_cast<InodeId>(ino);
  // Missing row -> ESTALE; see RequireAttr().
  ABSL_RETURN_IF_ERROR(RequireAttr(id).status());
  ABSL_ASSIGN_OR_RETURN(Credentials caller, req.Caller());

  // Phase 1, as Setxattr.
  ABSL_ASSIGN_OR_RETURN(cache::Mutation mutation,
                        cache::BeginXattrChange(ctx_, id, name));

  // Phase 2: ENODATA (already removed, or never existed) passes straight
  // through from the real syscall.
  absl::Status remove_status =
      backing::RemoveXattr(ctx_, caller, id, name, OpenFdOf(id));
  if (!remove_status.ok()) {
    mutation.End();
    // As Setxattr: `name` stays unknown.
    RefreshAttrsOf(id).IgnoreError();
    return remove_status;
  }

  // Phase 3 (unless another mutation of `id` overlapped this one: then
  // `name` stays unknown). Failures are logged, never replied
  // (audit-races F7).
  if (mutation.Owns(id)) {
    LogPhase3Failure("Removexattr", cache::RemoveXattr(ctx_, id, name));
  }
  mutation.End();
  LogPhase3Failure("Removexattr", backing::RefreshAttrs(ctx_, id));
  return req.ReplyErrno(0);
}

absl::Status DirCacheFS::Access(FuseRequest &req, fuse_ino_t ino, int mask) {
  return req.ReplyErrno(0);
}

absl::Status DirCacheFS::Create(
    FuseRequest &req, fuse_ino_t parent_ino, std::string_view name,
    mode_t mode, fuse_file_info &fi) {
  InodeId parent = static_cast<InodeId>(parent_ino);

  // Phase 2 here also creates the new file, with the caller's exact flags
  // (so O_TRUNC/O_EXCL/the requested access mode all apply as the caller
  // asked -- this is how e.g. `echo x > newfile` and O_EXCL work). That fd
  // is used only to create/truncate the file; it is not kept -- once
  // RecordNewChild (below) has probed the result, a fresh shared backing
  // descriptor for this (brand new, so definitely not already open) inode
  // is opened via MakeBackingFile, exactly like Open()'s, so a concurrent
  // Open()/Create() of the same inode later reuses it regardless of what
  // access mode this call asked for.
  //
  // A writable create is phase 1 of the passthrough writes that follow, as
  // in Open(): RecordNewChild records the new row with its attributes
  // already marked unknown, in the same transaction, and the inode joins
  // open_for_write_ before the reply.
  bool writable = (fi.flags & O_ACCMODE) != O_RDONLY;
  ABSL_ASSIGN_OR_RETURN(Credentials caller, req.Caller());
  ABSL_ASSIGN_OR_RETURN(
      backing::NewChild child,
      CreateChild(
          parent, name,
          [&](int parent_fd) -> absl::Status {
            return backing::CreateAt(ctx_, caller, parent_fd, name, fi.flags,
                                     mode)
                .status();
          },
          writable));
  ABSL_ASSIGN_OR_RETURN(BackingFile backing_file, MakeBackingFile(child.id, req));
  backing_file.refs = 1;
  if (writable) {
    backing_file.writable_refs = 1;
    open_for_write_.insert(child.id);
  }
  int backing_id = backing_file.backing_id;
  backing_files_.emplace(child.id, std::move(backing_file));
  // After the BackingFile exists, so a writable create's still-unknown
  // attributes are served from its fd (RefreshAttrsOf), not a reopen.
  absl::StatusOr<fuse_entry_param> entry = EntryFor(child.id);
  if (!entry.ok()) {
    // The create is failing, so no Release will ever come for this open.
    open_for_write_.erase(child.id);
    if (backing_id > 0) req.PassthroughClose(backing_id).IgnoreError();
    backing_files_.erase(child.id);
    return entry.status();
  }

  if (backing_id > 0) fi.backing_id = backing_id;
  fi.keep_cache = 0;

  uint64_t handle = next_handle_++;
  open_files_.emplace(handle, OpenFile{.ino = child.id, .writable = writable});
  fi.fh = handle;

  ABSL_RETURN_IF_ERROR(req.ReplyCreate(*entry, fi));
  ++lookups_[child.id];
  return absl::OkStatus();
}

absl::Status DirCacheFS::Fallocate(
    FuseRequest &req, fuse_ino_t ino, int mode, off_t offset, off_t length,
    fuse_file_info &fi) {
  InodeId id = static_cast<InodeId>(ino);
  auto backing_it = backing_files_.find(id);
  RET_CHECK(backing_it != backing_files_.end())
      << "Fallocate on inode " << id << " with no BackingFile";
  int fd = *backing_it->second.fd;

  // Phase 1.
  ABSL_ASSIGN_OR_RETURN(
      cache::Mutation mutation,
      cache::BeginAttrChange(ctx_, id, kXattrsChangedByWrite));

  // Phase 2. The shared fd is O_RDONLY only when this inode could not be
  // opened O_RDWR (see MakeBackingFile); fallocate on it then fails EBADF,
  // exactly as the kernel would report for any write-family syscall on a
  // read-only fd -- no special-casing needed here.
  absl::Status status = backing::FallocateFd(fd, mode, offset, length);
  // Phase 3 is refreshes only, which run as ordinary fills.
  mutation.End();
  if (!status.ok()) {
    backing::RefreshAttrsFromFd(ctx_, id, fd).IgnoreError();
    return status;
  }

  // Phase 3. Failures are logged, never replied (audit-races F7).
  LogPhase3Failure("Fallocate", backing::RefreshAttrsFromFd(ctx_, id, fd));
  ResolveSideEffectXattrs(id, kXattrsChangedByWrite, fd, "Fallocate");
  return req.ReplyErrno(0);
}

bool DirCacheFS::HasOpenFiles(InodeId id) const {
  return backing_files_.contains(id);
}

std::optional<int> DirCacheFS::OpenFdOf(InodeId id) const {
  auto it = backing_files_.find(id);
  if (it == backing_files_.end()) return std::nullopt;
  return *it->second.fd;
}

}  // namespace dcfs
