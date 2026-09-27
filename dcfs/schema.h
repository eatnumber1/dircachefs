#ifndef DCFS_SCHEMA_H_
#define DCFS_SCHEMA_H_

namespace dcfs {

// The dcfs schema (dcfs/schema.sql), embedded verbatim at build time by a
// genrule (see dcfs/BUILD.bazel's :schema_sql_cc). See dcfs/migrate.h for
// how it's applied to a fresh database.
extern const char kSchemaSql[];

}  // namespace dcfs

#endif  // DCFS_SCHEMA_H_
