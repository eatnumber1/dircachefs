#include "dcfs/migrate.h"

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>

#include "absl/random/random.h"
#include "absl/status/status.h"
#include "absl/status/status_macros.h"
#include "absl/status/statusor.h"
#include "absl/strings/numbers.h"
#include "absl/strings/str_cat.h"
#include "dcfs/device_id.h"
#include "dcfs/ret_check.h"
#include "dcfs/schema.h"
#include "dcfs/sqlite.h"

namespace dcfs {

namespace {

constexpr std::string_view kKeySchemaVersion = "schema_version";
constexpr std::string_view kKeyGenCounter = "gen_counter";
constexpr std::string_view kKeySourceDeviceId = "source_device_id";

// The reserved FUSE generation for the root inode (see schema.sql); minting
// skips this value.
constexpr uint32_t kRootFuseGeneration = 0;

// Views the bytes of `s` as a blob for Statement::Bind(). Several schema
// columns that hold raw bytes (e.g. filesystems.device_id) are declared
// BLOB in a STRICT table, which -- unlike an ordinary table -- rejects a
// TEXT value outright rather than coercing it, so these must go through
// the span<uint8_t> Bind() overload rather than the string_view one.
std::span<const uint8_t> AsBlob(const std::string &s) {
  return std::span<const uint8_t>(
      reinterpret_cast<const uint8_t *>(s.data()), s.size());
}

absl::StatusOr<bool> MetaTableExists(sqlite3::Connection &db) {
  ABSL_ASSIGN_OR_RETURN(
      sqlite3::Statement * stmt,
      db.Prepared("SELECT 1 FROM sqlite_master "
                   "WHERE type = 'table' AND name = 'meta'"));
  ABSL_ASSIGN_OR_RETURN(bool exists, stmt->Step());
  ABSL_RETURN_IF_ERROR(stmt->Reset());
  return exists;
}

absl::StatusOr<bool> RootInodeExists(sqlite3::Connection &db) {
  ABSL_ASSIGN_OR_RETURN(sqlite3::Statement * stmt,
                         db.Prepared("SELECT 1 FROM inodes WHERE id = 1"));
  ABSL_ASSIGN_OR_RETURN(bool exists, stmt->Step());
  ABSL_RETURN_IF_ERROR(stmt->Reset());
  return exists;
}

// Parses a meta value previously written as an unsigned 32-bit decimal
// string (gen_counter).
absl::StatusOr<uint32_t> ParseU32Meta(std::string_view key,
                                       const std::string &value) {
  uint32_t parsed = 0;
  RET_CHECK(absl::SimpleAtoi(value, &parsed))
      << "meta." << key << " is not a valid uint32: " << value;
  return parsed;
}

absl::Status CreateSchema(sqlite3::Connection &db, const RootIdentity &root) {
  ABSL_RETURN_IF_ERROR(db.ExecScript(kSchemaSql));

  ABSL_RETURN_IF_ERROR(
      SetMeta(db, kKeySchemaVersion, absl::StrCat(kSchemaVersion)));

  absl::BitGen bitgen;
  uint32_t gen_counter = absl::Uniform<uint32_t>(bitgen);
  ABSL_RETURN_IF_ERROR(
      SetMeta(db, kKeyGenCounter, absl::StrCat(gen_counter)));

  std::string device_id_bytes = root.device_id.Serialize();
  ABSL_RETURN_IF_ERROR(SetMeta(db, kKeySourceDeviceId, device_id_bytes));

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

absl::Status ValidateExistingSchema(sqlite3::Connection &db) {
  ABSL_ASSIGN_OR_RETURN(int version, GetSchemaVersion(db));
  if (version != kSchemaVersion) {
    return absl::FailedPreconditionError(absl::StrCat(
        "dcfs cache schema version mismatch: found ", version,
        ", this build expects ", kSchemaVersion,
        " (no upgrade path yet)"));
  }

  ABSL_ASSIGN_OR_RETURN(bool root_exists, RootInodeExists(db));
  if (!root_exists) {
    return absl::FailedPreconditionError(
        "corrupt cache: root inode (id 1) is missing");
  }
  return absl::OkStatus();
}

}  // namespace

absl::Status Migrate(sqlite3::Connection &db, const RootIdentity &root) {
  ABSL_ASSIGN_OR_RETURN(bool has_meta, MetaTableExists(db));
  if (!has_meta) {
    return db.Transaction(
        [&]() -> absl::Status { return CreateSchema(db, root); });
  }
  return ValidateExistingSchema(db);
}

absl::StatusOr<std::optional<std::string>> GetMeta(sqlite3::Connection &db,
                                                     std::string_view key) {
  ABSL_ASSIGN_OR_RETURN(sqlite3::Statement * stmt,
                         db.Prepared("SELECT value FROM meta WHERE key = ?"));
  ABSL_RETURN_IF_ERROR(stmt->Bind(1, key));
  ABSL_ASSIGN_OR_RETURN(bool has_row, stmt->Step());
  std::optional<std::string> result;
  if (has_row) result = stmt->Column<std::string>(0);
  ABSL_RETURN_IF_ERROR(stmt->Reset());
  return result;
}

absl::Status SetMeta(sqlite3::Connection &db, std::string_view key,
                      std::string_view value) {
  ABSL_ASSIGN_OR_RETURN(
      sqlite3::Statement * stmt,
      db.Prepared("INSERT INTO meta (key, value) VALUES (?, ?) "
                   "ON CONFLICT (key) DO UPDATE SET value = excluded.value"));
  ABSL_RETURN_IF_ERROR(stmt->BindAll(key, value));
  return stmt->ExecuteOnce();
}

absl::StatusOr<DeviceId> GetSourceDeviceId(sqlite3::Connection &db) {
  ABSL_ASSIGN_OR_RETURN(std::optional<std::string> value,
                         GetMeta(db, kKeySourceDeviceId));
  RET_CHECK(value.has_value()) << "meta.source_device_id is missing";
  return DeviceId::Parse(*value);
}

absl::StatusOr<int> GetSchemaVersion(sqlite3::Connection &db) {
  ABSL_ASSIGN_OR_RETURN(std::optional<std::string> value,
                         GetMeta(db, kKeySchemaVersion));
  RET_CHECK(value.has_value()) << "meta.schema_version is missing";
  int version = 0;
  RET_CHECK(absl::SimpleAtoi(*value, &version))
      << "meta.schema_version is not a valid integer: " << *value;
  return version;
}

absl::StatusOr<uint32_t> MintFuseGeneration(sqlite3::Connection &db) {
  RET_CHECK(db.InTransaction())
      << "MintFuseGeneration must be called inside a transaction";

  ABSL_ASSIGN_OR_RETURN(std::optional<std::string> value,
                         GetMeta(db, kKeyGenCounter));
  RET_CHECK(value.has_value()) << "meta.gen_counter is missing";
  ABSL_ASSIGN_OR_RETURN(uint32_t current,
                         ParseU32Meta(kKeyGenCounter, *value));

  uint32_t next = current + 1;  // wraps mod 2^32 on overflow.
  if (next == kRootFuseGeneration) next = kRootFuseGeneration + 1;

  ABSL_RETURN_IF_ERROR(SetMeta(db, kKeyGenCounter, absl::StrCat(next)));
  return next;
}

}  // namespace dcfs
