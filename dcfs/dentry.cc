namespace dcfs {
namespace {

using Builder = ::dcfs::DirectoryEntry::Builder;

struct Statements {
  constexpr static std::string_view kSelectDirectory = "select_directory";
  constexpr static std::string_view kUpsertDentry = "upsert_dentry";
  constexpr static std::string_view kGetName = "get_name";
  constexpr static std::string_view kSetChildrenFullyPopulated =
      "set_children_fully_populated";
  constexpr static std::string_view kIsChildrenFullyPopulated =
      "is_children_fully_populated";
  constexpr static std::string_view kSetExists = "set_exists";
  constexpr static std::string_view kGetExists = "get_exists";
};

}  // namespace

absl::StatusOr<Builder> Builder::Create(sqlite3::Connection *absl_nonnull db) {
  absl::flat_hash_map<std::string, sqlite3::Statement> statements;

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
      children_fully_populated BOOL NOT NULL DEFAULT FALSE,

      -- Whether or not this entry exists in the lower fs. If null, the lower fs
      -- must be checked.
      exists BOOL DEFAULT NULL,

      FOREIGN KEY(inode) REFERENCES inodes(rowid) ON DELETE SET NULL,
      FOREIGN KEY(parent) REFERENCES dentries(rowid) ON DELETE CASCADE,
      UNIQUE (name, parent)
    )
  )"));

  return Builder(db, std::move(statements));
}

absl::StatusOr<absl::flat_hash_map<std::string, sqlite3::Statement>>
Builder::CreateStatements() {
  absl::flat_hash_map<std::string, sqlite3::Statement> statements;

  // TODO move to separate file and syntax check+lint?
  ASSIGN_OR_RETURN(
      statements[Statements::kUpsertDentry],
      sqlite3::Statement::Prepare(*db, R"(
        INSERT INTO dentries
          (name, parent, inode, children_fully_populated, exists)
        VALUES
          (@name, @parent, @inode, @children_fully_populated, @exists)
        ON CONFLICT
          (name, parent)
        DO UPDATE
          inode = IF(@inode IS NOT NULL, @inode, inode),
          children_fully_populated =
            IF(@children_fully_populated, TRUE, children_fully_populated),
          exists = IF(@exists IS NOT NULL, @exists, exists)
      )"));

  // TODO move to separate file and syntax check+lint?
  ASSIGN_OR_RETURN(
      statements[Statements::kSelectDirectory],
      sqlite3::Statement::Prepare(*db, R"(
        SELECT rowid
        FROM dentries
        WHERE inode = @inode
      )"));

  // TODO move to separate file and syntax check+lint?
  ASSIGN_OR_RETURN(
      statements[Statements::kGetName],
      sqlite3::Statement::Prepare(*db, R"(
        SELECT name
        FROM dentries
        WHERE rowid = @rowid
      )"));

  // TODO move to separate file and syntax check+lint?
  ASSIGN_OR_RETURN(
      statements[Statements::kSetChildrenFullyPopulated],
      sqlite3::Statement::Prepare(*db, R"(
        UPDATE dentries
        SET children_fully_populated = @children_fully_populated
        WHERE rowid = @rowid
      )"));

  // TODO move to separate file and syntax check+lint?
  ASSIGN_OR_RETURN(
      statements[Statements::kGetChildrenFullyPopulated],
      sqlite3::Statement::Prepare(*db, R"(
        SELECT children_fully_populated
        FROM dentries
        WHERE rowid = @rowid
      )"));

  // TODO move to separate file and syntax check+lint?
  ASSIGN_OR_RETURN(
      statements[Statements::kSetExists],
      sqlite3::Statement::Prepare(*db, R"(
        UPDATE dentries
        SET exists = @exists
        WHERE rowid = @rowid
      )"));

  // TODO move to separate file and syntax check+lint?
  ASSIGN_OR_RETURN(
      statements[Statements::kGetExists],
      sqlite3::Statement::Prepare(*db, R"(
        SELECT exists
        FROM dentries
        WHERE rowid = @rowid
      )"));

  return statements;
}

absl::StatusOr<DirectoryEntry> Builder::Upsert(
    DentryID parent, std::string_view name, std::optional<bool> exists,
    std::optional<InodeID> inode, bool children_fully_populated) {
  return Upsert(
      std::make_optional(parent), name, exists, inode,
      children_fully_populated);
}

absl::StatusOr<DirectoryEntry> Builder::Upsert(
    std::optional<DentryID> parent, std::string_view name,
    std::optional<bool> exists, std::optional<InodeID> inode,
    bool children_fully_populated) {
  ASSIGN_OR_RETURN(auto s = CreateStatements());
  absl::flat_hash_map<std::string, sqlite3::Statement> statements = std::move(s);
  sqlite3::Statement &stmt = statements[Statements::kUpsertDentry];

  int64_t last_id = db_.LastInsertRowID();

  RETURN_IF_ERROR(stmt.BindBlobUnowned("@name", name));
  if (parent == std::nullopt) {
    RETURN_IF_ERROR(stmt.Bind("@parent", nullptr));
  } else {
    RETURN_IF_ERROR(stmt.Bind("@parent", parent->value()));
  }
  if (inode == std::nullopt) {
    RETURN_IF_ERROR(stmt.Bind("@inode", nullptr));
  } else {
    RETURN_IF_ERROR(stmt.Bind("@inode", inode->value()));
  }
  RETURN_IF_ERROR(
      stmt.Bind("@children_fully_populated", children_fully_populated));
  if (exists == std::nullopt) {
    RETURN_IF_ERROR(stmt.Bind("@exists", nullptr));
  } else {
    RETURN_IF_ERROR(stmt.Bind("@exists", *exists));
  }

  ASSIGN_OR_RETURN(stmt.StepThenDone());

  DirentID dentry_id { db_.LastInsertRowID() };
  // If the last_id is the same as it is now, our insert didn't work. That's
  // unexpected.
  RET_CHECK_NE(dentry_id.value(), last_id);

  return DirectoryEntry(&db_, std::move(statements), dentry_id);
}

absl::StatusOr<DirectoryEntry> Builder::UpsertRoot(
    InodeID root_inode, std::optional<bool> exists) {
  return Upsert(
      /*parent=*/std::nullopt, /*name=*/"", exists, root_inode,
      /*children_fully_populated=*/false);
}

absl::StatusOr<DirectoryEntry> Builder::Get(DirentID id) {
  ASSIGN_OR_RETURN(auto s = CreateStatements());
  absl::flat_hash_map<std::string, sqlite3::Statement> statements = std::move(s);

  return DirectoryEntry(&db_, std::move(statements), id);
}

absl::StatusOr<DirectoryEntry> Builder::GetDirectory(InodeID inode) {
  sqlite3::Statement &stmt = statements_[Statements::kSelectDirectory];
  RETURN_IF_ERROR(stmt.Bind("@inode", inode.value()));
  return stmt.StepOneCellThenDone<std::string_view>(0);
}

Builder::Builder(
    sqlite3::Connection *absl_nonnull db,
    absl::flat_hash_map<std::string, sqlite3::Statement> statements)
  : db_(*ABSL_DIE_IF_NULL(db)), statements_(std::move(statements)) {}

std::string_view DirectoryEntry::GetName() const {
  sqlite3::Statement &stmt = statements_[Statements::kGetName];
  RETURN_IF_ERROR(stmt.Bind("@rowid", id_.value()));
  return stmt.StepOneCellThenDone<std::string_view>(0);
}

absl::StatusOr<bool> DirectoryEntry::IsChildrenFullyPopulated() const {
  sqlite3::Statement &stmt = statements_[Statements::kGetChildrenFullyPopulated];
  RETURN_IF_ERROR(stmt.Bind("@rowid", id_.value()));
  return stmt.StepOneCellThenDone<bool>(0);
}

absl::Status DirectoryEntry::SetChildrenFullyPopulated(bool fully_populated) {
  sqlite3::Statement &stmt = statements_[Statements::kSetChildrenFullyPopulated];
  RETURN_IF_ERROR(stmt.Bind("@rowid", dirent_id_.value()));
  RETURN_IF_ERROR(
      stmt.Bind("@children_fully_populated", children_fully_populated));
  return stmt.StepThenDone();
}

absl::StatusOr<std::optional<bool>> DirectoryEntry::Exists() const {
  sqlite3::Statement &stmt = statements_[Statements::kGetExists];
  RETURN_IF_ERROR(stmt.Bind("@rowid", id_.value()));
  return stmt.StepOneCellThenDone<std::optional<bool>>(0);
}

absl::Status DirectoryEntry::SetExists(std::optional<bool> exists) {
  sqlite3::Statement &stmt = statements_[Statements::kSetChildrenFullyPopulated];
  RETURN_IF_ERROR(stmt.Bind("@rowid", dirent_id_.value()));
  RETURN_IF_ERROR(stmt.Bind("@exists", exists));
  return stmt.StepThenDone();
}

DentryID DirectoryEntry::GetID() const {
  return id_;
}

DirectoryEntry::DirectoryEntry(
    sqlite3::Connection *absl_nonnull db,
    absl::flat_hash_map<std::string, sqlite3::Statement> statements,
    DirentID id_)
  : db_(*ABSL_DIE_IF_NULL(db)), statements_(std::move(statements)),
    id_(id) {}

}  // namespace dcfs
