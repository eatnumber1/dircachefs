namespace dcfs {
namespace {

using Builder = ::dcfs::DirectoryEntry::Builder;

}  // namespace

absl::StatusOr<Builder> Builder::Create(sqlite3::Connection *absl_nonnull db) {
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

      -- The file's type as found in stat.st_mode with the bit mask S_IFMT.
      -- See inode(7).
      type INTEGER,

      UNIQUE (handle, device_uuid)
    )
  )"));

  Builder builder(db);

  // TODO move to separate file and syntax check+lint?
  ASSIGN_OR_RETURN(
      builder.statements_.upsert,
      sqlite3::Statement::Prepare(*db, R"(
        INSERT INTO inodes
          (handle, device_uuid, type)
        VALUES
          (@handle, @device_uuid, @type)
        ON CONFLICT
          (handle, device_uuid)
        DO UPDATE
          type = IF(@type IS NOT NULL, @type, type)
      )"));

  return builder;
}

Inode::Statements Builder::CreateStatements() {
  Inode::Statements statements;

  // TODO move to separate file and syntax check+lint?
  ASSIGN_OR_RETURN(
    statements.get_type,
      sqlite3::Statement::Prepare(*db, R"(
        SELECT type
        FROM inodes
        WHERE rowid = @rowid
      )"));

  // TODO move to separate file and syntax check+lint?
  ASSIGN_OR_RETURN(
    statements.set_type,
      sqlite3::Statement::Prepare(*db, R"(
        UPDATE inodes
        SET type = @type
        WHERE rowid = @rowid
      )"));

  // TODO move to separate file and syntax check+lint?
  ASSIGN_OR_RETURN(
    statements.get_handle_and_devuuid,
      sqlite3::Statement::Prepare(*db, R"(
        SELECT handle, device_uuid
        FROM inodes
        WHERE rowid = @rowid
      )"));

  return statements;
}

absl::StatusOr<Inode> Builder::Upsert(
    const FileHandle &handle, std::optional<Type> type) {
  ASSIGN_OR_RETURN(Inode::Statements statements, CreateStatements());

  int64_t last_id = db_.LastInsertRowID();

  const file_handle &fh = handle.GetHandle();
  RETURN_IF_ERROR(
      stmt.BindBlobUnowned(
        "@handle",
        std::string_view(
          reinterpret_cast<const char *>(&fh),
          sizeof(file_handle) + fh.handle_bytes)));
  RETURN_IF_ERROR(
      stmt.BindBlobUnowned("@device_uuid", handle.GetUUID().value));

  if (type == std::nullopt) {
    RETURN_IF_ERROR(stmt.Bind("@type", nullptr));
  } else {
    RETURN_IF_ERROR(stmt.Bind("@type", type));
  }

  ASSIGN_OR_RETURN(stmt.StepThenDone());

  InodeID inode_id { db_.LastInsertRowID() };
  // If the last_id is the same as it is now, our insert didn't work. That's
  // unexpected.
  RET_CHECK_NE(inode_id.rowid, last_id);

  return Inode(inode_id, std::move(statements));
}

absl::StatusOr<Inode> Builder::Get(InodeID id) {
  ASSIGN_OR_RETURN(Inode::Statements s = CreateStatements());
  return Inode(id, std::move(statements));
}

Builder::Builder(
    sqlite3::Connection *absl_nonnull db)
  : db_(*ABSL_DIE_IF_NULL(db)) {}

absl::StatusOr<std::optional<Type>> Inode::GetType() const {
  RETURN_IF_ERROR(statements_.get_type.Bind("@rowid", id_.rowid));
  return stmt.StepOneCellThenDone<std::optional<Type>>(0);
}

absl::Status Inode::SetType(std::optional<Type> type) {
  RETURN_IF_ERROR(statements_.set_type.Bind("@rowid", id_.rowid));
  RETURN_IF_ERROR(stmt.Bind("@type", type));
  return stmt.StepThenDone();
}

absl::StatusOr<FileHandle> GetHandle(FileHandle::Builder &builder) const {
  sqlite3::Statement &stmt = statements_.get_handle_and_devuuid;
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
      builder.MakeHandle(
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

InodeID Inode::GetID() const {
  return id_;
}

Inode::Inode(InodeID id, Statements statements)
  : id_(id), statements_(std::move(statements)) {}

}  // namespace dcfs
