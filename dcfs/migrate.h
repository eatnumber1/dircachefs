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
// v1: the original schema: cache-wide state in a key/value `meta` table,
//     FUSE generations from its gen_counter.
// v2: the typed single-row `cache_state` table replaces `meta`; random FUSE
//     generations (gen_counter gone); the durable dirty set (table `dirty`,
//     cache_state.clean_shutdown and boot_id); an explicit per-name
//     present/absent/unknown state on xattrs rows; the directories
//     completeness epoch; explicit present/absent/unknown/refused states
//     on dentries rows, and the inodes_delete_unknowns trigger.
// v3: the partial indexes dentries_present and dentries_unknown (readdir
//     costs a page of rows per query, not a whole directory).
// v4: boundary stubs (step 23.5): the `stubs` table, its triggers and the
//     partial index dentries_refused; refused dentries of an older cache
//     become unknown (they had no stub, and are probed again).
inline constexpr int kSchemaVersion = 4;

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
// If `db` is a fresh database (neither a `cache_state` nor a v1 `meta`
// table), this runs schema.sql and seeds it, all inside a single
// transaction:
//   - the cache_state row: schema_version = kSchemaVersion,
//     source_device_id = root.device_id.Serialize(), clean_shutdown 1,
//     boot_id NULL.
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

// --- cache_state accessors ------------------------------------------------
//
// Typed access to the one cache_state row (see schema.sql for what each
// column means and when it is written). The schema must already exist
// (Migrate() succeeded); a missing row is FailedPrecondition (corrupt
// cache).

absl::StatusOr<int> GetSchemaVersion(sqlite3::Connection &db);

// cache_state.source_device_id, parsed back into a DeviceId.
absl::StatusOr<DeviceId> GetSourceDeviceId(sqlite3::Connection &db);

// cache_state.clean_shutdown: whether the last run shut down cleanly (see
// backing::StartRun/FinishRun).
absl::StatusOr<bool> GetCleanShutdown(sqlite3::Connection &db);
absl::Status SetCleanShutdown(sqlite3::Connection &db, bool clean);

// cache_state.boot_id: /proc/sys/kernel/random/boot_id as of the last
// start; nullopt before the first.
absl::StatusOr<std::optional<std::string>> GetBootId(sqlite3::Connection &db);
absl::Status SetBootId(sqlite3::Connection &db, std::string_view boot_id);

}  // namespace dcfs

#endif  // DCFS_MIGRATE_H_
