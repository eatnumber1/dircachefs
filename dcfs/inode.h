#include "dcfs/strong_number.h"

namespace dcfs {

struct InodeID : public StrongNumber<int64_t, InodeID> {};

class Inode {
 public:
  // The inode's type, as defined by the S_IFMT mask bits of stat.st_mode. See
  // inode(7).
  using Type = mode_t;

  class Builder {
   public:
    static absl::StatusOr<Builder> Create(sqlite3::Connection *absl_nonnull db);

    // Insert a new Inode, or if already present, update the file type (if not
    // nullopt).
    absl::StatusOr<Inode> Upsert(
        const FileHandle &handle,
        std::optional<Type> type = std::nullopt);

    absl::StatusOr<Inode> Get(InodeID id);

   private:
    Builder(sqlite3::Connection *absl_nonnull db);

    Inode::Statements CreateStatements();

    struct {
      sqlite3::Statement upsert;
    } statements_;

    sqlite3::Connection &db_;
  };

  InodeID GetID() const;
  absl::StatusOr<FileHandle> GetHandle(FileHandle::Builder &builder) const;

  absl::StatusOr<std::optional<Type>> GetType() const;
  absl::Status SetType(std::optional<Type> type);

 private:
  DirectoryEntry(
      sqlite3::Connection *absl_nonnull db,
      absl::flat_hash_map<std::string, sqlite3::Statement> statements,
      DentryID id);

  struct Statements {
    sqlite3::Statement get_type;
    sqlite3::Statement set_type;
    sqlite3::Statement get_handle_and_devuuid;
  };

  InodeID id_;
  Statements statements_;
};

}  // namespace dcfs
