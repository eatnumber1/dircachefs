#include "dcfs/dir_cache_fs.h"

#include <limits>
#include <span>
#include <utility>
#include <memory>
#include <vector>
#include <stddef.h>
#include <fcntl.h>
#include <dirent.h>

#include "absl/cleanup/cleanup.h"
#include "absl/strings/str_cat.h"
#include "absl/log/log.h"
#include "absl/log/check.h"
#include "absl/status/status.h"
#include "dcfs/fuse.h"
#include "dcfs/fd.h"
#include "dcfs/file_handle.h"
#include "dcfs/fs_db.h"
#include "dcfs/ret_check.h"
#include "dcfs/status.h"
#include "dcfs/syscalls.h"
#include "dcfs/attributes.h"
#include "absl/log/die_if_null.h"
#include "absl/container/fixed_array.h"
#include "fuse_lowlevel.h"

namespace dcfs {
namespace {

using InodeID = ::dcfs::DirCacheFS::InodeID;

FileDescriptor *absl_nullable &GetFHPointer(fuse_file_info &fi) {
  using FileInfoHandle = decltype(fi.fh);
  // uintptr_t is defined as an unsigned integer type capable of holding a
  // pointer to void. Since we can cast class pointers (e.g. FileDescriptor) to
  // void*, this comparison should guarantee it's safe to convert from
  // pointer-to-FileInfoHandle to pointer-to-FileDescriptor.
  static_assert(
      std::numeric_limits<uintptr_t>::max() <=
      std::numeric_limits<FileInfoHandle>::max());
  return reinterpret_cast<FileDescriptor *&>(fi.fh);
}

}  // namespace

DirCacheFS::DirCacheFS(
    FileHandle::Builder *absl_nonnull handle_builder,
    FileSystemDatabase *absl_nonnull fs_db,
    InodeID root_inode,
    Options opts)
  : fs_db_(*ABSL_DIE_IF_NULL(fs_db)),
    handle_builder_(*ABSL_DIE_IF_NULL(handle_builder)),
    root_inode_(root_inode),
    opts_(std::move(opts)) {}

absl::StatusOr<InodeID> DirCacheFS::FuseToInode(fuse_ino_t ino) const {
  if (ino == FUSE_ROOT_ID) return root_inode_;

  using RowID = decltype(std::declval<InodeID>().rowid);
  // We must never use negative rowids
  RET_CHECK_GE(ino, 0);
  RET_CHECK_LE(ino, std::numeric_limits<RowID>::max());

  auto rowid = static_cast<RowID>(ino);
  return InodeID{rowid};
}

fuse_ino_t DirCacheFS::InodeToFuse(InodeID inode) const {
  using RowID = decltype(std::declval<InodeID>().rowid);
  static_assert(
      std::numeric_limits<RowID>::max() <=
      std::numeric_limits<fuse_ino_t>::max());
  return static_cast<fuse_ino_t>(inode.rowid);
}

absl::Status DirCacheFS::Init(struct fuse_conn_info &conn) {
  LOG(INFO)
    << "Fuse connection using kernel protocol version " << conn.proto_major
    << "." << conn.proto_minor;
  LOG(INFO) << "Maximum write buffer size is " << conn.max_write;
  // TODO allow setting this  when `-o max_read=<n>` is used.
  LOG(INFO) << "Maximum read buffer size is " << conn.max_read;
  LOG(INFO) << "Maximum readahead is " << conn.max_readahead;
  LOG(INFO) << "Maximum background requests is " << conn.max_background;
  LOG(INFO) << "Congestion threshold is " << conn.congestion_threshold;
  // TODO set FUSE_CAP_EXPORT_SUPPORT

  // TODO: eatnumber1 - Update libfuse to >= 3.17.1 to get passthrough support
  //RET_CHECK(fuse_set_feature_flag(conn, FUSE_CAP_PASSTHROUGH));

  return absl::OkStatus();
}

absl::Status DirCacheFS::Destroy() {
  LOG(INFO) << "Destroy()";
  return absl::OkStatus();
}

absl::Status DirCacheFS::Getattr(
    FuseRequest &req, fuse_ino_t ino, fuse_file_info *fi) {
  LOG(INFO)
    << "Getattr() ino:" << ino << ", fi:" << LogFuseFileInfo(fi);
  ASSIGN_OR_RETURN(InodeID inode, FuseToInode(ino));

  ASSIGN_OR_RETURN(FileHandle handle, fs_db_.GetHandle(inode));
  ASSIGN_OR_RETURN(
      FileDescriptor fd, handle.Open(O_PATH));

  ASSIGN_OR_RETURN(struct stat st, syscalls::fstat(*fd));
  st.st_ino = inode.rowid;
  return req.ReplyAttr(st, opts_.kernel_inode_attribute_timeout);
}

absl::Status DirCacheFS::Opendir(
    FuseRequest &req, fuse_ino_t ino, fuse_file_info &fi) {
  LOG(INFO)
    << "Opendir() ino:" << ino << ", fi:" << LogFuseFileInfo(fi);
  ASSIGN_OR_RETURN(InodeID inode, FuseToInode(ino));

  ASSIGN_OR_RETURN(Directory dir,

absl::StatusOr<Directory> Directory::OpenDir(
    sqlite3::Connection *absl_nonnull db,
    Inode inode,
    DirectoryEntry::Builder *absl_nonnull dentry_builder,
    Inode::Builder &inode_builder,
    FileHandle::Builder &handle_builder) {

  // TODO fill in fi flags such as keep_cache

  ASSIGN_OR_RETURN(FileHandle handle, fs_db_.GetHandle(inode));
  ASSIGN_OR_RETURN(
      FileDescriptor fd, handle.Open(O_DIRECTORY | O_RDONLY));

  FileDescriptor *&dirfd = GetFHPointer(fi);
  if (dirfd != nullptr) {
    return absl::InternalError(
        absl::StrCat(
          "opendir called on inode ", inode,
          " when directory is already open"));
  }
  auto owned_dirfd = std::make_unique<FileDescriptor>(std::move(fd));
  dirfd = owned_dirfd.get();

  absl::Status ret = req.ReplyOpen(fi);
  if (ret.ok()) std::move(owned_dirfd).release();
  return ret;
}

absl::Status DirCacheFS::Releasedir(
    FuseRequest &req, fuse_ino_t ino, fuse_file_info &fi) {
  LOG(INFO)
    << "Releasedir() ino:" << ino << ", fi:" << LogFuseFileInfo(fi);

  FileDescriptor *&dirfd = GetFHPointer(fi);
  if (dirfd == nullptr) {
    return absl::InternalError(
        absl::StrCat(
          "releasedir called on inode ", ino, " when directory is not open"));
  }
  std::unique_ptr<FileDescriptor> owned_dirfd(dirfd);
  dirfd = nullptr;
  return syscalls::close(std::move(*owned_dirfd));
}

absl::Status DirCacheFS::Readdir(
    FuseRequest &req, fuse_ino_t ino, size_t size, off_t off,
    fuse_file_info &fi) {
  LOG(INFO)
    << "Readdir() ino:" << ino << ", off:" << off << ", size:" << size
    << ", fi:" << LogFuseFileInfo(fi);
  // TODO change to use fuse_reply_data

  // Fetch the open directory handle from `fi`. If it's not present, this may be
  // because the filesystem was exported over NFS and the server restarted,
  // resulting in the client asking for a file handle that's not around anymore.
  // In that case, we just-in-time open the directory and use that, however in
  // that case there'll never be a Releasedir call, so we close it at the end of
  // Readdir using the `local_dirfd` variable's destruction.
  FileDescriptor local_dirfd;
  ASSIGN_OR_RETURN(
      FileDescriptor &dirfd,
      [&]() -> absl::StatusOr<std::reference_wrapper<FileDescriptor>> {
        FileDescriptor *dirfd = GetFHPointer(fi);
        if (dirfd != nullptr) return *dirfd;

        ASSIGN_OR_RETURN(InodeID inode, FuseToInode(ino));
        ASSIGN_OR_RETURN(FileHandle handle, fs_db_.GetHandle(inode));
        ASSIGN_OR_RETURN(local_dirfd, handle.Open(O_DIRECTORY | O_RDONLY));
        dirfd = &local_dirfd;

        ASSIGN_OR_RETURN(
            off_t lseek_off, syscalls::lseek(**dirfd, off, SEEK_SET));
        RET_CHECK_EQ(lseek_off, off);

        return *dirfd;
      }());

  {
    ASSIGN_OR_RETURN(off_t pos, syscalls::lseek(*dirfd, 0, SEEK_CUR));
    if (pos != off) {
      return absl::InternalError(
          absl::StrFormat(
            "Lower fs directory fd is not at the same position as the "
            "request. Request is %d, actual is %d", off, pos));
    }
  }

  using linux_dirent64 = ::dcfs::syscalls::linux_dirent64;
  absl::FixedArray<uint8_t> dentbuf(size);
  // We use getdents64 here instead of libc's readdir because with getdents64 we
  // can ensure we won't read more than the allowed buffer size in bytes (`size`
  // argument to this function).
  ASSIGN_OR_RETURN(
      size_t nb,
      syscalls::getdents64(*dirfd, dentbuf.data(), dentbuf.memsize()));
  RET_CHECK_LE(nb, dentbuf.memsize());

  std::vector<FuseDirEntry> dirs;
  for (size_t pos = 0; pos < nb;) {
    auto &dent = reinterpret_cast<linux_dirent64 &>(dentbuf[pos]);
    pos += dent.d_reclen;

    dirs.push_back({
      .name = dent.d_name,
      .stbuf = {
        .st_ino = dent.d_ino,
        .st_mode = dent.d_type,
      },
      .off = dent.d_off,
    });
  }

  return req.ReplyDirs(dirs, /*maxsize=*/size);
}

#if 0
absl::Status DirCacheFS::Readdirplus(
    FuseRequest &req, fuse_ino_t ino, size_t size, off_t off,
    fuse_file_info &fi) {
  dsadasdsad
}
#endif

absl::Status DirCacheFS::Lookup(
    FuseRequest &req, fuse_ino_t parent_ino, std::string_view name) {
  LOG(INFO)
    << "Lookup() parent_ino:" << parent_ino << ", name:" << name;
  ASSIGN_OR_RETURN(InodeID parent_inode, FuseToInode(parent_ino));
  ASSIGN_OR_RETURN(FileHandle parent_handle, fs_db_.GetHandle(parent_inode));
  ASSIGN_OR_RETURN(
      FileDescriptor dirfd, parent_handle.Open(O_DIRECTORY | O_PATH));

  absl::StatusOr<FileHandle> handle = handle_builder_.MakeHandle(*dirfd, name);

  struct stat st;
  ASSIGN_OR_RETURN(auto ino, [&]() -> absl::StatusOr<fuse_ino_t> {
    if (absl::IsNotFound(handle.status())) {
      memset(&st, 0, sizeof(struct stat));
      // ReplyEntry with an inode of zero means a negative lookup (entry does
      // not exist).
      constexpr static int kNoSuchInode = 0;
      return kNoSuchInode;
    }

    if (!handle.ok()) return std::move(handle).status();

    ASSIGN_OR_RETURN(FileDescriptor fd, handle->Open(O_PATH));
    ASSIGN_OR_RETURN(
        st, syscalls::fstatat(*fd, "", AT_EMPTY_PATH | AT_SYMLINK_NOFOLLOW));
    ASSIGN_OR_RETURN(InodeID inode, fs_db_.InsertHandle(*handle));
    return InodeToFuse(inode);
  }());

  // Inodes are never reused so generations are unnecessary
  constexpr static int kGeneration = 0;
  return req.ReplyEntry(
      ino, kGeneration, st, opts_.kernel_inode_attribute_timeout,
      opts_.kernel_directory_entry_timeout);
}

#if 0
// Checks if the cache knows that a file definitely does not exist
absl::StatusOr<bool> DirCacheFS::IsEntryNonexistent(
    fuse_ino_t parent_ino, std::string_view name) {
  sqlite3::Statement &stmt = statements_[kCheckEntryExistsStmt];
  RETURN_IF_ERROR(stmt.BindBlobUnowned("@name", name));
  RETURN_IF_ERROR(stmt.Bind("@parent_ino", parent_ino));

  ASSIGN_OR_RETURN(sqlite3::Statement::StepResult res, stmt.Step());
  absl::Cleanup reset([&]() { stmt.Reset().IgnoreError(); });
  if (res != sqlite3::Statement::StepResult::kRow) {
    return absl::InternalError(
        absl::StrCat("Expected StepResult::kRow, got ", res));
  }
  int data_count = stmt.GetDataCount();
  if (data_count == 0) return false;
  if (data_count > 1) {
    return absl::InternalError(
        absl::StrCat("Expected data count of 0 or 1, got ", data_count));
  }
  bool exists = stmt.Column<int>(0);
  ASSIGN_OR_RETURN(res, stmt.Step());
  if (res != sqlite3::Statement::StepResult::kDone) {
    return absl::InternalError(
        absl::StrCat("Expected StepResult::kDone, got ", res));
  }
  return exists;
}
#endif

}  // namespace dcfs
