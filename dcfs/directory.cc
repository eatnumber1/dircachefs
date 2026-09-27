namespace dcfs {

using Iterator = ::dcfs::Directory::Iterator;

Directory::Directory(
    sqlite3::Connection *absl_nonnull db,
    DirectoryEntry dentry,
    DirectoryEntry::Builder *absl_nonnull dentry_builder)
  : db_(*ABSL_DIE_IF_NULL(db)),
    dentry_(std::move(dentry)),
    dentry_builder_(*ABSL_DIE_IF_NULL(dentry_builder))
{}

absl::StatusOr<Directory> Directory::OpenDir(
    sqlite3::Connection *absl_nonnull db,
    Inode inode,
    DirectoryEntry::Builder *absl_nonnull dentry_builder,
    Inode::Builder &inode_builder,
    FileHandle::Builder &handle_builder) {
  // Start a savepoint before querying the directory so if it's not fully
  // populated, and we open it successfully, we're guaranteed the DirectoryEntry
  // does not get deleted before we populate it.
  ASSIGN_OR_RETURN(auto ws, sqlite3::WithSavepoint::Create(db));

  ASSIGN_OR_RETURN(DirectoryEntry dentry, dentry_builder.GetDirectory(inode));

  ASSIGN_OR_RETURN(bool children_fully_populated, dentry.IsChildrenFullyPopulated());
  if (!children_fully_populated) {
    RETURN_IF_ERROR(PopulateChildren(dentry, inode, *dentry_builder, inode_builder, handle_builder));
  }

  RETURN_IF_ERROR(std::move(ws).Commit());
  return Directory(db_, std::move(dentry), dentry_builder);
}

absl::Status Directory::PopulateChildren(
    DirectoryEntry &dentry,
    Inode &dir_inode,
    DirectoryEntry::Builder &dentry_builder,
    Inode::Builder &inode_builder,
    FileHandle::Builder &handle_builder) {
  ASSIGN_OR_RETURN(FileHandle handle, dir_inode.GetHandle(handle_builder));

  ASSIGN_OR_RETURN(auto dir, [&]() -> absl::StatusOr<absl_nonnull DIR_unique_ptr> {
    ASSIGN_OR_RETURN(FileDescriptor dirfd, handle.Open(O_DIRECTORY | O_RDONLY));
    return WrapDirfd(std::move(dirfd));
  }());
  ASSIGN_OR_RETURN(int dirfd, syscalls::dirfd(*dir));

  DentryID dentry_id = dentry.GetID();

  while (true) {
    ASSIGN_OR_RETURN(dirent *absl_nullable dent, syscalls::readdir(*dir));
    if (dent == nullptr) break;

    ASSIGN_OR_RETURN(
        FileHandle child_handle,
        handle_builder.MakeHandle(dirfd, dent->d_name));
    ASSIGN_OR_RETURN(
      Inode child_inode, inode_builder.Upsert(child_handle, dent->d_type));

    RETURN_IF_ERROR(dentry_builder.Upsert(
        /*parent=*/dentry_id, dent->d_name, /*exists=*/std::nullopt,
        child_inode.GetID(), /*children_fully_populated=*/false));
  }

  RETURN_IF_ERROR(dentry.SetChildrenFullyPopulated(true));
  return absl::OkStatus();
}

absl::StatusOr<Iterator> Directory::begin() {
  ASSIGN_OR_RETURN(
      auto get_children_stmt,
      sqlite3::Statement::Prepare(
          db_, R"(
            SELECT rowid AS dentry_id
            FROM dentries
            WHERE parent = @parent_dentry_id
          )"));

  RETURN_IF_ERROR(
      get_children_stmt.Bind("parent_dentry_id", dentry_.GetID().value()));

  return Iterator(std::move(get_children_stmt));
}

absl::Status Iterator::Advance() {
  using StepResult = ::sqlite3::Statement::StepResult;
  ASSIGN_OR_RETURN(StepResult sr, stmt_.Step());
  if (sr == StepResult::kDone) {
    stmt_ = std::nullopt;
    return absl::OkStatus();
  }

  ASSIGN_OR_RETURN(auto id, stmt_.Column<DentryID::RawType>("dentry_id"));
  DentryID dentry_id(id);

  ASSIGN_OR_RETURN(current_entry_, dentry_builder_.Get(dentry_id));
  return absl::OkStatus();
}

DirectoryEntry Iterator::operator*() {
  return *current_entry_;
}

// Returns true if we've reached the end of iteration.
bool Iterator::End() const {
  return stmt_ == std::nullopt;
}

}  // namespace dcfs
