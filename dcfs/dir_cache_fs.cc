#include "dcfs/dir_cache_fs.h"

#include <cerrno>
#include <cstdint>
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
#include "dcfs/fuse_request.h"
#include "dcfs/metadata_cache.h"
#include "dcfs/ret_check.h"
#include "dcfs/status.h"
#include "fuse_lowlevel.h"

namespace dcfs {

DirCacheFS::DirCacheFS(Context &ctx, Options opts)
    : ctx_(ctx), opts_(opts) {}

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

  LOG(INFO) << "FUSE kernel protocol " << conn.proto_major << "."
            << conn.proto_minor << "; FUSE_CAP_ATTR_GENERATION "
            << (attr_generation ? "granted" : "NOT granted")
            << "; FUSE_CAP_PASSTHROUGH " << (passthrough ? "granted" : "NOT granted");
  return absl::OkStatus();
}

absl::Status DirCacheFS::Destroy() {
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

absl::StatusOr<fuse_entry_param> DirCacheFS::EntryFor(InodeId id) {
  ABSL_ASSIGN_OR_RETURN(cache::CachedAttr attr, RequireAttr(id));
  if (!attr.valid) {
    ABSL_RETURN_IF_ERROR(backing::RefreshAttrs(ctx_, id));
    ABSL_ASSIGN_OR_RETURN(attr, RequireAttr(id));
  }

  fuse_entry_param entry{};
  entry.ino = static_cast<fuse_ino_t>(id);
  entry.generation = attr.fuse_gen;
  entry.attr = attr.st;
  entry.attr_timeout = absl::ToDoubleSeconds(opts_.attr_timeout);
  entry.entry_timeout = absl::ToDoubleSeconds(opts_.entry_timeout);
  return entry;
}

absl::Status DirCacheFS::Getattr(
    FuseRequest &req, fuse_ino_t ino, fuse_file_info *fi) {
  ABSL_ASSIGN_OR_RETURN(fuse_entry_param entry, EntryFor(static_cast<InodeId>(ino)));
  return req.ReplyAttr(entry.attr, opts_.attr_timeout, entry.generation);
}

absl::Status DirCacheFS::Setattr(
    FuseRequest &req, fuse_ino_t ino, struct stat *attr, int to_set,
    fuse_file_info *fi) {
  return req.ReplyErrno(ENOSYS);
}

absl::Status DirCacheFS::Lookup(
    FuseRequest &req, fuse_ino_t parent_ino, std::string_view name) {
  InodeId parent = static_cast<InodeId>(parent_ino);

  if (name == ".") {
    ABSL_ASSIGN_OR_RETURN(fuse_entry_param entry, EntryFor(parent));
    return req.ReplyEntry(
        entry.ino, entry.generation, entry.attr, opts_.attr_timeout,
        opts_.entry_timeout);
  }
  if (name == "..") {
    ABSL_ASSIGN_OR_RETURN(InodeId up, cache::ParentOf(ctx_, parent));
    ABSL_ASSIGN_OR_RETURN(fuse_entry_param entry, EntryFor(up));
    return req.ReplyEntry(
        entry.ino, entry.generation, entry.attr, opts_.attr_timeout,
        opts_.entry_timeout);
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
  return req.ReplyEntry(
      entry.ino, entry.generation, entry.attr, opts_.attr_timeout,
      opts_.entry_timeout);
}

void DirCacheFS::Forget(FuseRequest &req, fuse_ino_t ino, uint64_t nlookup) {
  req.ReplyNone();
}

void DirCacheFS::ForgetMulti(
    FuseRequest &req, std::span<const fuse_forget_data> forgets) {
  // Unlike the other not-yet-mutating ops, forget/forget_multi have no error
  // reply -- fuse_reply_none is the only valid reply -- so this cannot be a
  // req.ReplyErrno(ENOSYS) stub even though it does the same nothing.
  req.ReplyNone();
}

absl::Status DirCacheFS::Readlink(FuseRequest &req, fuse_ino_t ino) {
  InodeId id = static_cast<InodeId>(ino);
  absl::StatusOr<std::string> target = cache::Readlink(ctx_, id);
  if (target.ok()) return req.ReplyReadlink(*target);
  if (!absl::IsNotFound(target.status())) return target.status();

  // Not cached yet. Confirm this really is a symlink (rather than, say, a
  // caller racing a stale nodeid) before reading the backing filesystem.
  ABSL_ASSIGN_OR_RETURN(cache::CachedAttr attr, RequireAttr(id));
  if (!attr.valid) {
    ABSL_RETURN_IF_ERROR(backing::RefreshAttrs(ctx_, id));
    ABSL_ASSIGN_OR_RETURN(attr, RequireAttr(id));
  }
  RET_CHECK(S_ISLNK(attr.st.st_mode))
      << "Readlink on non-symlink inode " << id;

  ABSL_ASSIGN_OR_RETURN(std::string real_target, backing::ReadSymlink(ctx_, id));
  ABSL_RETURN_IF_ERROR(cache::SetSymlink(ctx_, id, real_target));
  return req.ReplyReadlink(real_target);
}

absl::Status DirCacheFS::Mknod(
    FuseRequest &req, fuse_ino_t parent, std::string_view name, mode_t mode,
    dev_t rdev) {
  return req.ReplyErrno(ENOSYS);
}

absl::Status DirCacheFS::Mkdir(
    FuseRequest &req, fuse_ino_t parent, std::string_view name,
    mode_t mode) {
  return req.ReplyErrno(ENOSYS);
}

absl::Status DirCacheFS::Unlink(
    FuseRequest &req, fuse_ino_t parent, std::string_view name) {
  return req.ReplyErrno(ENOSYS);
}

absl::Status DirCacheFS::Rmdir(
    FuseRequest &req, fuse_ino_t parent, std::string_view name) {
  return req.ReplyErrno(ENOSYS);
}

absl::Status DirCacheFS::Symlink(
    FuseRequest &req, std::string_view link, fuse_ino_t parent,
    std::string_view name) {
  return req.ReplyErrno(ENOSYS);
}

absl::Status DirCacheFS::Rename(
    FuseRequest &req, fuse_ino_t parent, std::string_view name,
    fuse_ino_t newparent, std::string_view newname, unsigned int flags) {
  return req.ReplyErrno(ENOSYS);
}

absl::Status DirCacheFS::Link(
    FuseRequest &req, fuse_ino_t ino, fuse_ino_t newparent,
    std::string_view newname) {
  return req.ReplyErrno(ENOSYS);
}

absl::Status DirCacheFS::Open(
    FuseRequest &req, fuse_ino_t ino, fuse_file_info &fi) {
  InodeId id = static_cast<InodeId>(ino);

  // Phase 4 owns writes; only a read-only, non-truncating open is served
  // for now.
  if ((fi.flags & O_ACCMODE) != O_RDONLY || (fi.flags & O_TRUNC)) {
    return req.ReplyErrno(EROFS);
  }

  // RequireAttr(), not cache::GetAttr(): the kernel's generic open path
  // (do_file_open_root, fs/namei.c) automatically retries a failed open
  // once with LOOKUP_REVAL after -ESTALE, and that second FUSE OPEN hits
  // this same call against the now-invalidated row -- which must come
  // back ESTALE again, not ENOENT.
  ABSL_ASSIGN_OR_RETURN(cache::CachedAttr attr, RequireAttr(id));
  if (!attr.valid) {
    ABSL_RETURN_IF_ERROR(backing::RefreshAttrs(ctx_, id));
    ABSL_ASSIGN_OR_RETURN(attr, RequireAttr(id));
  }
  // The kernel calls opendir(), not open(), on a directory, so this would
  // only trip on a row that changed type out from under a stale nodeid.
  RET_CHECK(!S_ISDIR(attr.st.st_mode)) << "Open on directory inode " << id;

  // A real read fd, stripped of the create/truncate flags Open() never
  // needs (this is reopening an existing node, not creating one).
  ABSL_ASSIGN_OR_RETURN(
      FileDescriptor fd,
      backing::OpenNode(
          ctx_, id,
          (fi.flags & ~(O_CREAT | O_EXCL | O_NOCTTY | O_TRUNC)) | O_CLOEXEC));

  // Ask the kernel to serve reads directly against `fd`. A 0 backing_id
  // means either the kernel never granted FUSE_CAP_PASSTHROUGH (logged
  // once at Init) or this particular open failed for some other reason;
  // either way it is not a dcfs-level error -- Read() below falls back to
  // serving the data itself.
  ABSL_ASSIGN_OR_RETURN(int backing_id, req.PassthroughOpen(*fd));
  if (backing_id > 0) fi.backing_id = backing_id;

  // A passthrough open must drop any stale page cache for this file
  // (fi.keep_cache = 0 is also correct, if redundant, on the fallback
  // path: nothing has cached this file's contents on a fresh open). Leave
  // fi.direct_io at its default 0 -- passthrough requires the default
  // cache mode, and the fallback path benefits from the normal page cache
  // like any other read-only file.
  fi.keep_cache = 0;

  uint64_t handle = next_handle_++;
  open_files_.emplace(
      handle, OpenFile{.ino = id, .fd = std::move(fd), .backing_id = backing_id});
  fi.fh = handle;

  return req.ReplyOpen(fi);
}

absl::Status DirCacheFS::Read(
    FuseRequest &req, fuse_ino_t ino, size_t size, off_t off,
    fuse_file_info &fi) {
  // The fallback path: only reached when Open() did not get passthrough
  // for this handle (or, per fuse_passthrough_open()'s contract, in the
  // unlikely case the kernel sends READ anyway despite passthrough).
  auto it = open_files_.find(fi.fh);
  RET_CHECK(it != open_files_.end()) << "Read on unknown handle " << fi.fh;
  ABSL_ASSIGN_OR_RETURN(
      std::string buf, backing::ReadFile(*it->second.fd, size, off));
  return req.ReplyBuf(buf);
}

absl::Status DirCacheFS::Write(
    FuseRequest &req, fuse_ino_t ino, std::span<const char> buf, off_t off,
    fuse_file_info &fi) {
  return req.ReplyErrno(ENOSYS);
}

absl::Status DirCacheFS::Flush(
    FuseRequest &req, fuse_ino_t ino, fuse_file_info &fi) {
  // Nothing is buffered on a read-only open; writes (and so anything a
  // flush would need to push out) are Phase 4.
  return req.ReplyErrno(0);
}

absl::Status DirCacheFS::Release(
    FuseRequest &req, fuse_ino_t ino, fuse_file_info &fi) {
  auto it = open_files_.find(fi.fh);
  RET_CHECK(it != open_files_.end()) << "Release on unknown handle " << fi.fh;
  if (it->second.backing_id > 0) {
    ABSL_RETURN_IF_ERROR(req.PassthroughClose(it->second.backing_id));
  }
  // Erasing drops the OpenFile, whose FileDescriptor closes our own fd
  // (the kernel took its own reference to the backing file back in
  // PassthroughOpen, independent of this one).
  open_files_.erase(it);
  return req.ReplyErrno(0);
}

absl::Status DirCacheFS::Fsync(
    FuseRequest &req, fuse_ino_t ino, int datasync, fuse_file_info &fi) {
  // Only read-only opens exist until Phase 4; nothing to sync.
  return req.ReplyErrno(0);
}

absl::Status DirCacheFS::Opendir(
    FuseRequest &req, fuse_ino_t ino, fuse_file_info &fi) {
  InodeId id = static_cast<InodeId>(ino);
  ABSL_ASSIGN_OR_RETURN(cache::CachedAttr attr, RequireAttr(id));
  if (!attr.valid) {
    ABSL_RETURN_IF_ERROR(backing::RefreshAttrs(ctx_, id));
    ABSL_ASSIGN_OR_RETURN(attr, RequireAttr(id));
  }
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

absl::Status DirCacheFS::Readdir(
    FuseRequest &req, fuse_ino_t ino, size_t size, off_t off,
    fuse_file_info &fi) {
  InodeId dir = static_cast<InodeId>(ino);
  ABSL_ASSIGN_OR_RETURN(bool complete, cache::IsDirComplete(ctx_, dir));
  if (!complete) {
    ABSL_RETURN_IF_ERROR(backing::PopulateDirectory(ctx_, dir));
  }

  std::vector<FuseDirEntry> entries;
  size_t used = 0;
  if (off < 1) {
    entries.push_back({.name = ".", .stbuf = DotStat(dir), .off = 1});
    used += DirEntrySize(entries.back().name);
  }
  if (off < 2) {
    ABSL_ASSIGN_OR_RETURN(InodeId parent, cache::ParentOf(ctx_, dir));
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
  ABSL_ASSIGN_OR_RETURN(bool complete, cache::IsDirComplete(ctx_, dir));
  if (!complete) {
    ABSL_RETURN_IF_ERROR(backing::PopulateDirectory(ctx_, dir));
  }

  std::vector<FuseDirEntryPlus> entries;
  size_t used = 0;
  if (off < 1) {
    ABSL_ASSIGN_OR_RETURN(fuse_entry_param entry, EntryFor(dir));
    entries.push_back({.name = ".", .entry = entry, .off = 1});
    used += DirEntryPlusSize(entries.back().name);
  }
  if (off < 2) {
    ABSL_ASSIGN_OR_RETURN(InodeId parent, cache::ParentOf(ctx_, dir));
    ABSL_ASSIGN_OR_RETURN(fuse_entry_param entry, EntryFor(parent));
    entries.push_back({.name = "..", .entry = entry, .off = 2});
    used += DirEntryPlusSize(entries.back().name);
  }

  // The kernel bumps the lookup count for every entry here with ino != 0;
  // that is fine because Forget is a no-op (rows persist regardless).
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
  return req.ReplyDirsPlus(entries, size);
}

absl::Status DirCacheFS::Releasedir(
    FuseRequest &req, fuse_ino_t ino, fuse_file_info &fi) {
  return req.ReplyErrno(0);
}

absl::Status DirCacheFS::Fsyncdir(
    FuseRequest &req, fuse_ino_t ino, int datasync, fuse_file_info &fi) {
  return req.ReplyErrno(ENOSYS);
}

absl::Status DirCacheFS::Statfs(FuseRequest &req, fuse_ino_t ino) {
  InodeId id = static_cast<InodeId>(ino);
  // backing::StatFilesystem() calls cache::GetAttr() itself (it isn't a
  // DirCacheFS method and so can't use RequireAttr()); check here first so
  // a stale nodeid comes back ESTALE rather than whatever plain NotFound
  // maps to.
  ABSL_RETURN_IF_ERROR(RequireAttr(id).status());
  ABSL_ASSIGN_OR_RETURN(struct statvfs st, backing::StatFilesystem(ctx_, id));
  return req.ReplyStatfs(st);
}

absl::Status DirCacheFS::Setxattr(
    FuseRequest &req, fuse_ino_t ino, std::string_view name,
    std::string_view value, int flags) {
  return req.ReplyErrno(ENOSYS);
}

absl::Status DirCacheFS::Getxattr(
    FuseRequest &req, fuse_ino_t ino, std::string_view name, size_t size) {
  InodeId id = static_cast<InodeId>(ino);
  // Establishes the row exists (ESTALE via RequireAttr() if not) before
  // treating a NotFound from cache::GetXattr() below as "no such xattr"
  // (ENODATA): that call's own NotFound doesn't distinguish a missing row
  // from a present row with no such xattr.
  ABSL_RETURN_IF_ERROR(RequireAttr(id).status());
  absl::StatusOr<std::optional<std::string>> value =
      cache::GetXattr(ctx_, id, name);
  if (!value.ok()) {
    if (absl::IsNotFound(value.status())) return req.ReplyErrno(ENODATA);
    return value.status();
  }
  if (!value->has_value()) {
    ABSL_RETURN_IF_ERROR(backing::RefreshXattrs(ctx_, id));
    value = cache::GetXattr(ctx_, id, name);
    if (!value.ok()) {
      if (absl::IsNotFound(value.status())) return req.ReplyErrno(ENODATA);
      return value.status();
    }
    RET_CHECK(value->has_value())
        << "xattr " << name << " on inode " << id
        << " still unknown after a complete refresh";
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
  ABSL_RETURN_IF_ERROR(RequireAttr(id).status());
  ABSL_ASSIGN_OR_RETURN(
      std::optional<std::vector<std::string>> names,
      cache::ListXattrs(ctx_, id));
  if (!names.has_value()) {
    ABSL_RETURN_IF_ERROR(backing::RefreshXattrs(ctx_, id));
    ABSL_ASSIGN_OR_RETURN(names, cache::ListXattrs(ctx_, id));
    RET_CHECK(names.has_value())
        << "xattr set of inode " << id << " still unknown after a complete "
        << "refresh";
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
  return req.ReplyErrno(ENOSYS);
}

absl::Status DirCacheFS::Access(FuseRequest &req, fuse_ino_t ino, int mask) {
  return req.ReplyErrno(0);
}

absl::Status DirCacheFS::Create(
    FuseRequest &req, fuse_ino_t parent, std::string_view name, mode_t mode,
    fuse_file_info &fi) {
  return req.ReplyErrno(ENOSYS);
}

absl::Status DirCacheFS::Fallocate(
    FuseRequest &req, fuse_ino_t ino, int mode, off_t offset, off_t length,
    fuse_file_info &fi) {
  return req.ReplyErrno(ENOSYS);
}

bool DirCacheFS::HasOpenFiles(InodeId id) const {
  // A linear scan is fine while nothing calls this; 4.3 should index
  // open_files_ by inode if it becomes a hot path.
  for (const auto &[handle, file] : open_files_) {
    if (file.ino == id) return true;
  }
  return false;
}

}  // namespace dcfs
