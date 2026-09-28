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

// The schema version this build of dcfs creates. Bump when schema.sql
// changes in a way that requires a migration, and add the upgrade step from
// the previous version to Migrate() (migrate.cc UpgradeSchema).
//
// v1: the original schema (FUSE generations from meta.gen_counter).
// v2: random FUSE generations (meta.gen_counter dropped); the durable dirty
//     set (table `dirty`, meta.clean_shutdown, meta.boot_id).
inline constexpr int kSchemaVersion = 2;

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
//   - meta rows: schema_version = kSchemaVersion, source_device_id =
//     root.device_id.Serialize().
//   - the source filesystems row (parent_inode/boundary_name NULL).
//   - the root inodes row: id = 1 (FUSE_ROOT_ID), fuse_gen = 0,
//     backing_ino/backing_gen from `root`, attrs_valid 0.
//   - the root directories row (children_complete 0).
//
// If `db` already has a schema, this instead upgrades it to kSchemaVersion
// (one transaction, one version step at a time; a version newer than this
// build's, or garbage, is absl::FailedPreconditionError naming both) and
// validates it: the root inode row (id 1) must exist (else
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
// specific keys used are schema_version and source_device_id, exposed
// below via typed wrappers.

// meta keys of the durable dirty set's unclean-shutdown detection (see
// backing::StartRun/FinishRun): "1" once a run has shut down cleanly
// (backing filesystems synced, dirty set empty), "0" while one is running;
// and /proc/sys/kernel/random/boot_id as of the last start, so a machine
// crash can be told from a daemon crash in the log.
inline constexpr std::string_view kMetaCleanShutdown = "clean_shutdown";
inline constexpr std::string_view kMetaBootId = "boot_id";

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

}  // namespace dcfs

#endif  // DCFS_MIGRATE_H_
