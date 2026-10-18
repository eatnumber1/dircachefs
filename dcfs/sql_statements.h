// SPDX-License-Identifier: MIT
#ifndef DCFS_SQL_STATEMENTS_H_
#define DCFS_SQL_STATEMENTS_H_

#include <array>
#include <span>
#include <string_view>

namespace dcfs {

// Macro fragments for attribute columns, matching the order BindAttrs binds
// them.
#define DCFS_ATTR_VALUES                                                \
  "mode = ?, nlink = ?, uid = ?, gid = ?, rdev = ?, "                   \
  "size = ?, blocks = ?, blksize = ?, atime_s = ?, atime_ns = ?, "      \
  "mtime_s = ?, mtime_ns = ?, ctime_s = ?, ctime_ns = ?, btime_s = ?, " \
  "btime_ns = ?"

#define DCFS_KEEP_STAMP                                                    \
  "(mode & 61440) IN (16384, 40960) AND (mode & 61440) = (:mode & 61440) " \
  "AND (atime_s > :atime_s OR (atime_s = :atime_s AND atime_ns > :atime_ns))"

#define DCFS_ATTR_ASSIGNMENTS                                           \
  "attrs_valid = 1, "                                                   \
  "mode = :mode, nlink = ?, uid = ?, gid = ?, rdev = ?, "               \
  "size = ?, blocks = ?, blksize = ?, "                                 \
  "atime_s = CASE WHEN " DCFS_KEEP_STAMP                                \
  " THEN atime_s ELSE :atime_s END, "                                   \
  "atime_ns = CASE WHEN " DCFS_KEEP_STAMP                               \
  " THEN atime_ns ELSE :atime_ns "                                      \
  "END, "                                                               \
  "mtime_s = ?, mtime_ns = ?, ctime_s = ?, ctime_ns = ?, btime_s = ?, " \
  "btime_ns = ?"

#define DCFS_ATTR_COLUMNS                                                   \
  "mode, nlink, uid, gid, rdev, size, blocks, blksize, atime_s, atime_ns, " \
  "mtime_s, mtime_ns, ctime_s, ctime_ns, btime_s, btime_ns"

#define DCFS_ATTR_NULLS                                                      \
  "NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, " \
  "NULL, NULL, NULL, NULL"

#define DCFS_ATTR_PLACEHOLDERS "?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?"

inline constexpr int kNumAttrColumns = 16;

// SQL statements prepared by dcfs/metadata_cache.cc.
inline constexpr std::string_view kSqlRequireInode =
    "SELECT 1 FROM inodes WHERE id = ?";

inline constexpr std::string_view kSqlMarkIncomplete =
    "UPDATE directories SET children_complete = 0, "
    "epoch = epoch + 1 WHERE inode = ?";

inline constexpr std::string_view kSqlLookup =
    "SELECT d.state, d.inode, s.id FROM dentries d "
    "LEFT JOIN stubs s ON s.parent = d.parent AND s.name = d.name "
    "WHERE d.parent = ? AND d.name = ?";

inline constexpr std::string_view kSqlGetAttr =
    "SELECT attrs_valid, fuse_gen, device_id, backing_ino, "
    "backing_gen, " DCFS_ATTR_COLUMNS " FROM inodes WHERE id = ?";

inline constexpr std::string_view kSqlGetStub =
    "SELECT parent, name, fuse_gen, " DCFS_ATTR_COLUMNS
    " FROM stubs WHERE id = ?";

inline constexpr std::string_view kSqlGetGeneration =
    "SELECT fuse_gen FROM inodes WHERE id = ?";

inline constexpr std::string_view kSqlListDir =
    "SELECT d.rowid, d.name, d.inode, i.attrs_valid, i.fuse_gen, "
    "i.device_id, i.backing_ino, i.backing_gen, " DCFS_ATTR_COLUMNS
    " FROM dentries d JOIN inodes i ON i.id = d.inode "
    "WHERE d.parent = ?1 AND d.rowid > ?2 AND d.state = 'present' "
    "UNION ALL "
    "SELECT d.rowid, d.name, s.id, NULL, NULL, NULL, NULL, "
    "NULL, " DCFS_ATTR_NULLS
    " FROM dentries d JOIN stubs s ON s.parent = d.parent AND s.name = d.name "
    "WHERE d.parent = ?1 AND d.rowid > ?2 AND d.state = 'refused' "
    "ORDER BY 1 LIMIT ?3";

inline constexpr std::string_view kSqlChildrenComplete =
    "SELECT children_complete FROM directories WHERE inode = ?";

inline constexpr std::string_view kSqlIsDirComplete =
    "SELECT children_complete AND NOT EXISTS ("
    "SELECT 1 FROM dentries WHERE parent = ?1 AND state = 'unknown') "
    "FROM directories WHERE inode = ?1";

inline constexpr std::string_view kSqlReadLink =
    "SELECT target FROM symlinks WHERE inode = ?";

inline constexpr std::string_view kSqlXattrsComplete =
    "SELECT xattrs_complete FROM inodes WHERE id = ?";

inline constexpr std::string_view kSqlAnyXattrUnknown =
    "SELECT 1 FROM xattrs WHERE inode = ? AND state = 'unknown' "
    "LIMIT 1";

inline constexpr std::string_view kSqlListPresentXattrs =
    "SELECT name FROM xattrs WHERE inode = ? AND state = 'present' "
    "ORDER BY name";

inline constexpr std::string_view kSqlGetXattr =
    "SELECT state, value FROM xattrs WHERE inode = ? AND name = ?";

inline constexpr std::string_view kSqlGetHandle =
    "SELECT device_id, handle_type, handle FROM inodes WHERE id = ?";

inline constexpr std::string_view kSqlPresentDentryByInode =
    "SELECT parent, name FROM dentries WHERE inode = ? AND "
    "state = 'present' LIMIT 1";

inline constexpr std::string_view kSqlSelectUnknownDentries =
    "SELECT parent, name FROM dentries WHERE state = 'unknown' "
    "LIMIT ?";

inline constexpr std::string_view kSqlParentOf =
    "SELECT d.parent FROM inodes i LEFT JOIN dentries d "
    "ON d.inode = i.id AND d.state = 'present' WHERE i.id = ? "
    "LIMIT 2";

inline constexpr std::string_view kSqlListFilesystems =
    "SELECT device_id, fstype, parent_inode, boundary_name "
    "FROM filesystems ORDER BY rowid";

inline constexpr std::string_view kSqlGetFilesystem =
    "SELECT device_id, fstype, parent_inode, boundary_name "
    "FROM filesystems WHERE device_id = ?";

inline constexpr std::string_view kSqlFindMatchingInodes =
    "SELECT id, fuse_gen, backing_gen, handle_type, handle, "
    "btime_s, btime_ns FROM inodes "
    "WHERE device_id = ? AND backing_ino = ?";

inline constexpr std::string_view kSqlUpdateMatchingRow =
    "UPDATE inodes SET handle_type = ?, handle = "
    "?, " DCFS_ATTR_ASSIGNMENTS " WHERE id = ?";

inline constexpr std::string_view kSqlInsertNewRow =
    "INSERT INTO inodes (device_id, backing_ino, "
    "backing_gen, fuse_gen, handle_type, handle, "
    "attrs_valid, " DCFS_ATTR_COLUMNS
    ") VALUES (?, ?, ?, ?, ?, ?, 1, " DCFS_ATTR_PLACEHOLDERS ")";

inline constexpr std::string_view kSqlReinstateInode =
    "INSERT INTO inodes (id, device_id, backing_ino, "
    "backing_gen, fuse_gen, handle_type, handle, "
    "attrs_valid, " DCFS_ATTR_COLUMNS
    ") VALUES (?, ?, ?, ?, ?, ?, ?, 1, " DCFS_ATTR_PLACEHOLDERS ")";

inline constexpr std::string_view kSqlFilesystemExists =
    "SELECT 1 FROM filesystems WHERE device_id = ?";

inline constexpr std::string_view kSqlFindStaleRootInodes =
    "SELECT id FROM inodes "
    "WHERE device_id = ? AND backing_ino = ? AND id != ?";

inline constexpr std::string_view kSqlUpdateRoot =
    "UPDATE inodes SET backing_ino = ?, backing_gen = ?, "
    "handle_type = ?, handle = ?, " DCFS_ATTR_ASSIGNMENTS " WHERE id = ?";

inline constexpr std::string_view kSqlPutDentry =
    "INSERT INTO dentries (parent, name, state, inode) "
    "VALUES (?, ?, ?, ?) "
    "ON CONFLICT (parent, name) DO UPDATE SET "
    "state = excluded.state, inode = excluded.inode";

inline constexpr std::string_view kSqlFindStub =
    "SELECT id FROM stubs WHERE parent = ? AND name = ?";

inline constexpr std::string_view kSqlUpdateStubAttr =
    "UPDATE stubs SET " DCFS_ATTR_VALUES " WHERE id = ?";

inline constexpr std::string_view kSqlGetLastStubId =
    "SELECT last_stub_id FROM cache_state WHERE id = 1";

inline constexpr std::string_view kSqlSetLastStubId =
    "UPDATE cache_state SET last_stub_id = ? WHERE id = 1";

inline constexpr std::string_view kSqlInsertStub =
    "INSERT INTO stubs (id, parent, name, fuse_gen, " DCFS_ATTR_COLUMNS
    ") VALUES (?, ?, ?, ?, " DCFS_ATTR_PLACEHOLDERS ")";

inline constexpr std::string_view kSqlMarkDirComplete =
    "INSERT INTO directories (inode, children_complete) "
    "VALUES (?, ?) ON CONFLICT (inode) DO UPDATE SET "
    "children_complete = excluded.children_complete, "
    "epoch = epoch + (excluded.children_complete = 0)";

inline constexpr std::string_view kSqlDirEpoch =
    "SELECT epoch FROM directories WHERE inode = ?";

inline constexpr std::string_view kSqlEnsureDirectory =
    "INSERT INTO directories (inode, children_complete) "
    "VALUES (?, 0) ON CONFLICT (inode) DO NOTHING";

inline constexpr std::string_view kSqlSelectDentryNamesInParent =
    "SELECT name FROM dentries WHERE parent = ?";

inline constexpr std::string_view kSqlDeleteDentry =
    "DELETE FROM dentries WHERE parent = ? AND name = ?";

inline constexpr std::string_view kSqlDeleteAbsentDentries =
    "DELETE FROM dentries WHERE parent = ? AND state = 'absent'";

inline constexpr std::string_view kSqlRefusedDentriesToUnknown =
    "UPDATE dentries SET state = 'unknown' "
    "WHERE parent = ? AND state = 'refused'";

inline constexpr std::string_view kSqlInvalidateAttr =
    "UPDATE inodes SET attrs_valid = 0 WHERE id = ?";

inline constexpr std::string_view kSqlUpdateAttr =
    "UPDATE inodes SET " DCFS_ATTR_ASSIGNMENTS " WHERE id = ?";

inline constexpr std::string_view kSqlDropAtimeStamp =
    "UPDATE inodes SET atime_s = ?, atime_ns = 0 "
    "WHERE id = ? AND attrs_valid = 0 AND (mode & ?) IN (?, ?)";

inline constexpr std::string_view kSqlSelectTimesForTouchAtime =
    "SELECT attrs_valid, atime_s, atime_ns, mtime_s, mtime_ns, "
    "ctime_s, ctime_ns FROM inodes WHERE id = ?";

inline constexpr std::string_view kSqlUpdateAtime =
    "UPDATE inodes SET atime_s = ?, atime_ns = ? "
    "WHERE id = ? AND attrs_valid = 1 AND atime_s = ? "
    "AND atime_ns = ?";

inline constexpr std::string_view kSqlUpsertSymlink =
    "INSERT INTO symlinks (inode, target) VALUES (?, ?) "
    "ON CONFLICT (inode) DO UPDATE SET target = excluded.target";

inline constexpr std::string_view kSqlPutXattr =
    "INSERT INTO xattrs (inode, name, state, value) "
    "VALUES (?, ?, ?, ?) "
    "ON CONFLICT (inode, name) DO UPDATE SET "
    "state = excluded.state, value = excluded.value";

inline constexpr std::string_view kSqlSetXattrsComplete =
    "UPDATE inodes SET xattrs_complete = ? WHERE id = ?";

inline constexpr std::string_view kSqlDeleteAllXattrs =
    "DELETE FROM xattrs WHERE inode = ?";

inline constexpr std::string_view kSqlDeleteInode =
    "DELETE FROM inodes WHERE id = ?";

inline constexpr std::string_view kSqlInsertFilesystem =
    "INSERT INTO filesystems "
    "(device_id, fstype, parent_inode, boundary_name) "
    "VALUES (?, ?, ?, ?)";

inline constexpr std::string_view kSqlDeleteFilesystem =
    "DELETE FROM filesystems WHERE device_id = ?";

inline constexpr std::string_view kSqlMarkAtimeDirty =
    "INSERT INTO dirty (inode, atime_only) "
    "VALUES (?, 1) ON CONFLICT (inode) DO NOTHING";

inline constexpr std::string_view kSqlInsertDirty =
    "INSERT INTO dirty (inode, atime_only) VALUES (?, 0) "
    "ON CONFLICT (inode) DO UPDATE SET atime_only = 0";

inline constexpr std::string_view kSqlIsInodeDirty =
    "SELECT 1 FROM dirty WHERE inode = ?";

inline constexpr std::string_view kSqlIsInodeMutationDirty =
    "SELECT 1 FROM dirty WHERE inode = ? AND atime_only = 0";

inline constexpr std::string_view kSqlSelectMutationDirtyInodes =
    "SELECT inode FROM dirty WHERE atime_only = 0 ORDER BY inode";

inline constexpr std::string_view kSqlSelectInodeFromDirty =
    "SELECT inode FROM dirty ORDER BY inode";

inline constexpr std::string_view kSqlSelectDirtySnapshot =
    "SELECT inode, atime_only FROM dirty ORDER BY inode";

inline constexpr std::string_view kSqlHasDirtyRows =
    "SELECT EXISTS (SELECT 1 FROM dirty WHERE atime_only = 0), "
    "EXISTS (SELECT 1 FROM dirty WHERE atime_only = 1)";

inline constexpr std::string_view kSqlClearAllDirty = "DELETE FROM dirty";

inline constexpr std::string_view kSqlInsertDirtyRow =
    "INSERT INTO dirty (inode, atime_only) VALUES (?, ?)";

inline constexpr std::string_view kSqlUpdateDirtyAtimeOnly =
    "UPDATE dirty SET atime_only = 1 WHERE inode = ?";

inline constexpr std::string_view kSqlClearDirtyInode =
    "DELETE FROM dirty WHERE inode = ?";

inline constexpr std::string_view kSqlSweepUnlinkedInodes =
    "DELETE FROM inodes WHERE id != ? AND nlink = 0 "
    "AND (mode & ?) != ? "
    "AND NOT EXISTS (SELECT 1 FROM dentries "
    "WHERE dentries.inode = inodes.id AND state = 'present')";

inline constexpr std::string_view kSqlCountDirty = "SELECT COUNT(*) FROM dirty";

inline constexpr std::string_view kSqlRecoverResetDirectories =
    "UPDATE directories SET children_complete = 0, "
    "epoch = epoch + 1 "
    "WHERE inode IN (SELECT inode FROM dirty WHERE atime_only = 0)";

inline constexpr std::string_view kSqlRecoverResetDentries =
    "DELETE FROM dentries WHERE parent IN "
    "(SELECT inode FROM dirty WHERE atime_only = 0)";

inline constexpr std::string_view kSqlRecoverResetDentriesUnknown =
    "UPDATE dentries SET state = 'unknown', inode = NULL "
    "WHERE inode IN (SELECT inode FROM dirty WHERE atime_only = 0)";

inline constexpr std::string_view kSqlRecoverInvalidateInodes =
    "UPDATE inodes SET attrs_valid = 0, xattrs_complete = 0 "
    "WHERE id IN (SELECT inode FROM dirty WHERE atime_only = 0)";

inline constexpr std::string_view kSqlRecoverInvalidateInodesAtime =
    "UPDATE inodes SET attrs_valid = 0 "
    "WHERE id IN (SELECT inode FROM dirty WHERE atime_only = 1)";

inline constexpr std::string_view kSqlRecoverResetXattrs =
    "DELETE FROM xattrs WHERE inode IN "
    "(SELECT inode FROM dirty WHERE atime_only = 0)";

inline constexpr std::string_view kSqlRecoverResetSymlinks =
    "DELETE FROM symlinks WHERE inode IN "
    "(SELECT inode FROM dirty WHERE atime_only = 0)";

// SQL statements prepared by dcfs/migrate.cc.
inline constexpr std::string_view kSqlTableExists =
    "SELECT 1 FROM sqlite_master WHERE type = 'table' AND name = ?";

inline constexpr std::string_view kSqlRootInodeExists =
    "SELECT 1 FROM inodes WHERE id = 1";

inline constexpr std::string_view kSqlCreateSchemaState =
    "INSERT INTO cache_state "
    "(id, schema_version, source_device_id, clean_shutdown, "
    " boot_id) VALUES (1, ?, ?, 1, NULL)";

inline constexpr std::string_view kSqlCreateSchemaFilesystem =
    "INSERT INTO filesystems "
    "(device_id, fstype, parent_inode, boundary_name) "
    "VALUES (?, ?, NULL, NULL)";

inline constexpr std::string_view kSqlCreateSchemaInode =
    "INSERT INTO inodes "
    "(id, device_id, backing_ino, backing_gen, fuse_gen, "
    " attrs_valid) "
    "VALUES (1, ?, ?, ?, 0, 0)";

inline constexpr std::string_view kSqlCreateSchemaDirectory =
    "INSERT INTO directories (inode, children_complete) "
    "VALUES (1, 0)";

inline constexpr std::string_view kSqlMigrateV4CheckColumn =
    "SELECT 1 FROM pragma_table_info('cache_state') "
    "WHERE name = 'last_stub_id'";

inline constexpr std::string_view kSqlMigrateV5CheckColumn =
    "SELECT 1 FROM pragma_table_info('dirty') "
    "WHERE name = 'atime_only'";

inline constexpr std::string_view kSqlGetSchemaVersion =
    "SELECT schema_version FROM cache_state WHERE id = 1";

inline constexpr std::string_view kSqlGetSourceDeviceId =
    "SELECT source_device_id FROM cache_state WHERE id = 1";

inline constexpr std::string_view kSqlGetCleanShutdown =
    "SELECT clean_shutdown FROM cache_state WHERE id = 1";

inline constexpr std::string_view kSqlSetCleanShutdown =
    "UPDATE cache_state SET clean_shutdown = ? WHERE id = 1";

inline constexpr std::string_view kSqlGetBootId =
    "SELECT boot_id FROM cache_state WHERE id = 1";

inline constexpr std::string_view kSqlSetBootId =
    "UPDATE cache_state SET boot_id = ? WHERE id = 1";

inline constexpr std::string_view kSqlMigrateSelectMetaSchemaVersion =
    "SELECT value FROM meta WHERE key = 'schema_version'";

// SQL statements prepared by dcfs/fsck.cc.
inline constexpr std::string_view kSqlFsckInspectDirty =
    "SELECT inode, inode NOT IN (SELECT id FROM inodes) "
    "FROM dirty ORDER BY inode";

// All SQL statements prepared by dcfs against the real schema.
inline constexpr std::array kAllSqlStatements = {
    kSqlRequireInode,
    kSqlMarkIncomplete,
    kSqlLookup,
    kSqlGetAttr,
    kSqlGetStub,
    kSqlGetGeneration,
    kSqlListDir,
    kSqlChildrenComplete,
    kSqlIsDirComplete,
    kSqlReadLink,
    kSqlXattrsComplete,
    kSqlAnyXattrUnknown,
    kSqlListPresentXattrs,
    kSqlGetXattr,
    kSqlGetHandle,
    kSqlPresentDentryByInode,
    kSqlSelectUnknownDentries,
    kSqlParentOf,
    kSqlListFilesystems,
    kSqlGetFilesystem,
    kSqlFindMatchingInodes,
    kSqlUpdateMatchingRow,
    kSqlInsertNewRow,
    kSqlReinstateInode,
    kSqlFilesystemExists,
    kSqlFindStaleRootInodes,
    kSqlUpdateRoot,
    kSqlPutDentry,
    kSqlFindStub,
    kSqlUpdateStubAttr,
    kSqlGetLastStubId,
    kSqlSetLastStubId,
    kSqlInsertStub,
    kSqlMarkDirComplete,
    kSqlDirEpoch,
    kSqlEnsureDirectory,
    kSqlSelectDentryNamesInParent,
    kSqlDeleteDentry,
    kSqlDeleteAbsentDentries,
    kSqlRefusedDentriesToUnknown,
    kSqlInvalidateAttr,
    kSqlUpdateAttr,
    kSqlDropAtimeStamp,
    kSqlSelectTimesForTouchAtime,
    kSqlUpdateAtime,
    kSqlUpsertSymlink,
    kSqlPutXattr,
    kSqlSetXattrsComplete,
    kSqlDeleteAllXattrs,
    kSqlDeleteInode,
    kSqlInsertFilesystem,
    kSqlDeleteFilesystem,
    kSqlMarkAtimeDirty,
    kSqlInsertDirty,
    kSqlIsInodeDirty,
    kSqlIsInodeMutationDirty,
    kSqlSelectMutationDirtyInodes,
    kSqlSelectInodeFromDirty,
    kSqlSelectDirtySnapshot,
    kSqlHasDirtyRows,
    kSqlClearAllDirty,
    kSqlInsertDirtyRow,
    kSqlUpdateDirtyAtimeOnly,
    kSqlClearDirtyInode,
    kSqlSweepUnlinkedInodes,
    kSqlCountDirty,
    kSqlRecoverResetDirectories,
    kSqlRecoverResetDentries,
    kSqlRecoverResetDentriesUnknown,
    kSqlRecoverInvalidateInodes,
    kSqlRecoverInvalidateInodesAtime,
    kSqlRecoverResetXattrs,
    kSqlRecoverResetSymlinks,
    kSqlTableExists,
    kSqlRootInodeExists,
    kSqlCreateSchemaState,
    kSqlCreateSchemaFilesystem,
    kSqlCreateSchemaInode,
    kSqlCreateSchemaDirectory,
    kSqlMigrateV4CheckColumn,
    kSqlMigrateV5CheckColumn,
    kSqlGetSchemaVersion,
    kSqlGetSourceDeviceId,
    kSqlGetCleanShutdown,
    kSqlSetCleanShutdown,
    kSqlGetBootId,
    kSqlSetBootId,
    kSqlFsckInspectDirty,
};

inline std::span<const std::string_view> AllSqlStatements() {
  return kAllSqlStatements;
}

}  // namespace dcfs

#endif  // DCFS_SQL_STATEMENTS_H_
