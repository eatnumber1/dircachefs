#ifndef DCFS_MIGRATE_H_
#define DCFS_MIGRATE_H_

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "dcfs/device_id.h"
#include "dcfs/sqlite.h"

namespace dcfs {

// The schema version this build of dcfs understands. Bump when schema.sql
// changes in a way that requires a migration -- future upgrade steps go in
// Migrate() below, gated on the stored value being less than this.
inline constexpr int kSchemaVersion = 1;

// Identifies the root of the cache: the backing filesystem being cached,
// and the backing (ino, generation) of its root directory. Only consulted
// the first time a cache database is created -- see Migrate().
struct RootIdentity {
  DeviceId device_id;
  int64_t fstype = 0;
  uint64_t backing_ino = 0;
  uint64_t backing_gen = 0;
};

// Creates or validates the dcfs schema in `db`.
//
// If `db` is a fresh database (no `meta` table), this runs schema.sql and
// seeds it, all inside a single transaction:
//   - meta rows: schema_version = kSchemaVersion, gen_counter (randomly
//     seeded), source_device_id = root.device_id.Serialize().
//   - the source filesystems row (parent_inode/boundary_name NULL).
//   - the root inodes row: id = 1 (FUSE_ROOT_ID), fuse_gen = 0,
//     backing_ino/backing_gen from `root`, attrs_valid 0.
//   - the root directories row (children_complete 0).
//
// If `db` already has a schema, this instead validates it: the stored
// schema_version must equal kSchemaVersion (there is no upgrade path yet;
// returns absl::FailedPreconditionError naming both versions if it
// doesn't), and the root inode row (id 1) must exist (else
// absl::FailedPreconditionError: corrupt cache). Calling Migrate() again on
// an already-migrated, uncorrupted database is a no-op.
//
// Deliberately does NOT compare `root.device_id` against the stored
// source_device_id -- callers that need to refuse a cache built against a
// different source filesystem should do that themselves, via
// GetSourceDeviceId(), so they can produce a clearer error message than
// this generic function could.
absl::Status Migrate(sqlite3::Connection &db, const RootIdentity &root);

// --- meta table accessors -------------------------------------------------
//
// `meta` is a small key/value store for cache-wide state that doesn't fit
// anywhere else (see schema.sql). These are generic accessors; the
// specific keys used are schema_version, gen_counter and
// source_device_id, exposed above/below via typed wrappers.

// Returns the value stored for `key`, or nullopt if there is no such row.
absl::StatusOr<std::optional<std::string>> GetMeta(sqlite3::Connection &db,
                                                     std::string_view key);

// Upserts `key` = `value`.
absl::Status SetMeta(sqlite3::Connection &db, std::string_view key,
                      std::string_view value);

// Returns meta.source_device_id, parsed back into a DeviceId. The schema
// must already exist (i.e. Migrate() must have been called successfully).
absl::StatusOr<DeviceId> GetSourceDeviceId(sqlite3::Connection &db);

// Returns meta.schema_version. The schema must already exist.
absl::StatusOr<int> GetSchemaVersion(sqlite3::Connection &db);

// Mints the next FUSE generation number: reads meta.gen_counter, increments
// it modulo 2^32 skipping the value 0 (0 is reserved for the root inode),
// writes the new value back, and returns it. Successive calls (without an
// intervening rollback) return strictly increasing values, except across
// the 2^32 wraparound.
//
// The caller must already be inside a transaction (see
// Connection::Transaction): minting a generation is only meaningful
// together with whatever row update uses it, and both must commit or roll
// back together.
absl::StatusOr<uint32_t> MintFuseGeneration(sqlite3::Connection &db);

}  // namespace dcfs

#endif  // DCFS_MIGRATE_H_
