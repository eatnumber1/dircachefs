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
  fuse_set_feature_flag(&conn, FUSE_CAP_PASSTHROUGH);

  LOG(INFO) << "FUSE kernel protocol " << conn.proto_major << "."
            << conn.proto_minor << "; FUSE_CAP_ATTR_GENERATION "
            << (attr_generation ? "granted" : "NOT granted");
  return absl::OkStatus();
}

absl::Status DirCacheFS::Destroy() {
  return absl::OkStatus();
}

absl::StatusOr<fuse_entry_param> DirCacheFS::EntryFor(InodeId id) {
  absl::StatusOr<cache::CachedAttr> attr = cache::GetAttr(ctx_, id);
  if (!attr.ok()) {
    if (absl::IsNotFound(attr.status()) && id != cache::kRootInode) {
      // The kernel is still holding a nodeid we no longer have a row for
      // (e.g. its backing inode number was recycled and the old row
      // invalidated): that is what ESTALE means to the kernel, not ENOENT.
      return dcfs::ErrnoToStatus(
          ESTALE, absl::StrCat("no cached row for nodeid ", id));
    }
    return attr.status();
  }
  if (!attr->valid) {
    ABSL_RETURN_IF_ERROR(backing::RefreshAttrs(ctx_, id));
    ABSL_ASSIGN_OR_RETURN(attr, cache::GetAttr(ctx_, id));
  }

  fuse_entry_param entry{};
  entry.ino = static_cast<fuse_ino_t>(id);
  entry.generation = attr->fuse_gen;
  entry.attr = attr->st;
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
  ABSL_ASSIGN_OR_RETURN(cache::CachedAttr attr, cache::GetAttr(ctx_, id));
  if (!attr.valid) {
    ABSL_RETURN_IF_ERROR(backing::RefreshAttrs(ctx_, id));
    ABSL_ASSIGN_OR_RETURN(attr, cache::GetAttr(ctx_, id));
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
  return req.ReplyErrno(ENOSYS);
}

absl::Status DirCacheFS::Read(
    FuseRequest &req, fuse_ino_t ino, size_t size, off_t off,
    fuse_file_info &fi) {
  return req.ReplyErrno(ENOSYS);
}

absl::Status DirCacheFS::Write(
    FuseRequest &req, fuse_ino_t ino, std::span<const char> buf, off_t off,
    fuse_file_info &fi) {
  return req.ReplyErrno(ENOSYS);
}

absl::Status DirCacheFS::Flush(
    FuseRequest &req, fuse_ino_t ino, fuse_file_info &fi) {
  return req.ReplyErrno(ENOSYS);
}

absl::Status DirCacheFS::Release(
    FuseRequest &req, fuse_ino_t ino, fuse_file_info &fi) {
  return req.ReplyErrno(ENOSYS);
}

absl::Status DirCacheFS::Fsync(
    FuseRequest &req, fuse_ino_t ino, int datasync, fuse_file_info &fi) {
  return req.ReplyErrno(ENOSYS);
}

absl::Status DirCacheFS::Opendir(
    FuseRequest &req, fuse_ino_t ino, fuse_file_info &fi) {
  InodeId id = static_cast<InodeId>(ino);
  ABSL_ASSIGN_OR_RETURN(cache::CachedAttr attr, cache::GetAttr(ctx_, id));
  if (!attr.valid) {
    ABSL_RETURN_IF_ERROR(backing::RefreshAttrs(ctx_, id));
    ABSL_ASSIGN_OR_RETURN(attr, cache::GetAttr(ctx_, id));
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
  if (off < 1) entries.push_back({.name = ".", .stbuf = DotStat(dir), .off = 1});
  if (off < 2) {
    ABSL_ASSIGN_OR_RETURN(InodeId parent, cache::ParentOf(ctx_, dir));
    entries.push_back({.name = "..", .stbuf = DotStat(parent), .off = 2});
  }

  ABSL_RETURN_IF_ERROR(cache::ListDir(
      ctx_, dir, CursorFromOffset(off),
      [&](std::string_view name, InodeId child,
          int64_t next_cursor) -> absl::StatusOr<bool> {
        ABSL_ASSIGN_OR_RETURN(cache::CachedAttr attr, cache::GetAttr(ctx_, child));
        struct stat st = {};
        st.st_ino = attr.backing_ino;
        st.st_mode = attr.st.st_mode;
        entries.push_back(
            {.name = std::string(name), .stbuf = st, .off = next_cursor + 2});
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
  if (off < 1) {
    ABSL_ASSIGN_OR_RETURN(fuse_entry_param entry, EntryFor(dir));
    entries.push_back({.name = ".", .entry = entry, .off = 1});
  }
  if (off < 2) {
    ABSL_ASSIGN_OR_RETURN(InodeId parent, cache::ParentOf(ctx_, dir));
    ABSL_ASSIGN_OR_RETURN(fuse_entry_param entry, EntryFor(parent));
    entries.push_back({.name = "..", .entry = entry, .off = 2});
  }

  // The kernel bumps the lookup count for every entry here with ino != 0;
  // that is fine because Forget is a no-op (rows persist regardless).
  ABSL_RETURN_IF_ERROR(cache::ListDir(
      ctx_, dir, CursorFromOffset(off),
      [&](std::string_view name, InodeId child,
          int64_t next_cursor) -> absl::StatusOr<bool> {
        ABSL_ASSIGN_OR_RETURN(fuse_entry_param entry, EntryFor(child));
        entries.push_back(
            {.name = std::string(name), .entry = entry,
             .off = next_cursor + 2});
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
  ABSL_ASSIGN_OR_RETURN(
      struct statvfs st, backing::StatFilesystem(ctx_, static_cast<InodeId>(ino)));
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

}  // namespace dcfs
