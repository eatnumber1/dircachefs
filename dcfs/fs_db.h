#ifndef DCFS_FS_DB_H_
#define DCFS_FS_DB_H_

#include <optional>
#include <stdint.h>
#include <string>

#include "absl/container/flat_hash_map.h"
#include "absl/status/statusor.h"
#include "dcfs/attributes.h"
#include "dcfs/file_handle.h"
#include "dcfs/sqlite.h"

namespace dcfs {

class FileSystemDatabase {
 public:
  struct InodeID {
    int64_t rowid = 0;

    template <typename H>
    friend H AbslHashValue(H h, const InodeID& inode) {
      return H::combine(std::move(h), inode.rowid);
    }

    friend bool operator==(const InodeID &a, const InodeID &b) {
      return a.rowid == b.rowid;
    }

    template <typename Sink>
    friend void AbslStringify(Sink& sink, const InodeID &inode) {
      absl::Format(&sink, "%v", inode.rowid);
    }
  };
  struct DentryID {
    int64_t rowid = 0;

    template <typename H>
    friend H AbslHashValue(H h, const DentryID& dentry) {
      return H::combine(std::move(h), dentry.rowid);
    }

    friend bool operator==(const DentryID &a, const DentryID &b) {
      return a.rowid == b.rowid;
    }

    template <typename Sink>
    friend void AbslStringify(Sink& sink, const DentryID& dentry) {
      absl::Format(&sink, "%v", dentry.rowid);
    }
  };

  static absl::StatusOr<FileSystemDatabase> Create(
      sqlite3::Database *absl_nonnull db,
      FileHandle::Builder *absl_nonnull handle_builder,
      const FileHandle &root_handle);

  absl::StatusOr<InodeID> InsertHandle(const FileHandle &handle);
  absl::StatusOr<InodeID> InsertRootDirectory(const FileHandle &handle);

  absl::StatusOr<FileHandle> GetHandle(InodeID inode);

  absl::StatusOr<Directory> Directory::OpenDir(
      sqlite3::Connection *absl_nonnull db,
      Inode inode,
      Inode::Builder &inode_builder,
      FileHandle::Builder &handle_builder);

  // TODO rename fs_db to Inode?
  using InodeType = decltype(std::declval<linux_dirent64>().d_type);
  absl::StatusOr<InodeType> GetInodeType(InodeID inode);

 private:
  FileSystemDatabase(
      sqlite3::Database *absl_nonnull db,
      FileHandle::Builder *absl_nonnull handle_builder,
      absl::flat_hash_map<std::string, sqlite3::Statement> statements);

  absl::StatusOr<InodeID> InsertDirectoryEntry(
      const FileHandle &handle, std::string_view name,
      std::optional<DentryID> parent);

  sqlite3::Database &db_;
  DirectoryEntry::Builder &dentry_builder_;
  FileHandle::Builder &handle_builder_;
  absl::flat_hash_map<std::string, sqlite3::Statement> statements_;
};

}  // namespace dcfs

#endif  // DCFS_FS_DB_H_
