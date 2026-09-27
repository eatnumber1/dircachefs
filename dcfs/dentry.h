#include <utility>

#include "dcfs/strong_number.h"
#include "dcfs/syscalls.h"

namespace dcfs {

struct DentryID : public StrongNumber<int64_t, DentryID> {}

// A DirectoryEntry is a single entry... in a directory. It has a name and a
// parent (the directory that contains it), but it may or may not have children,
// since it may be any one of a file, directory, block device, or several other
// things. If it is a directory, it has children.
class DirectoryEntry {
 public:
  class Builder {
   public:
    static absl::StatusOr<Builder> Create(sqlite3::Connection *absl_nonnull db);

    // Insert a Dirent. If the (name,parent) is already present, merge the
    // fields as follows:
    //  - optional values: nullopt in the db gets overridden with the argument
    //  - children_fully_populated: a true argument overrides false in the db
    absl::StatusOr<DirectoryEntry> Upsert(
        DentryID parent, std::string_view name,
        std::optional<bool> exists = std::nullopt,
        std::optional<InodeID> inode = std::nullopt,
        bool children_fully_populated = false);

    absl::StatusOr<DirectoryEntry> UpsertRoot(
        InodeID root_inode,
        std::optional<bool> exists = std::nullopt);

    absl::StatusOr<DirectoryEntry> Get(DentryID id);

    // Get a dentry for a directory entry by inode. This only works because
    // hardlinks to directories are not allowed, so there can only exist one
    // dentry per inode.
    absl::StatusOr<DirectoryEntry> GetDirectory(InodeID inode);
   private:
    Builder(sqlite3::Connection *absl_nonnull db);

    absl::StatusOr<DirectoryEntry> Upsert(
        std::optional<DentryID> parent, std::string_view name,
        std::optional<bool> exists, std::optional<InodeID> inode,
        bool children_fully_populated);

    absl::StatusOr<absl::flat_hash_map<std::string, sqlite3::Statement>>
      CreateStatements();

    sqlite3::Connection &db_;
  };

  DentryID GetID() const;
  absl::StatusOr<std::string_view> GetName() const;

  // Returns whether missing children of this directory is fully populated (e.g.
  // should return ENOENT if not present in the db) and skip the underlying fs.
  absl::StatusOr<bool> IsChildrenFullyPopulated() const;
  absl::Status SetChildrenFullyPopulated(bool fully_populated);

  // Returns whether this directory entry exists (or not) on the underlying fs.
  // If nullopt, it should be checked against the underlying fs, even if
  // IsChildrenFullyPopulated of the parent dir is true.
  absl::StatusOr<std::optional<bool>> Exists() const;
  absl::Status SetExists(std::optional<bool> exists);

  absl::StatusOr<InodeID> GetInode() const;
  absl::Status SetInode(InodeID inode);

 private:
  DirectoryEntry(
      sqlite3::Connection *absl_nonnull db,
      absl::flat_hash_map<std::string, sqlite3::Statement> statements,
      DentryID id);

  sqlite3::Connection &db_;
  mutable absl::flat_hash_map<std::string, sqlite3::Statement> statements_;
  DentryID id_;
};

}  // namespace dcfs
