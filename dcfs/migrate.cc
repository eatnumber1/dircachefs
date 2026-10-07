#include "dcfs/migrate.h"

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "absl/status/status.h"
#include "absl/status/status_macros.h"
#include "absl/status/statusor.h"
#include "absl/strings/numbers.h"
#include "dcfs/device_id.h"
#include "dcfs/ret_check.h"
#include "dcfs/schema.h"
#include "dcfs/sqlite.h"
#include "dcfs/status.h"

namespace dcfs {

namespace {

// Views the bytes of `s` as a blob for Statement::Bind(). Several schema
// columns that hold raw bytes (e.g. filesystems.device_id) are declared
// BLOB in a STRICT table, which -- unlike an ordinary table -- rejects a
// TEXT value outright rather than coercing it, so these must go through
// the span<uint8_t> Bind() overload rather than the string_view one.
std::span<const uint8_t> AsBlob(const std::string &s) {
  return std::span<const uint8_t>(
      reinterpret_cast<const uint8_t *>(s.data()), s.size());
}

absl::StatusOr<bool> TableExists(sqlite3::Connection &db,
                                 std::string_view name) {
  ABSL_ASSIGN_OR_RETURN(
      sqlite3::Statement * stmt,
      db.Prepared("SELECT 1 FROM sqlite_master "
                  "WHERE type = 'table' AND name = ?"));
  ABSL_RETURN_IF_ERROR(stmt->Bind(1, name));
  ABSL_ASSIGN_OR_RETURN(bool exists, stmt->Step());
  ABSL_RETURN_IF_ERROR(stmt->Reset());
  return exists;
}

// Runs `sql`, a SELECT of the one cache_state row, and returns read_row(row).
template <typename T, typename Read>
absl::StatusOr<T> ReadCacheState(sqlite3::Connection &db, std::string_view sql,
                                 Read read_row) {
  ABSL_ASSIGN_OR_RETURN(sqlite3::Statement * stmt, db.Prepared(sql));
  ABSL_ASSIGN_OR_RETURN(bool has_row, stmt->Step());
  if (!has_row) {
    ABSL_RETURN_IF_ERROR(stmt->Reset());
    return FailedPreconditionErrorBuilder()
           << "corrupt cache: the cache_state row is missing";
  }
  T value = read_row(*stmt);
  ABSL_RETURN_IF_ERROR(stmt->Reset());
  return value;
}

// The schema version of an existing database: cache_state's, or 1 for a
// v1 database (which kept it in its key/value `meta` table instead).
absl::StatusOr<int> ExistingSchemaVersion(sqlite3::Connection &db) {
  ABSL_ASSIGN_OR_RETURN(bool has_state, TableExists(db, "cache_state"));
  if (has_state) return GetSchemaVersion(db);
  ABSL_ASSIGN_OR_RETURN(
      sqlite3::Statement * stmt,
      db.Prepared("SELECT value FROM meta WHERE key = 'schema_version'"));
  ABSL_ASSIGN_OR_RETURN(bool has_row, stmt->Step());
  std::string value = has_row ? stmt->Column<std::string>(0) : "";
  ABSL_RETURN_IF_ERROR(stmt->Reset());
  int version = 0;
  if (!absl::SimpleAtoi(value, &version)) {
    return FailedPreconditionErrorBuilder()
           << "corrupt cache: meta.schema_version is not an integer: '"
           << value << "'";
  }
  return version;
}

absl::StatusOr<bool> RootInodeExists(sqlite3::Connection &db) {
  ABSL_ASSIGN_OR_RETURN(sqlite3::Statement * stmt,
                         db.Prepared("SELECT 1 FROM inodes WHERE id = 1"));
  ABSL_ASSIGN_OR_RETURN(bool exists, stmt->Step());
  ABSL_RETURN_IF_ERROR(stmt->Reset());
  return exists;
}

absl::Status CreateSchema(sqlite3::Connection &db, const RootIdentity &root) {
  ABSL_RETURN_IF_ERROR(db.ExecScript(kSchemaSql));

  std::string device_id_bytes = root.device_id.Serialize();
  ABSL_ASSIGN_OR_RETURN(
      sqlite3::Statement * state_stmt,
      db.Prepared("INSERT INTO cache_state "
                  "(id, schema_version, source_device_id, clean_shutdown, "
                  " boot_id) VALUES (1, ?, ?, 1, NULL)"));
  ABSL_RETURN_IF_ERROR(state_stmt->Bind(1, kSchemaVersion));
  ABSL_RETURN_IF_ERROR(state_stmt->Bind(2, AsBlob(device_id_bytes)));
  ABSL_RETURN_IF_ERROR(state_stmt->ExecuteOnce());

  ABSL_ASSIGN_OR_RETURN(
      sqlite3::Statement * fs_stmt,
      db.Prepared("INSERT INTO filesystems "
                   "(device_id, fstype, parent_inode, boundary_name) "
                   "VALUES (?, ?, NULL, NULL)"));
  ABSL_RETURN_IF_ERROR(fs_stmt->Bind(1, AsBlob(device_id_bytes)));
  ABSL_RETURN_IF_ERROR(fs_stmt->Bind(2, root.fstype));
  ABSL_RETURN_IF_ERROR(fs_stmt->ExecuteOnce());

  ABSL_ASSIGN_OR_RETURN(
      sqlite3::Statement * inode_stmt,
      db.Prepared(
          "INSERT INTO inodes "
          "(id, device_id, backing_ino, backing_gen, fuse_gen, "
          " attrs_valid) "
          "VALUES (1, ?, ?, ?, 0, 0)"));
  ABSL_RETURN_IF_ERROR(inode_stmt->Bind(1, AsBlob(device_id_bytes)));
  ABSL_RETURN_IF_ERROR(inode_stmt->Bind(2, root.backing_ino));
  ABSL_RETURN_IF_ERROR(inode_stmt->Bind(3, root.backing_gen));
  ABSL_RETURN_IF_ERROR(inode_stmt->ExecuteOnce());

  ABSL_ASSIGN_OR_RETURN(
      sqlite3::Statement * dir_stmt,
      db.Prepared(
          "INSERT INTO directories (inode, children_complete) "
          "VALUES (1, 0)"));
  return dir_stmt->ExecuteOnce();
}

// v1 -> v2:
//  - The untyped key/value `meta` table becomes the typed single-row
//    cache_state: schema_version and source_device_id are copied over (v1
//    stored the device id's bytes as TEXT; CAST keeps them byte for byte),
//    clean_shutdown starts at 1 and boot_id NULL (a v1 daemon tracked
//    neither, and nothing is dirty), and v1's gen_counter is dropped:
//  - FUSE generations become random per row (cache::UpsertInode); existing
//    rows keep the generations they were given.
//  - The durable dirty set appears, empty.
//  - dentries rows gain an explicit state (step 4.13): a v1 row with an
//    inode is 'present', one without is 'absent' (v1 predates refused
//    boundaries, amendment 12, and unknown rows); rebuilt (keeping rowids,
//    the readdir cursors) since the CHECKs and the inode column's
//    ON DELETE SET NULL cannot be changed in place, which the
//    inodes_delete_unknowns trigger replaces (audit F8).
//  - directories gains `epoch` (step 4.12), starting at 0.
//  - xattrs rows gain their state; every v1 row held a value, so is
//    'present'. (SQLite cannot add a table CHECK in place: rebuild.)
absl::Status MigrateV1ToV2(sqlite3::Connection &db) {
  // As in schema.sql.
  ABSL_RETURN_IF_ERROR(db.ExecScript(R"sql(
    CREATE TABLE cache_state (
      id INTEGER PRIMARY KEY CHECK (id = 1),
      schema_version INTEGER NOT NULL,
      source_device_id BLOB NOT NULL,
      clean_shutdown INTEGER NOT NULL,
      boot_id TEXT NULL
    ) STRICT;
    INSERT INTO cache_state
      (id, schema_version, source_device_id, clean_shutdown, boot_id)
      SELECT 1, 2, CAST(value AS BLOB), 1, NULL
      FROM meta WHERE key = 'source_device_id';
    DROP TABLE meta;
    CREATE TABLE dirty (inode INTEGER PRIMARY KEY) STRICT;
    CREATE TABLE dentries_v2 (
      parent INTEGER NOT NULL REFERENCES inodes (id) ON DELETE CASCADE,
      name BLOB NOT NULL,
      state TEXT NOT NULL
          CHECK (state IN ('present', 'absent', 'unknown', 'refused')),
      inode INTEGER NULL REFERENCES inodes (id),
      CHECK ((state = 'present') = (inode IS NOT NULL)),
      PRIMARY KEY (parent, name)
    ) STRICT;
    INSERT INTO dentries_v2 (rowid, parent, name, state, inode)
      SELECT rowid, parent, name,
             CASE WHEN inode IS NULL THEN 'absent' ELSE 'present' END, inode
      FROM dentries;
    DROP TABLE dentries;
    ALTER TABLE dentries_v2 RENAME TO dentries;
    CREATE INDEX dentries_inode ON dentries (inode);
    CREATE TRIGGER inodes_delete_unknowns BEFORE DELETE ON inodes BEGIN
      UPDATE dentries SET state = 'unknown', inode = NULL
      WHERE inode = OLD.id;
    END;
    ALTER TABLE directories ADD COLUMN epoch INTEGER NOT NULL DEFAULT 0;
    CREATE TABLE xattrs_v2 (
      inode INTEGER NOT NULL REFERENCES inodes (id) ON DELETE CASCADE,
      name BLOB NOT NULL,
      state TEXT NOT NULL CHECK (state IN ('present', 'absent', 'unknown')),
      value BLOB NULL,
      CHECK ((state = 'present') = (value IS NOT NULL)),
      PRIMARY KEY (inode, name)
    ) STRICT;
    INSERT INTO xattrs_v2 (inode, name, state, value)
      SELECT inode, name, 'present', value FROM xattrs;
    DROP TABLE xattrs;
    ALTER TABLE xattrs_v2 RENAME TO xattrs;
  )sql"));
  ABSL_ASSIGN_OR_RETURN(int version, GetSchemaVersion(db));
  RET_CHECK_EQ(version, 2) << "v1 meta.source_device_id is missing";
  return absl::OkStatus();
}

// v2 -> v3 (Phase 6.2): the partial indexes that make a readdir request's
// queries (cache::ListDir, cache::IsDirComplete) cost a page instead of a
// whole directory; see schema.sql. No row changes.
absl::Status MigrateV2ToV3(sqlite3::Connection &db) {
  ABSL_RETURN_IF_ERROR(db.ExecScript(R"sql(
    CREATE INDEX dentries_present ON dentries (parent) WHERE state = 'present';
    CREATE INDEX dentries_unknown ON dentries (parent) WHERE state = 'unknown';
    UPDATE cache_state SET schema_version = 3 WHERE id = 1;
  )sql"));
  ABSL_ASSIGN_OR_RETURN(int version, GetSchemaVersion(db));
  RET_CHECK_EQ(version, 3);
  return absl::OkStatus();
}

// v3 -> v4 (step 23.5): boundary stubs. A refused dentry is now served as
// a stub directory, whose nodeid and attributes live in `stubs`; an older
// cache's refused dentries have none, so they become unknown (the
// tri-state rule: never absent) and the next lookup or listing probes them
// again and records their stubs. IF NOT EXISTS: a test that makes a v2 or v3
// database from a fresh one undoes only what those steps add.
absl::Status MigrateV3ToV4(sqlite3::Connection &db) {
  ABSL_RETURN_IF_ERROR(db.ExecScript(R"sql(
    CREATE INDEX IF NOT EXISTS dentries_refused ON dentries (parent)
        WHERE state = 'refused';
    CREATE TABLE IF NOT EXISTS stubs (
      id INTEGER PRIMARY KEY CHECK (id < 0),
      parent INTEGER NOT NULL REFERENCES inodes (id) ON DELETE CASCADE,
      name BLOB NOT NULL,
      fuse_gen INTEGER NOT NULL,
      mode INTEGER NOT NULL,
      nlink INTEGER NOT NULL,
      uid INTEGER NOT NULL,
      gid INTEGER NOT NULL,
      rdev INTEGER NOT NULL,
      size INTEGER NOT NULL,
      blocks INTEGER NOT NULL,
      blksize INTEGER NOT NULL,
      atime_s INTEGER NOT NULL,
      atime_ns INTEGER NOT NULL,
      mtime_s INTEGER NOT NULL,
      mtime_ns INTEGER NOT NULL,
      ctime_s INTEGER NOT NULL,
      ctime_ns INTEGER NOT NULL,
      btime_s INTEGER NOT NULL,
      btime_ns INTEGER NOT NULL,
      UNIQUE (parent, name)
    ) STRICT;
    CREATE TRIGGER IF NOT EXISTS dentries_unrefused
        AFTER UPDATE OF state ON dentries
        WHEN OLD.state = 'refused' AND NEW.state != 'refused' BEGIN
      DELETE FROM stubs WHERE parent = OLD.parent AND name = OLD.name;
    END;
    CREATE TRIGGER IF NOT EXISTS dentries_refused_deleted
        AFTER DELETE ON dentries WHEN OLD.state = 'refused' BEGIN
      DELETE FROM stubs WHERE parent = OLD.parent AND name = OLD.name;
    END;
    UPDATE dentries SET state = 'unknown' WHERE state = 'refused';
    UPDATE cache_state SET schema_version = 4 WHERE id = 1;
  )sql"));
  ABSL_ASSIGN_OR_RETURN(int version, GetSchemaVersion(db));
  RET_CHECK_EQ(version, 4);
  return absl::OkStatus();
}

// v4 -> v5 (step 12.4b): the stub nodeids' high-water mark, from the stubs
// there are (an id that went before cannot be known: one may come back
// once, as it could before), the triggers that keep a forgotten refusal's
// stub, and the partial index of rows with nlink 0 (the sweep at every
// start). The column only if missing: a test that makes an older
// database from a fresh one may keep it.
absl::Status MigrateV4ToV5(sqlite3::Connection &db) {
  ABSL_ASSIGN_OR_RETURN(
      sqlite3::Statement * column,
      db.Prepared("SELECT 1 FROM pragma_table_info('cache_state') "
                  "WHERE name = 'last_stub_id'"));
  ABSL_ASSIGN_OR_RETURN(bool has_column, column->Step());
  ABSL_RETURN_IF_ERROR(column->Reset());
  if (!has_column) {
    ABSL_RETURN_IF_ERROR(db.Exec(
        "ALTER TABLE cache_state ADD COLUMN last_stub_id INTEGER NULL"));
  }
  ABSL_RETURN_IF_ERROR(db.ExecScript(R"sql(
    UPDATE cache_state SET last_stub_id = (SELECT MAX(id) FROM stubs)
        WHERE id = 1;
    CREATE INDEX IF NOT EXISTS inodes_unlinked ON inodes (id)
        WHERE nlink = 0;
    DROP TRIGGER IF EXISTS dentries_unrefused;
    DROP TRIGGER IF EXISTS dentries_refused_deleted;
    CREATE TRIGGER dentries_unrefused AFTER UPDATE OF state ON dentries
        WHEN OLD.state IN ('refused', 'unknown')
             AND NEW.state IN ('present', 'absent') BEGIN
      DELETE FROM stubs WHERE parent = OLD.parent AND name = OLD.name;
    END;
    CREATE TRIGGER dentries_refused_deleted AFTER DELETE ON dentries
        WHEN OLD.state IN ('refused', 'unknown') BEGIN
      DELETE FROM stubs WHERE parent = OLD.parent AND name = OLD.name;
    END;
    UPDATE cache_state SET schema_version = 5 WHERE id = 1;
  )sql"));
  ABSL_ASSIGN_OR_RETURN(int version, GetSchemaVersion(db));
  RET_CHECK_EQ(version, 5);
  return absl::OkStatus();
}

// Upgrades an existing database, one version at a time, to kSchemaVersion,
// in one transaction. A version newer than this build's is refused.
absl::Status UpgradeSchema(sqlite3::Connection &db) {
  return db.Transaction([&]() -> absl::Status {
    ABSL_ASSIGN_OR_RETURN(int version, ExistingSchemaVersion(db));
    if (version < 1 || version > kSchemaVersion) {
      return FailedPreconditionErrorBuilder()
             << "dcfs cache schema version mismatch: found " << version
             << ", this build understands 1 through " << kSchemaVersion;
    }
    if (version == 1) {
      ABSL_RETURN_IF_ERROR(MigrateV1ToV2(db));
      version = 2;
    }
    if (version == 2) {
      ABSL_RETURN_IF_ERROR(MigrateV2ToV3(db));
      version = 3;
    }
    if (version == 3) {
      ABSL_RETURN_IF_ERROR(MigrateV3ToV4(db));
      version = 4;
    }
    if (version == 4) {
      ABSL_RETURN_IF_ERROR(MigrateV4ToV5(db));
      version = 5;
    }
    RET_CHECK_EQ(version, kSchemaVersion);
    return absl::OkStatus();
  });
}

absl::Status ValidateExistingSchema(sqlite3::Connection &db) {
  ABSL_RETURN_IF_ERROR(UpgradeSchema(db));

  ABSL_ASSIGN_OR_RETURN(bool root_exists, RootInodeExists(db));
  if (!root_exists) {
    return FailedPreconditionErrorBuilder()
           << "corrupt cache: root inode (id 1) is missing";
  }

  return absl::OkStatus();
}

}  // namespace

absl::Status Migrate(sqlite3::Connection &db, const RootIdentity &root) {
  ABSL_ASSIGN_OR_RETURN(bool has_state, TableExists(db, "cache_state"));
  ABSL_ASSIGN_OR_RETURN(bool has_v1_meta, TableExists(db, "meta"));
  if (!has_state && !has_v1_meta) {
    return db.Transaction(
        [&]() -> absl::Status { return CreateSchema(db, root); });
  }
  return ValidateExistingSchema(db);
}

absl::StatusOr<int> GetSchemaVersion(sqlite3::Connection &db) {
  return ReadCacheState<int>(
      db, "SELECT schema_version FROM cache_state WHERE id = 1",
      [](sqlite3::Statement &row) { return row.Column<int>(0); });
}

absl::StatusOr<DeviceId> GetSourceDeviceId(sqlite3::Connection &db) {
  ABSL_ASSIGN_OR_RETURN(
      std::string bytes,
      ReadCacheState<std::string>(
          db, "SELECT source_device_id FROM cache_state WHERE id = 1",
          [](sqlite3::Statement &row) {
            std::vector<uint8_t> blob = row.Column<std::vector<uint8_t>>(0);
            return std::string(blob.begin(), blob.end());
          }));
  return DeviceId::Parse(bytes);
}

absl::StatusOr<bool> GetCleanShutdown(sqlite3::Connection &db) {
  return ReadCacheState<bool>(
      db, "SELECT clean_shutdown FROM cache_state WHERE id = 1",
      [](sqlite3::Statement &row) { return row.Column<bool>(0); });
}

absl::Status SetCleanShutdown(sqlite3::Connection &db, bool clean) {
  ABSL_ASSIGN_OR_RETURN(
      sqlite3::Statement * stmt,
      db.Prepared("UPDATE cache_state SET clean_shutdown = ? WHERE id = 1"));
  ABSL_RETURN_IF_ERROR(stmt->Bind(1, clean));
  ABSL_RETURN_IF_ERROR(stmt->ExecuteOnce());
  RET_CHECK_EQ(db.Changes(), 1) << "the cache_state row is missing";
  return absl::OkStatus();
}

absl::StatusOr<std::optional<std::string>> GetBootId(sqlite3::Connection &db) {
  return ReadCacheState<std::optional<std::string>>(
      db, "SELECT boot_id FROM cache_state WHERE id = 1",
      [](sqlite3::Statement &row) {
        return row.Column<std::optional<std::string>>(0);
      });
}

absl::Status SetBootId(sqlite3::Connection &db, std::string_view boot_id) {
  ABSL_ASSIGN_OR_RETURN(
      sqlite3::Statement * stmt,
      db.Prepared("UPDATE cache_state SET boot_id = ? WHERE id = 1"));
  ABSL_RETURN_IF_ERROR(stmt->Bind(1, boot_id));
  ABSL_RETURN_IF_ERROR(stmt->ExecuteOnce());
  RET_CHECK_EQ(db.Changes(), 1) << "the cache_state row is missing";
  return absl::OkStatus();
}

}  // namespace dcfs
