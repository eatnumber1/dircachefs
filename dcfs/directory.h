#include <iterator>

namespace dcfs {

class Directory {
 public:
  Directory(DirectoryEntry dentry, DirectoryEntry::Builder *absl_nonnull dentry_builder);

  static absl::StatusOr<Directory> OpenDir(
      sqlite3::Connection *absl_nonnull db,
      Inode inode,
      DirectoryEntry::Builder *absl_nonnull dentry_builder,
      Inode::Builder &inode_builder,
      FileHandle::Builder &handle_builder);

  // Similar to an std::input_iterator, but Advance can fail, so it can't be
  // used in range-based for loops.
  class Iterator {
   public:
    DirectoryEntry operator*();

    absl::Status Advance();

    // Returns true if we've reached the end of iteration.
    bool End() const;

    Iterator() = default;

   private:
    Iterator(sqlite3::Statement stmt) = default;

    std::optional<sqlite3::Statement> stmt_;  // nullopt if end
    std::optional<DirectoryEntry> current_entry_;
  };

  absl::StatusOr<Iterator> begin();

 private:
  absl::Status Directory::PopulateChildren(
      DirectoryEntry &dentry,
      Inode &dir_inode,
      DirectoryEntry::Builder &dentry_builder,
      Inode::Builder &inode_builder,
      FileHandle::Builder &handle_builder);

  sqlite3::Connection &db_;
  DirectoryEntry dentry_;
  DirectoryEntry::Builder &dentry_builder_;
};

}  // namespace dcfs
