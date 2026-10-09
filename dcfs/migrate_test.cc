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
#include "dcfs/testonly/assert_ok_and_assign.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"

namespace dcfs {
namespace {

using ::absl_testing::IsOk;
using ::testing::Not;
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

  ASSERT_OK_AND_ASSIGN(DeviceId device_before, GetSourceDeviceId(db_));
  ASSERT_OK_AND_ASSIGN(int64_t inodes_before, CountRows(db_, "inodes"));
  ASSERT_OK_AND_ASSIGN(int64_t fs_before, CountRows(db_, "filesystems"));
  ASSERT_OK_AND_ASSIGN(int64_t dirs_before, CountRows(db_, "directories"));

  ASSERT_THAT(Migrate(db_, root), IsOk());

  EXPECT_THAT(GetSourceDeviceId(db_), IsOkAndHolds(device_before));
  EXPECT_THAT(CountRows(db_, "inodes"), IsOkAndHolds(inodes_before));
  EXPECT_THAT(CountRows(db_, "filesystems"), IsOkAndHolds(fs_before));
  EXPECT_THAT(CountRows(db_, "directories"), IsOkAndHolds(dirs_before));
}

// Phase 6.2: a cache made before the readdir indexes existed (schema v2)
// gets them when the new code opens it, and is then at version 3; its rows
// are untouched.
TEST_F(MigrateTest, V2DatabaseGainsTheReaddirIndexes) {
  RootIdentity root = TestRoot();
  ASSERT_THAT(Migrate(db_, root), IsOk());
  ASSERT_THAT(db_.ExecScript("DROP INDEX dentries_present; "
                             "DROP INDEX dentries_unknown; "
                             "UPDATE cache_state SET schema_version = 2;"),
              IsOk());
  ASSERT_THAT(GetSchemaVersion(db_), IsOkAndHolds(2));
  ASSERT_THAT(CountRows(db_, "sqlite_master WHERE name IN "
                             "('dentries_present', 'dentries_unknown')"),
              IsOkAndHolds(0));
  ASSERT_THAT(db_.Exec("INSERT INTO dentries (parent, name, state, inode) "
                       "VALUES (1, x'61', 'absent', NULL)"),
              IsOk());

  ASSERT_THAT(Migrate(db_, root), IsOk());
  EXPECT_THAT(GetSchemaVersion(db_), IsOkAndHolds(kSchemaVersion));
  EXPECT_THAT(CountRows(db_, "sqlite_master WHERE type = 'index' AND name IN "
                             "('dentries_present', 'dentries_unknown')"),
              IsOkAndHolds(2));
  EXPECT_THAT(CountRows(db_, "dentries"), IsOkAndHolds(1));
  // Opening it again is a no-op.
  EXPECT_THAT(Migrate(db_, root), IsOk());
  EXPECT_THAT(GetSchemaVersion(db_), IsOkAndHolds(kSchemaVersion));
}

// Step 23.5: a v3 cache gains the stubs table, its triggers and index, and
// its refused dentries (which had no stub) become unknown, never absent.
TEST_F(MigrateTest, V3DatabaseGainsStubsAndForgetsItsRefusals) {
  RootIdentity root = TestRoot();
  ASSERT_THAT(Migrate(db_, root), IsOk());
  ASSERT_THAT(db_.ExecScript("DROP TRIGGER dentries_unrefused; "
                             "DROP TRIGGER dentries_refused_deleted; "
                             "DROP TABLE stubs; "
                             "DROP INDEX dentries_refused; "
                             "UPDATE cache_state SET schema_version = 3;"),
              IsOk());
  ASSERT_THAT(db_.Exec("INSERT INTO dentries (parent, name, state, inode) "
                       "VALUES (1, x'6d70', 'refused', NULL), "
                       "(1, x'61', 'absent', NULL)"),
              IsOk());

  ASSERT_THAT(Migrate(db_, root), IsOk());
  EXPECT_THAT(GetSchemaVersion(db_), IsOkAndHolds(kSchemaVersion));
  EXPECT_THAT(CountRows(db_, "sqlite_master WHERE name IN ('stubs', "
                             "'dentries_refused', 'dentries_unrefused', "
                             "'dentries_refused_deleted')"),
              IsOkAndHolds(4));
  EXPECT_THAT(CountRows(db_, "dentries WHERE name = x'6d70' AND "
                             "state = 'unknown'"),
              IsOkAndHolds(1));
  EXPECT_THAT(CountRows(db_, "dentries WHERE name = x'61' AND "
                             "state = 'absent'"),
              IsOkAndHolds(1));
}

// The stubs triggers: a stub goes when its dentry is recorded present or
// absent (relisted as something else, or gone) and when the dentry is
// deleted; a dentry merely forgotten (unknown) keeps it. (That it goes with
// its parent's row is metadata_cache_test's StubsLiveWithTheirRefusals.)
TEST_F(MigrateTest, StubsGoWithTheirRefusals) {
  ASSERT_THAT(Migrate(db_, TestRoot()), IsOk());
  auto add = [&](std::string_view name_hex, int64_t id) {
    ASSERT_THAT(db_.Exec(absl::StrCat(
                    "INSERT INTO dentries (parent, name, state, inode) "
                    "VALUES (1, x'", name_hex, "', 'refused', NULL)")),
                IsOk());
    ASSERT_THAT(db_.Exec(absl::StrCat(
                    "INSERT INTO stubs VALUES (", id, ", 1, x'", name_hex,
                    "', 7, 16877, 2, 0, 0, 0, 0, 0, 4096, "
                    "0, 0, 0, 0, 0, 0, 0, 0)")),
                IsOk());
  };
  add("61", -9223372036854775807 - 1);
  add("62", -9223372036854775807);
  ASSERT_THAT(CountRows(db_, "stubs"), IsOkAndHolds(2));
  ASSERT_THAT(db_.Exec("UPDATE dentries SET state = 'unknown' "
                       "WHERE name = x'61'"),
              IsOk());
  EXPECT_THAT(CountRows(db_, "stubs"), IsOkAndHolds(2));
  ASSERT_THAT(db_.Exec("UPDATE dentries SET state = 'absent' "
                       "WHERE name = x'61'"),
              IsOk());
  EXPECT_THAT(CountRows(db_, "stubs"), IsOkAndHolds(1));
  ASSERT_THAT(db_.Exec("DELETE FROM dentries WHERE name = x'62'"), IsOk());
  EXPECT_THAT(CountRows(db_, "stubs"), IsOkAndHolds(0));
  // ... and a forgotten one deleted (a relisting that does not find it).
  add("65", -9223372036854775807 + 3);
  ASSERT_THAT(db_.Exec("UPDATE dentries SET state = 'unknown' "
                       "WHERE name = x'65'"),
              IsOk());
  ASSERT_THAT(db_.Exec("DELETE FROM dentries WHERE name = x'65'"), IsOk());
  EXPECT_THAT(CountRows(db_, "stubs"), IsOkAndHolds(0));
  // A refused dentry staying refused keeps its stub.
  add("63", -9223372036854775807 + 1);
  ASSERT_THAT(db_.Exec("UPDATE dentries SET state = 'refused' "
                       "WHERE name = x'63'"),
              IsOk());
  EXPECT_THAT(CountRows(db_, "stubs"), IsOkAndHolds(1));
  // Nodeids below 2^63 (non-negative) are not stubs'.
  EXPECT_FALSE(db_.Exec("INSERT INTO stubs VALUES (5, 1, x'64', 7, 16877, "
                        "2, 0, 0, 0, 0, 0, 4096, 0, 0, 0, 0, 0, 0, 0, 0)")
                   .ok());
}

// Step 12.4b: a v4 cache gains the stub high-water mark (the highest stub
// nodeid it ever handed out, so that none is handed out again), the
// triggers that keep a forgotten refusal's stub, and the index of rows with
// nlink 0.
TEST_F(MigrateTest, V4DatabaseGainsTheStubHighWaterMark) {
  RootIdentity root = TestRoot();
  ASSERT_THAT(Migrate(db_, root), IsOk());
  ASSERT_THAT(db_.ExecScript(R"sql(
    ALTER TABLE cache_state DROP COLUMN last_stub_id;
    DROP INDEX inodes_unlinked;
    DROP TRIGGER dentries_unrefused;
    DROP TRIGGER dentries_refused_deleted;
    CREATE TRIGGER dentries_unrefused AFTER UPDATE OF state ON dentries
        WHEN OLD.state = 'refused' AND NEW.state != 'refused' BEGIN
      DELETE FROM stubs WHERE parent = OLD.parent AND name = OLD.name;
    END;
    CREATE TRIGGER dentries_refused_deleted AFTER DELETE ON dentries
        WHEN OLD.state = 'refused' BEGIN
      DELETE FROM stubs WHERE parent = OLD.parent AND name = OLD.name;
    END;
    INSERT INTO dentries (parent, name, state, inode)
        VALUES (1, x'61', 'refused', NULL);
    INSERT INTO stubs VALUES (-9223372036854775806, 1, x'61', 7, 16877, 2,
        0, 0, 0, 0, 0, 4096, 0, 0, 0, 0, 0, 0, 0, 0);
    UPDATE cache_state SET schema_version = 4;
  )sql"),
              IsOk());

  ASSERT_THAT(Migrate(db_, root), IsOk());
  EXPECT_THAT(GetSchemaVersion(db_), IsOkAndHolds(kSchemaVersion));
  EXPECT_THAT(CountRows(db_, "cache_state WHERE "
                             "last_stub_id = -9223372036854775806"),
              IsOkAndHolds(1));
  EXPECT_THAT(CountRows(db_, "sqlite_master WHERE type = 'index' AND "
                             "name = 'inodes_unlinked'"),
              IsOkAndHolds(1));
  ASSERT_THAT(db_.Exec("UPDATE dentries SET state = 'unknown'"), IsOk());
  EXPECT_THAT(CountRows(db_, "stubs"), IsOkAndHolds(1));
  // Opening it again is a no-op.
  EXPECT_THAT(Migrate(db_, root), IsOk());
  EXPECT_THAT(GetSchemaVersion(db_), IsOkAndHolds(kSchemaVersion));
}

// Step 23.8: a v5 cache's dirty rows all stand for mutations: they gain
// atime_only = 0, and the column refuses anything but 0 and 1.
TEST_F(MigrateTest, V5DatabaseGainsTheDirtyRowsReason) {
  RootIdentity root = TestRoot();
  ASSERT_THAT(Migrate(db_, root), IsOk());
  ASSERT_THAT(db_.ExecScript(R"sql(
    ALTER TABLE dirty DROP COLUMN atime_only;
    INSERT INTO dirty (inode) VALUES (1);
    UPDATE cache_state SET schema_version = 5;
  )sql"),
              IsOk());

  ASSERT_THAT(Migrate(db_, root), IsOk());
  EXPECT_THAT(GetSchemaVersion(db_), IsOkAndHolds(kSchemaVersion));
  EXPECT_THAT(CountRows(db_, "dirty WHERE inode = 1 AND atime_only = 0"),
              IsOkAndHolds(1));
  EXPECT_THAT(db_.Exec("UPDATE dirty SET atime_only = 2"), Not(IsOk()));
  EXPECT_THAT(Migrate(db_, root), IsOk());
  EXPECT_THAT(GetSchemaVersion(db_), IsOkAndHolds(kSchemaVersion));
}

TEST_F(MigrateTest, WrongSchemaVersionFailsPrecondition) {
  RootIdentity root = TestRoot();
  ASSERT_THAT(Migrate(db_, root), IsOk());
  ASSERT_THAT(db_.Exec("UPDATE cache_state SET schema_version = 99"), IsOk());

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

TEST_F(MigrateTest, FreshDatabaseHasTypedCacheState) {
  RootIdentity root = TestRoot();
  ASSERT_THAT(Migrate(db_, root), IsOk());
  EXPECT_THAT(CountRows(db_, "cache_state"), IsOkAndHolds(1));
  EXPECT_THAT(CountRows(db_, "sqlite_master WHERE name = 'meta'"),
              IsOkAndHolds(0));
  EXPECT_THAT(GetCleanShutdown(db_), IsOkAndHolds(true));
  EXPECT_THAT(GetBootId(db_), IsOkAndHolds(std::nullopt));

  ASSERT_THAT(SetCleanShutdown(db_, false), IsOk());
  ASSERT_THAT(SetBootId(db_, "abc"), IsOk());
  EXPECT_THAT(GetCleanShutdown(db_), IsOkAndHolds(false));
  EXPECT_THAT(GetBootId(db_), IsOkAndHolds(std::optional<std::string>("abc")));

  // Exactly one row, by construction.
  EXPECT_FALSE(db_.Exec("INSERT INTO cache_state (id, schema_version, "
                        "source_device_id, clean_shutdown) "
                        "VALUES (2, 2, x'00', 1)")
                   .ok());
  // And typed: STRICT rejects a TEXT device id.
  EXPECT_FALSE(
      db_.Exec("UPDATE cache_state SET source_device_id = 'text'").ok());
}

TEST_F(MigrateTest, MissingCacheStateRowIsCorrupt) {
  ASSERT_THAT(Migrate(db_, TestRoot()), IsOk());
  ASSERT_THAT(db_.Exec("DELETE FROM cache_state"), IsOk());
  EXPECT_THAT(GetSourceDeviceId(db_),
              StatusIs(absl::StatusCode::kFailedPrecondition));
  EXPECT_THAT(SetCleanShutdown(db_, true), testing::Not(IsOk()));
}

// Turns a freshly created current-version database back into what schema
// v1 looked like, as far as any later migration step can tell: v1 kept
// cache-wide state in a key/value `meta` table, binding every value --
// including the device id's raw bytes -- as TEXT, and dentries had no
// `refused` column (amendment 12, step 4.8) -- dropped here since this
// starts from a fresh (current-schema) database, which already has it.
absl::Status DowngradeToV1(sqlite3::Connection &db, const DeviceId &device) {
  ABSL_RETURN_IF_ERROR(db.ExecScript(
      "DROP TRIGGER inodes_delete_unknowns; "
      "CREATE TABLE dentries_v1 (parent INTEGER NOT NULL REFERENCES "
      "inodes (id) ON DELETE CASCADE, name BLOB NOT NULL, inode INTEGER "
      "NULL REFERENCES inodes (id) ON DELETE SET NULL, "
      "PRIMARY KEY (parent, name)) STRICT; "
      "INSERT INTO dentries_v1 SELECT parent, name, inode FROM dentries; "
      "DROP TABLE dentries; "
      "ALTER TABLE dentries_v1 RENAME TO dentries; "
      "CREATE INDEX dentries_inode ON dentries (inode); "
      "ALTER TABLE directories DROP COLUMN epoch; "
      "DROP TABLE dirty; DROP TABLE cache_state; DROP TABLE xattrs; "
      "CREATE TABLE xattrs (inode INTEGER NOT NULL REFERENCES inodes (id) "
      "ON DELETE CASCADE, name BLOB NOT NULL, value BLOB NOT NULL, "
      "PRIMARY KEY (inode, name)) STRICT; "
      "CREATE TABLE meta (key TEXT PRIMARY KEY, value ANY) STRICT; "
      "INSERT INTO meta VALUES ('schema_version', '1'), "
      "('gen_counter', '12345');"));
  ABSL_ASSIGN_OR_RETURN(
      sqlite3::Statement * stmt,
      db.Prepared("INSERT INTO meta VALUES ('source_device_id', ?)"));
  std::string bytes = device.Serialize();
  ABSL_RETURN_IF_ERROR(stmt->Bind(1, std::string_view(bytes)));
  return stmt->ExecuteOnce();
}

TEST_F(MigrateTest, UpgradesV1ToCurrentKeepingRows) {
  RootIdentity root = TestRoot();
  ASSERT_THAT(Migrate(db_, root), IsOk());
  ASSERT_THAT(DowngradeToV1(db_, root.device_id), IsOk());
  std::string device_bytes = root.device_id.Serialize();
  ASSERT_OK_AND_ASSIGN(
      sqlite3::Statement * insert,
      db_.Prepared("INSERT INTO inodes "
                    "(id, device_id, backing_ino, backing_gen, fuse_gen) "
                    "VALUES (7, ?, 70, 0, 12345)"));
  ASSERT_THAT(insert->Bind(1, Blob(device_bytes)), IsOk());
  ASSERT_THAT(insert->ExecuteOnce(), IsOk());
  ASSERT_OK_AND_ASSIGN(
      sqlite3::Statement * xattr,
      db_.Prepared("INSERT INTO xattrs (inode, name, value) VALUES (7, ?, ?)"));
  ASSERT_THAT(xattr->Bind(1, Blob("user.k")), IsOk());
  ASSERT_THAT(xattr->Bind(2, Blob("v")), IsOk());
  ASSERT_THAT(xattr->ExecuteOnce(), IsOk());

  ASSERT_THAT(Migrate(db_, root), IsOk());

  EXPECT_THAT(GetSchemaVersion(db_), IsOkAndHolds(kSchemaVersion));
  EXPECT_THAT(CountRows(db_, "sqlite_master WHERE name = 'meta'"),
              IsOkAndHolds(0));
  EXPECT_THAT(GetCleanShutdown(db_), IsOkAndHolds(true));
  EXPECT_THAT(GetBootId(db_), IsOkAndHolds(std::nullopt));
  EXPECT_THAT(CountRows(db_, "inodes WHERE id = 7 AND fuse_gen = 12345"),
              IsOkAndHolds(1));
  EXPECT_THAT(CountRows(db_, "inodes WHERE id = 1 AND fuse_gen = 0"),
              IsOkAndHolds(1));
  EXPECT_THAT(GetSourceDeviceId(db_), IsOkAndHolds(root.device_id));
  EXPECT_THAT(CountRows(db_, "dirty"), IsOkAndHolds(0));
  // A v1 xattr row is a present one.
  EXPECT_THAT(CountRows(db_, "xattrs WHERE inode = 7 AND state = 'present' "
                             "AND value = CAST('v' AS BLOB)"),
              IsOkAndHolds(1));

  // And the upgraded database is now current: migrating again is a no-op.
  ASSERT_THAT(Migrate(db_, root), IsOk());
  EXPECT_THAT(GetSchemaVersion(db_), IsOkAndHolds(kSchemaVersion));
}

TEST_F(MigrateTest, OlderThanV1FailsPrecondition) {
  RootIdentity root = TestRoot();
  ASSERT_THAT(Migrate(db_, root), IsOk());
  ASSERT_THAT(DowngradeToV1(db_, root.device_id), IsOk());
  ASSERT_THAT(
      db_.Exec("UPDATE meta SET value = '0' WHERE key = 'schema_version'"),
      IsOk());
  EXPECT_THAT(Migrate(db_, root),
              StatusIs(absl::StatusCode::kFailedPrecondition));
  // The failed upgrade rolled back: nothing changed.
  EXPECT_THAT(CountRows(db_, "meta WHERE key = 'schema_version' AND "
                             "value = '0'"),
              IsOkAndHolds(1));
  EXPECT_THAT(CountRows(db_, "sqlite_master WHERE name = 'cache_state'"),
              IsOkAndHolds(0));
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

// Audit F8: deleting an inode row leaves the dentries that pointed at it
// unknown, never a negative ("absent") entry.
TEST_F(MigrateTest, DeletingInodeLeavesDentryUnknown) {
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
      db_.Prepared("INSERT INTO dentries (parent, name, state, inode) "
                    "VALUES (1, ?, 'present', 2)"));
  ASSERT_THAT(dentry_stmt->Bind(1, Blob("child")), IsOk());
  ASSERT_THAT(dentry_stmt->ExecuteOnce(), IsOk());

  ASSERT_THAT(db_.Exec("DELETE FROM inodes WHERE id = 2"), IsOk());

  ASSERT_OK_AND_ASSIGN(
      sqlite3::Statement * check,
      db_.Prepared(
          "SELECT inode, state FROM dentries WHERE parent = 1 AND name = ?"));
  ASSERT_THAT(check->Bind(1, Blob("child")), IsOk());
  ASSERT_THAT(check->Step(), IsOkAndHolds(true));
  EXPECT_TRUE(check->ColumnIsNull(0));
  EXPECT_EQ(check->Column<std::string>(1), "unknown");
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
          "INSERT INTO dentries (parent, name, state, inode) "
          "VALUES (1, ?, 'present', 2)"));
  ASSERT_THAT(dentry_stmt->Bind(1, Blob("mnt")), IsOk());
  ASSERT_THAT(dentry_stmt->ExecuteOnce(), IsOk());

  ASSERT_OK_AND_ASSIGN(
      sqlite3::Statement * xattr_stmt,
      db_.Prepared(
          "INSERT INTO xattrs (inode, name, state, value) "
          "VALUES (2, ?, 'present', ?)"));
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
  // an unknown entry (the mount point still exists), never a negative one.
  ASSERT_OK_AND_ASSIGN(
      sqlite3::Statement * check,
      db_.Prepared(
          "SELECT inode, state FROM dentries WHERE parent = 1 AND name = ?"));
  ASSERT_THAT(check->Bind(1, Blob("mnt")), IsOk());
  ASSERT_THAT(check->Step(), IsOkAndHolds(true));
  EXPECT_TRUE(check->ColumnIsNull(0));
  EXPECT_EQ(check->Column<std::string>(1), "unknown");

  EXPECT_THAT(CountRows(db_, "inodes WHERE id = 1"), IsOkAndHolds(1));
  EXPECT_THAT(CountRows(db_, "directories WHERE inode = 1"), IsOkAndHolds(1));
}

TEST_F(MigrateTest, DentriesHaveAUsableRowid) {
  RootIdentity root = TestRoot();
  ASSERT_THAT(Migrate(db_, root), IsOk());

  ASSERT_OK_AND_ASSIGN(
      sqlite3::Statement * insert_a,
      db_.Prepared(
          "INSERT INTO dentries (parent, name, state, inode) "
          "VALUES (1, ?, 'absent', NULL)"));
  ASSERT_THAT(insert_a->Bind(1, Blob("a")), IsOk());
  ASSERT_THAT(insert_a->ExecuteOnce(), IsOk());

  ASSERT_OK_AND_ASSIGN(
      sqlite3::Statement * insert_b,
      db_.Prepared(
          "INSERT INTO dentries (parent, name, state, inode) "
          "VALUES (1, ?, 'absent', NULL)"));
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


// Regression test for the typed cache_state table replacing the untyped
// key/value meta table, both in a fresh cache and after the v1 -> v2
// upgrade. Raw SQL only (no accessors), so it runs unchanged against the
// code from before the change, where it fails.
TEST_F(MigrateTest, CacheStateIsTypedAndMetaIsGone) {
  RootIdentity root = TestRoot();
  ASSERT_THAT(Migrate(db_, root), IsOk());
  EXPECT_THAT(CountRows(db_, "sqlite_master WHERE type = 'table' AND "
                             "name = 'cache_state'"),
              IsOkAndHolds(1));
  EXPECT_THAT(CountRows(db_, "sqlite_master WHERE name = 'meta'"),
              IsOkAndHolds(0));

  // A v1 database, as v1 wrote it: every meta value bound as TEXT, and no
  // dentries.refused column (amendment 12, step 4.8; dropped here since
  // this starts from a fresh, current-schema database, which already has
  // it).
  ASSERT_THAT(db_.ExecScript(
                  "DROP TABLE IF EXISTS dirty; "
                  "DROP TABLE IF EXISTS cache_state; "
                  "DROP TABLE IF EXISTS meta; "
                  "DROP TRIGGER inodes_delete_unknowns; "
                  "CREATE TABLE dentries_v1 (parent INTEGER NOT NULL REFERENCES "
                  "inodes (id) ON DELETE CASCADE, name BLOB NOT NULL, inode INTEGER "
                  "NULL REFERENCES inodes (id) ON DELETE SET NULL, "
                  "PRIMARY KEY (parent, name)) STRICT; "
                  "INSERT INTO dentries_v1 SELECT parent, name, inode FROM dentries; "
                  "DROP TABLE dentries; "
                  "ALTER TABLE dentries_v1 RENAME TO dentries; "
                  "CREATE INDEX dentries_inode ON dentries (inode); "
                  "ALTER TABLE directories DROP COLUMN epoch; "
                  "CREATE TABLE meta (key TEXT PRIMARY KEY, value ANY) STRICT; "
                  "INSERT INTO meta VALUES ('schema_version', '1'), "
                  "('gen_counter', '12345');"),
              IsOk());
  std::string device_bytes = root.device_id.Serialize();
  ASSERT_OK_AND_ASSIGN(
      sqlite3::Statement * insert,
      db_.Prepared("INSERT INTO meta VALUES ('source_device_id', ?)"));
  ASSERT_THAT(insert->Bind(1, std::string_view(device_bytes)), IsOk());
  ASSERT_THAT(insert->ExecuteOnce(), IsOk());

  ASSERT_THAT(Migrate(db_, root), IsOk());
  EXPECT_THAT(CountRows(db_, "sqlite_master WHERE name = 'meta'"),
              IsOkAndHolds(0));
  ASSERT_OK_AND_ASSIGN(
      sqlite3::Statement * state,
      db_.Prepared("SELECT schema_version, typeof(source_device_id), "
                   "source_device_id, clean_shutdown FROM cache_state"));
  ASSERT_THAT(state->Step(), IsOkAndHolds(true));
  EXPECT_EQ(state->Column<int>(0), kSchemaVersion);
  EXPECT_EQ(state->Column<std::string>(1), "blob");
  std::vector<uint8_t> stored = state->Column<std::vector<uint8_t>>(2);
  EXPECT_EQ(std::string(stored.begin(), stored.end()), device_bytes);
  EXPECT_EQ(state->Column<int>(3), 1);
  ASSERT_THAT(state->Step(), IsOkAndHolds(false));

  // The upgraded dentries table has the readdir indexes a fresh one has
  // (Phase 6.2: schema.sql, "dentries_present").
  EXPECT_THAT(CountRows(db_, "sqlite_master WHERE type = 'index' AND name IN "
                             "('dentries_present', 'dentries_unknown')"),
              IsOkAndHolds(2));
}

}  // namespace
}  // namespace dcfs
