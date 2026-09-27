#include "dcfs/fs_db.h"

#include <optional>
#include <stdint.h>
#include <string>
#include <string_view>

#include "absl/cleanup/cleanup.h"
#include "absl/container/flat_hash_map.h"
#include "absl/log/die_if_null.h"
#include "absl/status/statusor.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/str_format.h"
#include "dcfs/attributes.h"
#include "dcfs/file_handle.h"
#include "dcfs/ret_check.h"
#include "dcfs/sqlite.h"
#include "dcfs/status.h"

namespace dcfs {
namespace {

using InodeID = ::dcfs::FileSystemDatabase::InodeID;
using DentryID = ::dcfs::FileSystemDatabase::DentryID;

struct Statements {
  constexpr static std::string_view kInsertInode = "insert_inode";
  constexpr static std::string_view kInsertDentry = "insert_dentry";
  constexpr static std::string_view kSelectInode = "select_inode";
};

}  // namespace

absl::StatusOr<FileSystemDatabase> FileSystemDatabase::Create(
    sqlite3::Database *db,
    FileHandle::Builder *handle_builder,
    const FileHandle &root_handle) {
  absl::flat_hash_map<std::string, sqlite3::Statement> statements;

#if 0
  // TODO move to separate file and syntax check+lint?
  RETURN_IF_ERROR(db.Exec(R"(
    CREATE TABLE IF NOT EXISTS entries (
      inode INTEGER NOT NULL PRIMARY KEY,
      name BLOB NOT NULL,
      parent_ino INTEGER,
      FOREIGN KEY(parent_ino) REFERENCES entries(inode) ON DELETE CASCADE,
      UNIQUE (inode, parent_ino)
    )
  )"));

  // TODO move to separate file and syntax check+lint?
  RETURN_IF_ERROR(db.Exec(R"(
    CREATE INDEX IF NOT EXISTS entries_parents_index ON entries (
      parent_ino
    )
  )"));

  RETURN_IF_ERROR(db.Exec(R"(
    CREATE TABLE IF NOT EXISTS negative_entries (
      name BLOB NOT NULL,
      parent_ino INTEGER NOT NULL,
      FOREIGN KEY(parent_ino) REFERENCES entries(inode) ON DELETE CASCADE,
      PRIMARY KEY (name, parent_ino)
    )
  )"));

  // TODO move to separate file and syntax check+lint?
  RETURN_IF_ERROR(db.Exec(R"(
    CREATE INDEX IF NOT EXISTS negative_entries_parents_index ON entries (
      name
    )
  )"));

  // TODO move to separate file and syntax check+lint?
  ASSIGN_OR_RETURN(
      statements[kCheckEntryExistsStmt],
      sqlite3::Statement::Prepare(db, R"(
        SELECT
          TRUE AS exist
        FROM entries
        WHERE TRUE
          AND parent_ino = @parent_ino
          AND name = @name
        UNION
        SELECT
          FALSE AS exist
        FROM negative_entries
        WHERE TRUE
          AND parent_ino = @parent_ino
          AND name = @name
      )"));
#endif

  // Since Fuse doesn't pass generation numbers from kernel to userspace, we
  // must use fully unique, never-reused inode numbers. For that reason, the
  // inode number we'll pass to callers is the auto-incrementing row id from
  // this table.
  //
  // This also means it's impossible to write a fully functional pass-through
  // Fuse filesystem that preserves inode numbers.
  //
  // generation - https://github.com/torvalds/linux/blob/0af2f6be1b4281385b618cb86ad946eded089ac8/fs/fuse/inode.c#L1052
  // device - https://github.com/torvalds/linux/blob/a24588245776dafc227243a01bfbeb8a59bafba9/include/linux/types.h#L21
  // inum - https://github.com/torvalds/linux/blob/a24588245776dafc227243a01bfbeb8a59bafba9/include/linux/types.h#L22
  RETURN_IF_ERROR(db->Exec(R"(
    CREATE TABLE IF NOT EXISTS inodes (
      rowid INTEGER NOT NULL PRIMARY KEY AUTOINCREMENT,
      handle BLOB NOT NULL,
      device_uuid BLOB NOT NULL,

      -- The file's type as returned in st_mode by stat(2) with the bit mask
      -- S_IFMT.
      type INTEGER NOT NULL,

      UNIQUE (handle, device_uuid)
    )
  )"));
  // TODO move to separate file and syntax check+lint?
  ASSIGN_OR_RETURN(
      statements[Statements::kInsertInode],
      sqlite3::Statement::Prepare(*db, R"(
        INSERT OR IGNORE INTO inodes (handle, device_uuid)
        VALUES (@handle, @device_uuid)
      )"));
  // TODO move to separate file and syntax check+lint?
  ASSIGN_OR_RETURN(
      statements[Statements::kSelectInode],
      sqlite3::Statement::Prepare(*db, R"(
        SELECT handle, device_uuid
        FROM inodes
        WHERE rowid = @rowid
      )"));

  // A null inode is a negative dentry
  // An empty string in the name column means this dentry is the root
  // A null parent means this dentry is the root
  //
  // https://github.com/torvalds/linux/blob/a24588245776dafc227243a01bfbeb8a59bafba9/include/linux/dcache.h#L91
  RETURN_IF_ERROR(db->Exec(R"(
    CREATE TABLE IF NOT EXISTS dentries (
      rowid INTEGER NOT NULL PRIMARY KEY,
      name BLOB NOT NULL,

      -- A null parent means this dentry is the root of the filesystem.
      parent INTEGER,

      -- A null inode means the inode hasn't been looked up in the lower fs.
      inode INTEGER,

      -- If true, a lookup for a dentry that's a child of this dentry that does
      -- not exist should return ENOENT without checking the lower fs.
      is_children_fully_populated BOOL NOT NULL DEFAULT FALSE,

      -- Whether or not this entry exists in the lower fs. If null, the lower fs
      -- must be checked.
      exists BOOL DEFAULT NULL,

      FOREIGN KEY(inode) REFERENCES inodes(rowid) ON DELETE SET NULL,
      FOREIGN KEY(parent) REFERENCES dentries(rowid) ON DELETE CASCADE,
      UNIQUE (name, parent)
    )
  )"));
  // TODO move to separate file and syntax check+lint?
  ASSIGN_OR_RETURN(
      statements[Statements::kInsertDentry],
      sqlite3::Statement::Prepare(*db, R"(
        INSERT OR REPLACE INTO dentries (inode, name, parent)
        VALUES (@inode, @name, @parent)
      )"));

  return FileSystemDatabase(db, handle_builder, std::move(statements));
}

absl::StatusOr<InodeID> FileSystemDatabase::InsertHandle(
    const FileHandle &handle) {
  sqlite3::Statement &stmt = statements_[Statements::kInsertInode];
  const file_handle &fh = handle.GetHandle();
  RETURN_IF_ERROR(
      stmt.BindBlobUnowned(
        "@handle",
        std::string_view(
          reinterpret_cast<const char *>(&fh),
          sizeof(file_handle) + fh.handle_bytes)));
  RETURN_IF_ERROR(
      stmt.BindBlobUnowned("@device_uuid", handle.GetUUID().value));

  ASSIGN_OR_RETURN(sqlite3::Statement::StepResult res, stmt.Step());
  WithStatementReset reset(&stmt);
  RET_CHECK_EQ(res, sqlite3::Statement::StepResult::kDone);
  InodeID inode { db_.LastInsertRowID() };

  RETURN_IF_ERROR(std::move(reset).Reset());
  return inode;
}

absl::StatusOr<InodeID> FileSystemDatabase::InsertRootDirectory(
    const FileHandle &handle) {
  return InsertDirectoryEntry(
        handle, /*name=*/"", /*parent=*/std::nullopt);
}

absl::StatusOr<InodeID> FileSystemDatabase::InsertDirectoryEntry(
    const FileHandle &handle, std::string_view name,
    std::optional<DentryID> parent) {
  InodeID inode(0);

  RETURN_IF_ERROR(db_.Transaction([&]() {
    ASSIGN_OR_RETURN(inode, InsertHandle(handle));

    RETURN_IF_ERROR([&]() {
      sqlite3::Statement &stmt = statements_[Statements::kInsertDentry];
      RETURN_IF_ERROR(stmt.Bind("@inode", inode.rowid));
      RETURN_IF_ERROR(stmt.BindBlobUnowned("@name", name));
      if (parent != std::nullopt) {
        RETURN_IF_ERROR(stmt.Bind("@parent", parent->rowid));
      }

      return stmt.StepThenDone();
    }());

    return absl::OkStatus();
  }));

  RET_CHECK_GT(inode.rowid, 0);
  return inode;
}

absl::StatusOr<FileHandle> FileSystemDatabase::GetHandle(InodeID inode) {
  sqlite3::Statement &stmt = statements_[Statements::kSelectInode];
  RETURN_IF_ERROR(stmt.Bind("@rowid", inode.rowid));

  ASSIGN_OR_RETURN(sqlite3::Statement::StepResult res, stmt.Step());
  WithStatementReset reset(&stmt);

  if (res != sqlite3::Statement::StepResult::kRow) {
    return absl::InternalError(
        absl::StrCat("Expected StepResult::kRow, got ", res));
  }
  if (int column_count = stmt.GetDataCount(); column_count != 2) {
    return absl::InternalError(
        absl::StrFormat(
          "Expected column count of 2 for id %v, got %d", inode, column_count));
  }

  ASSIGN_OR_RETURN(
      auto device_uuid, stmt.Column<std::string_view>("device_uuid"));
  ASSIGN_OR_RETURN(auto handle_bytes, stmt.Column<std::string_view>("handle"));

  ASSIGN_OR_RETURN(
      FileHandle handle,
      handle_builder_.MakeHandle(
        CopyFileHandle(
          *reinterpret_cast<const file_handle *>(handle_bytes.data())),
        DeviceUUID(std::string(device_uuid))));

  ASSIGN_OR_RETURN(res, stmt.Step());
  if (res != sqlite3::Statement::StepResult::kDone) {
    return absl::InternalError(
        absl::StrCat("Expected StepResult::kDone, got ", res));
  }

  return handle;
}

FileSystemDatabase::FileSystemDatabase(
    sqlite3::Database *db,
    FileHandle::Builder *handle_builder,
    absl::flat_hash_map<std::string, sqlite3::Statement> statements)
  : db_(*ABSL_DIE_IF_NULL(db)),
    handle_builder_(*ABSL_DIE_IF_NULL(handle_builder)),
    statements_(std::move(statements)) {}

}  // namespace dcfs
