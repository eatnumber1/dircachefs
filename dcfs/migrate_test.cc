#include "dcfs/migrate.h"

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "absl/status/status.h"
#include "absl/status/status_macros.h"
#include "absl/status/status_matchers.h"
#include "absl/status/statusor.h"
#include "absl/strings/str_cat.h"
#include "dcfs/device_id.h"
#include "dcfs/ret_check.h"
#include "dcfs/sqlite.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"

// This version of Abseil's status_matchers.h doesn't provide
// ASSERT_OK_AND_ASSIGN, so define the usual helper locally (scoped to this
// file only) -- same as dcfs/sqlite_test.cc.
#define DCFS_TEST_CONCAT_INNER(x, y) x##y
#define DCFS_TEST_CONCAT(x, y) DCFS_TEST_CONCAT_INNER(x, y)
#define ASSERT_OK_AND_ASSIGN(lhs, rexpr)                        \
  ASSERT_OK_AND_ASSIGN_IMPL(                                    \
      DCFS_TEST_CONCAT(_status_or_value_, __LINE__), lhs, rexpr)
#define ASSERT_OK_AND_ASSIGN_IMPL(statusor, lhs, rexpr) \
  auto statusor = (rexpr);                              \
  ASSERT_THAT(statusor, ::absl_testing::IsOk());        \
  lhs = std::move(statusor).value()

namespace dcfs {
namespace {

using ::absl_testing::IsOk;
using ::absl_testing::IsOkAndHolds;
using ::absl_testing::StatusIs;

// Views the bytes of `s` as a blob for Statement::Bind() -- see the
// AsBlob() comment in migrate.cc. Several schema columns exercised directly
// by these tests (dentries.name, xattrs.name/value, symlinks.target) are
// BLOB in a STRICT table and reject a bound TEXT value outright.
std::span<const uint8_t> Blob(std::string_view s) {
  return std::span<const uint8_t>(
      reinterpret_cast<const uint8_t *>(s.data()), s.size());
}

DeviceId TestDeviceId(uint8_t fill) {
  DeviceId id;
  id.uuid.fill(fill);
  return id;
}

RootIdentity TestRoot() {
  return RootIdentity{
      .device_id = TestDeviceId(0xAB),
      .fstype = 0xEF53,
      .backing_ino = 2,
      .backing_gen = 0,
  };
}

absl::StatusOr<int64_t> CountRows(sqlite3::Connection &db,
                                   std::string_view from_where) {
  ABSL_ASSIGN_OR_RETURN(
      sqlite3::Statement * stmt,
      db.Prepared(absl::StrCat("SELECT COUNT(*) FROM ", from_where)));
  ABSL_ASSIGN_OR_RETURN(bool has_row, stmt->Step());
  RET_CHECK(has_row);
  int64_t count = stmt->Column<int64_t>(0);
  ABSL_RETURN_IF_ERROR(stmt->Reset());
  return count;
}

class MigrateTest : public ::testing::Test {
 protected:
  void SetUp() override {
    ASSERT_OK_AND_ASSIGN(
        db_, sqlite3::ConnectionFactory{.path = ":memory:"}.Open());
  }

  sqlite3::Connection db_;
};

TEST_F(MigrateTest, FreshDatabaseMigratesAndSeedsRoot) {
  RootIdentity root = TestRoot();
  ASSERT_THAT(Migrate(db_, root), IsOk());

  EXPECT_THAT(GetSchemaVersion(db_), IsOkAndHolds(kSchemaVersion));

  ASSERT_OK_AND_ASSIGN(DeviceId source, GetSourceDeviceId(db_));
  EXPECT_EQ(source, root.device_id);

  ASSERT_OK_AND_ASSIGN(
      sqlite3::Statement * stmt,
      db_.Prepared("SELECT fuse_gen, backing_ino, backing_gen, attrs_valid "
                    "FROM inodes WHERE id = 1"));
  ASSERT_THAT(stmt->Step(), IsOkAndHolds(true));
  EXPECT_EQ(stmt->Column<int64_t>(0), 0);
  EXPECT_EQ(stmt->Column<uint64_t>(1), root.backing_ino);
  EXPECT_EQ(stmt->Column<uint64_t>(2), root.backing_gen);
  EXPECT_EQ(stmt->Column<bool>(3), false);
  ASSERT_THAT(stmt->Step(), IsOkAndHolds(false));

  EXPECT_THAT(CountRows(db_, "directories WHERE inode = 1"), IsOkAndHolds(1));
  EXPECT_THAT(CountRows(db_, "filesystems WHERE parent_inode IS NULL"),
              IsOkAndHolds(1));
}

TEST_F(MigrateTest, MigratingAgainIsANoOp) {
  RootIdentity root = TestRoot();
  ASSERT_THAT(Migrate(db_, root), IsOk());

  ASSERT_OK_AND_ASSIGN(std::optional<std::string> device_before,
                        GetMeta(db_, "source_device_id"));
  ASSERT_TRUE(device_before.has_value());
  ASSERT_OK_AND_ASSIGN(int64_t inodes_before, CountRows(db_, "inodes"));
  ASSERT_OK_AND_ASSIGN(int64_t fs_before, CountRows(db_, "filesystems"));
  ASSERT_OK_AND_ASSIGN(int64_t dirs_before, CountRows(db_, "directories"));

  ASSERT_THAT(Migrate(db_, root), IsOk());

  EXPECT_THAT(GetMeta(db_, "source_device_id"), IsOkAndHolds(device_before));
  EXPECT_THAT(CountRows(db_, "inodes"), IsOkAndHolds(inodes_before));
  EXPECT_THAT(CountRows(db_, "filesystems"), IsOkAndHolds(fs_before));
  EXPECT_THAT(CountRows(db_, "directories"), IsOkAndHolds(dirs_before));
}

TEST_F(MigrateTest, WrongSchemaVersionFailsPrecondition) {
  RootIdentity root = TestRoot();
  ASSERT_THAT(Migrate(db_, root), IsOk());
  ASSERT_THAT(SetMeta(db_, "schema_version", "99"), IsOk());

  EXPECT_THAT(Migrate(db_, root),
              StatusIs(absl::StatusCode::kFailedPrecondition));
}

TEST_F(MigrateTest, MissingRootInodeFailsPreconditionAsCorrupt) {
  RootIdentity root = TestRoot();
  ASSERT_THAT(Migrate(db_, root), IsOk());
  ASSERT_THAT(db_.Exec("DELETE FROM inodes WHERE id = 1"), IsOk());

  EXPECT_THAT(Migrate(db_, root),
              StatusIs(absl::StatusCode::kFailedPrecondition));
}

TEST_F(MigrateTest, FreshDatabaseHasNoGenerationCounter) {
  ASSERT_THAT(Migrate(db_, TestRoot()), IsOk());
  EXPECT_THAT(GetMeta(db_, "gen_counter"), IsOkAndHolds(std::nullopt));
}

// Turns a freshly created current-version database back into what schema
// v1 looked like, as far as any later migration step can tell.
absl::Status DowngradeToV1(sqlite3::Connection &db) {
  ABSL_RETURN_IF_ERROR(db.Exec("DROP TABLE dirty"));
  ABSL_RETURN_IF_ERROR(SetMeta(db, "schema_version", "1"));
  return SetMeta(db, "gen_counter", "12345");
}

TEST_F(MigrateTest, UpgradesV1ToCurrentKeepingRows) {
  RootIdentity root = TestRoot();
  ASSERT_THAT(Migrate(db_, root), IsOk());
  ASSERT_THAT(DowngradeToV1(db_), IsOk());
  std::string device_bytes = root.device_id.Serialize();
  ASSERT_OK_AND_ASSIGN(
      sqlite3::Statement * insert,
      db_.Prepared("INSERT INTO inodes "
                    "(id, device_id, backing_ino, backing_gen, fuse_gen) "
                    "VALUES (7, ?, 70, 0, 12345)"));
  ASSERT_THAT(insert->Bind(1, Blob(device_bytes)), IsOk());
  ASSERT_THAT(insert->ExecuteOnce(), IsOk());

  ASSERT_THAT(Migrate(db_, root), IsOk());

  EXPECT_THAT(GetSchemaVersion(db_), IsOkAndHolds(kSchemaVersion));
  EXPECT_THAT(GetMeta(db_, "gen_counter"), IsOkAndHolds(std::nullopt));
  EXPECT_THAT(CountRows(db_, "inodes WHERE id = 7 AND fuse_gen = 12345"),
              IsOkAndHolds(1));
  EXPECT_THAT(CountRows(db_, "inodes WHERE id = 1 AND fuse_gen = 0"),
              IsOkAndHolds(1));
  EXPECT_THAT(GetSourceDeviceId(db_), IsOkAndHolds(root.device_id));
  EXPECT_THAT(CountRows(db_, "dirty"), IsOkAndHolds(0));

  // And the upgraded database is now current: migrating again is a no-op.
  ASSERT_THAT(Migrate(db_, root), IsOk());
  EXPECT_THAT(GetSchemaVersion(db_), IsOkAndHolds(kSchemaVersion));
}

TEST_F(MigrateTest, OlderThanV1FailsPrecondition) {
  RootIdentity root = TestRoot();
  ASSERT_THAT(Migrate(db_, root), IsOk());
  ASSERT_THAT(SetMeta(db_, "schema_version", "0"), IsOk());
  EXPECT_THAT(Migrate(db_, root),
              StatusIs(absl::StatusCode::kFailedPrecondition));
  // The failed upgrade rolled back: nothing changed.
  EXPECT_THAT(GetMeta(db_, "schema_version"), IsOkAndHolds("0"));
}

TEST_F(MigrateTest, DuplicateBackingIdentityViolatesUniqueConstraint) {
  RootIdentity root = TestRoot();
  ASSERT_THAT(Migrate(db_, root), IsOk());
  std::string device_bytes = root.device_id.Serialize();

  ASSERT_OK_AND_ASSIGN(
      sqlite3::Statement * stmt,
      db_.Prepared(
          "INSERT INTO inodes "
          "(id, device_id, backing_ino, backing_gen, fuse_gen, attrs_valid) "
          "VALUES (2, ?, ?, ?, 1, 0)"));
  ASSERT_THAT(stmt->Bind(1, Blob(device_bytes)), IsOk());
  ASSERT_THAT(stmt->Bind(2, root.backing_ino), IsOk());
  ASSERT_THAT(stmt->Bind(3, root.backing_gen), IsOk());

  absl::StatusOr<bool> result = stmt->Step();
  ASSERT_FALSE(result.ok());
  EXPECT_EQ(result.status().code(), absl::StatusCode::kAlreadyExists);
}

TEST_F(MigrateTest, DeletingInodeLeavesDentryAsNegativeEntry) {
  RootIdentity root = TestRoot();
  ASSERT_THAT(Migrate(db_, root), IsOk());
  std::string device_bytes = root.device_id.Serialize();

  ASSERT_OK_AND_ASSIGN(
      sqlite3::Statement * inode_stmt,
      db_.Prepared(
          "INSERT INTO inodes "
          "(id, device_id, backing_ino, backing_gen, fuse_gen, attrs_valid) "
          "VALUES (2, ?, 99, 0, 1, 0)"));
  ASSERT_THAT(inode_stmt->Bind(1, Blob(device_bytes)), IsOk());
  ASSERT_THAT(inode_stmt->ExecuteOnce(), IsOk());

  ASSERT_OK_AND_ASSIGN(
      sqlite3::Statement * dentry_stmt,
      db_.Prepared("INSERT INTO dentries (parent, name, inode) "
                    "VALUES (1, ?, 2)"));
  ASSERT_THAT(dentry_stmt->Bind(1, Blob("child")), IsOk());
  ASSERT_THAT(dentry_stmt->ExecuteOnce(), IsOk());

  ASSERT_THAT(db_.Exec("DELETE FROM inodes WHERE id = 2"), IsOk());

  ASSERT_OK_AND_ASSIGN(
      sqlite3::Statement * check,
      db_.Prepared(
          "SELECT inode FROM dentries WHERE parent = 1 AND name = ?"));
  ASSERT_THAT(check->Bind(1, Blob("child")), IsOk());
  ASSERT_THAT(check->Step(), IsOkAndHolds(true));
  EXPECT_TRUE(check->ColumnIsNull(0));
  ASSERT_THAT(check->Step(), IsOkAndHolds(false));
}

TEST_F(MigrateTest, DeletingFilesystemCascadesButRootSurvives) {
  RootIdentity root = TestRoot();
  ASSERT_THAT(Migrate(db_, root), IsOk());

  std::string child_bytes = TestDeviceId(0xCD).Serialize();

  ASSERT_OK_AND_ASSIGN(
      sqlite3::Statement * fs_stmt,
      db_.Prepared("INSERT INTO filesystems "
                   "(device_id, fstype, parent_inode, boundary_name) "
                   "VALUES (?, ?, 1, ?)"));
  ASSERT_THAT(fs_stmt->Bind(1, Blob(child_bytes)), IsOk());
  ASSERT_THAT(fs_stmt->Bind(2, int64_t{0xEF53}), IsOk());
  ASSERT_THAT(fs_stmt->Bind(3, Blob("mnt")), IsOk());
  ASSERT_THAT(fs_stmt->ExecuteOnce(), IsOk());

  ASSERT_OK_AND_ASSIGN(
      sqlite3::Statement * inode_stmt,
      db_.Prepared(
          "INSERT INTO inodes "
          "(id, device_id, backing_ino, backing_gen, fuse_gen, attrs_valid) "
          "VALUES (2, ?, 5, 0, 7, 0)"));
  ASSERT_THAT(inode_stmt->Bind(1, Blob(child_bytes)), IsOk());
  ASSERT_THAT(inode_stmt->ExecuteOnce(), IsOk());

  ASSERT_OK_AND_ASSIGN(
      sqlite3::Statement * dir_stmt,
      db_.Prepared(
          "INSERT INTO directories (inode, children_complete) "
          "VALUES (2, 0)"));
  ASSERT_THAT(dir_stmt->ExecuteOnce(), IsOk());

  ASSERT_OK_AND_ASSIGN(
      sqlite3::Statement * dentry_stmt,
      db_.Prepared(
          "INSERT INTO dentries (parent, name, inode) VALUES (1, ?, 2)"));
  ASSERT_THAT(dentry_stmt->Bind(1, Blob("mnt")), IsOk());
  ASSERT_THAT(dentry_stmt->ExecuteOnce(), IsOk());

  ASSERT_OK_AND_ASSIGN(
      sqlite3::Statement * xattr_stmt,
      db_.Prepared(
          "INSERT INTO xattrs (inode, name, value) VALUES (2, ?, ?)"));
  ASSERT_THAT(xattr_stmt->Bind(1, Blob("user.foo")), IsOk());
  ASSERT_THAT(xattr_stmt->Bind(2, Blob("bar")), IsOk());
  ASSERT_THAT(xattr_stmt->ExecuteOnce(), IsOk());

  ASSERT_OK_AND_ASSIGN(
      sqlite3::Statement * symlink_stmt,
      db_.Prepared("INSERT INTO symlinks (inode, target) VALUES (2, ?)"));
  ASSERT_THAT(symlink_stmt->Bind(1, Blob("target")), IsOk());
  ASSERT_THAT(symlink_stmt->ExecuteOnce(), IsOk());

  ASSERT_OK_AND_ASSIGN(
      sqlite3::Statement * delete_stmt,
      db_.Prepared("DELETE FROM filesystems WHERE device_id = ?"));
  ASSERT_THAT(delete_stmt->Bind(1, Blob(child_bytes)), IsOk());
  ASSERT_THAT(delete_stmt->ExecuteOnce(), IsOk());

  EXPECT_THAT(CountRows(db_, "inodes WHERE id = 2"), IsOkAndHolds(0));
  EXPECT_THAT(CountRows(db_, "directories WHERE inode = 2"), IsOkAndHolds(0));
  EXPECT_THAT(CountRows(db_, "xattrs WHERE inode = 2"), IsOkAndHolds(0));
  EXPECT_THAT(CountRows(db_, "symlinks WHERE inode = 2"), IsOkAndHolds(0));

  // The dentry through which the deleted filesystem was mounted survives as
  // a negative entry rather than being deleted.
  ASSERT_OK_AND_ASSIGN(
      sqlite3::Statement * check,
      db_.Prepared(
          "SELECT inode FROM dentries WHERE parent = 1 AND name = ?"));
  ASSERT_THAT(check->Bind(1, Blob("mnt")), IsOk());
  ASSERT_THAT(check->Step(), IsOkAndHolds(true));
  EXPECT_TRUE(check->ColumnIsNull(0));

  EXPECT_THAT(CountRows(db_, "inodes WHERE id = 1"), IsOkAndHolds(1));
  EXPECT_THAT(CountRows(db_, "directories WHERE inode = 1"), IsOkAndHolds(1));
}

TEST_F(MigrateTest, DentriesHaveAUsableRowid) {
  RootIdentity root = TestRoot();
  ASSERT_THAT(Migrate(db_, root), IsOk());

  ASSERT_OK_AND_ASSIGN(
      sqlite3::Statement * insert_a,
      db_.Prepared(
          "INSERT INTO dentries (parent, name, inode) VALUES (1, ?, NULL)"));
  ASSERT_THAT(insert_a->Bind(1, Blob("a")), IsOk());
  ASSERT_THAT(insert_a->ExecuteOnce(), IsOk());

  ASSERT_OK_AND_ASSIGN(
      sqlite3::Statement * insert_b,
      db_.Prepared(
          "INSERT INTO dentries (parent, name, inode) VALUES (1, ?, NULL)"));
  ASSERT_THAT(insert_b->Bind(1, Blob("b")), IsOk());
  ASSERT_THAT(insert_b->ExecuteOnce(), IsOk());

  ASSERT_OK_AND_ASSIGN(
      sqlite3::Statement * select,
      db_.Prepared("SELECT rowid FROM dentries ORDER BY rowid"));
  std::vector<int64_t> rowids;
  ASSERT_THAT(select->ForEachRow([&](sqlite3::Statement &s) -> absl::Status {
                rowids.push_back(s.Column<int64_t>(0));
                return absl::OkStatus();
              }),
              IsOk());
  ASSERT_EQ(rowids.size(), 2u);
  EXPECT_LT(rowids[0], rowids[1]);
}

}  // namespace
}  // namespace dcfs
