#include "dcfs/metadata_cache.h"

#include <sys/stat.h>
#include <sys/sysmacros.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "absl/cleanup/cleanup.h"
#include "absl/container/flat_hash_set.h"
#include "absl/functional/function_ref.h"
#include "absl/log/log.h"
#include "absl/random/distributions.h"
#include "absl/status/status.h"
#include "absl/status/status_macros.h"
#include "absl/status/statusor.h"
#include "dcfs/context.h"
#include "dcfs/device_id.h"
#include "dcfs/escape.h"
#include "dcfs/file_handle.h"
#include "dcfs/migrate.h"
#include "dcfs/protocol_events.h"
#include "dcfs/ret_check.h"
#include "dcfs/sqlite.h"
#include "dcfs/status.h"

namespace dcfs::cache {
namespace {

using sqlite3::Statement;

// How many dentries ListDir reads per query. ListDir materializes each
// batch before running callbacks so that no cursor is open while a callback
// runs; this bounds the memory that costs.
constexpr int64_t kListDirBatch = 64;

// Every BLOB column in schema.sql is in a STRICT table, which rejects a
// bound TEXT value outright, so byte strings must go through the
// span<uint8_t> Bind() overload rather than the string_view one.
std::span<const uint8_t> Blob(std::string_view s) {
  return std::span<const uint8_t>(reinterpret_cast<const uint8_t *>(s.data()),
                                  s.size());
}

// Returns ctx.db's cached statement for `sql` with `args` bound to its
// parameters in order.
template <typename... Args>
absl::StatusOr<Statement *> Query(Context &ctx, std::string_view sql,
                                  Args &&...args) {
  ABSL_ASSIGN_OR_RETURN(Statement * stmt, ctx.db.Prepared(sql));
  ABSL_RETURN_IF_ERROR(stmt->BindAll(std::forward<Args>(args)...));
  return stmt;
}

// Runs a row-less statement and returns how many rows it changed directly
// (not counting foreign-key cascades).
template <typename... Args>
absl::StatusOr<int64_t> Execute(Context &ctx, std::string_view sql,
                                Args &&...args) {
  ABSL_ASSIGN_OR_RETURN(Statement * stmt,
                        Query(ctx, sql, std::forward<Args>(args)...));
  ABSL_RETURN_IF_ERROR(stmt->ExecuteOnce());
  return ctx.db.Changes();
}

// Runs `stmt`, a lookup that cannot produce more than one row (a primary or
// unique key, an aggregate, or LIMIT 1), calling `fn` on that row if there
// is one. Returns whether there was. Stops after the first row: no step to
// find the end of a result that cannot have more (it cost a statement step
// per lookup). A query that could return several and must check uses
// ForEachRow (ParentOf: LIMIT 2). Always leaves `stmt` reset, so no read
// cursor outlives the call.
absl::StatusOr<bool> ReadOne(Statement &stmt,
                             absl::FunctionRef<absl::Status(Statement &)> fn) {
  absl::Cleanup reset_when_done = [&stmt] { stmt.Reset().IgnoreError(); };
  ABSL_ASSIGN_OR_RETURN(bool has_row, stmt.Step());
  if (!has_row) return false;
  ABSL_RETURN_IF_ERROR(fn(stmt));
  return true;
}

absl::Status NoInode(InodeId id) {
  return NotFoundErrorBuilder() << "No cached inode " << id;
}

// NotFound unless `id` has a row. Write paths check this up front so that a
// stale id reports NotFound rather than a foreign-key constraint failure.
absl::Status RequireInode(Context &ctx, InodeId id) {
  ABSL_ASSIGN_OR_RETURN(Statement * stmt,
                        Query(ctx, "SELECT 1 FROM inodes WHERE id = ?", id));
  ABSL_ASSIGN_OR_RETURN(
      bool found,
      ReadOne(*stmt, [](Statement &) { return absl::OkStatus(); }));
  if (!found) return NoInode(id);
  return absl::OkStatus();
}

absl::Status MarkIncomplete(Context &ctx, InodeId dir) {
  return Execute(ctx,
                 "UPDATE directories SET children_complete = 0, "
                 "epoch = epoch + 1 WHERE inode = ?",
                 dir)
      .status();
}

// The attribute columns, in the order BindAttrs() binds them. A macro
// (rather than a constant) so it can be spliced into SQL string literals,
// which keeps each statement's text a compile-time constant.
#define DCFS_ATTR_VALUES                                                   \
  "mode = ?, nlink = ?, uid = ?, gid = ?, rdev = ?, "                      \
  "size = ?, blocks = ?, blksize = ?, atime_s = ?, atime_ns = ?, "         \
  "mtime_s = ?, mtime_ns = ?, ctime_s = ?, ctime_ns = ?, btime_s = ?, "    \
  "btime_ns = ?"
// An inode row's: DCFS_ATTR_VALUES, except that (step 23.8) a directory's
// or a symlink's cached access time is kept when it is the later one: the
// stamp TouchAtime makes (a listing, a readlink) never reaches the backing
// filesystem, so a record from it must not take the stamp back (an explicit
// atime set first drops it: DropAtimeStamp). The CASE reads the row's old
// values (SQLite evaluates every SET expression against the row before the
// update); the named parameters take the positions DCFS_ATTR_VALUES's ?s
// had (a ? is numbered one past the largest number before it), so BindAttrs
// binds them unchanged.
#define DCFS_KEEP_STAMP                                                      \
  "(mode & 61440) IN (16384, 40960) AND (mode & 61440) = (:mode & 61440) "   \
  "AND (atime_s > :atime_s OR (atime_s = :atime_s AND atime_ns > :atime_ns))"
#define DCFS_ATTR_ASSIGNMENTS                                                \
  "attrs_valid = 1, "                                                        \
  "mode = :mode, nlink = ?, uid = ?, gid = ?, rdev = ?, "                    \
  "size = ?, blocks = ?, blksize = ?, "                                      \
  "atime_s = CASE WHEN " DCFS_KEEP_STAMP " THEN atime_s ELSE :atime_s END, " \
  "atime_ns = CASE WHEN " DCFS_KEEP_STAMP " THEN atime_ns ELSE :atime_ns "  \
  "END, "                                                                    \
  "mtime_s = ?, mtime_ns = ?, ctime_s = ?, ctime_ns = ?, btime_s = ?, "      \
  "btime_ns = ?"
#define DCFS_ATTR_COLUMNS                                                  \
  "mode, nlink, uid, gid, rdev, size, blocks, blksize, atime_s, atime_ns, " \
  "mtime_s, mtime_ns, ctime_s, ctime_ns, btime_s, btime_ns"
// One NULL for each DCFS_ATTR_COLUMNS column (the stub half of ListDir's
// union has no inode row; a different count fails to prepare).
#define DCFS_ATTR_NULLS \
  "NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, " \
  "NULL, NULL, NULL, NULL"
#define DCFS_ATTR_PLACEHOLDERS "?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?"
constexpr int kNumAttrColumns = 16;

// Binds `stx`'s attributes to parameters first..first+15, in
// DCFS_ATTR_COLUMNS order. Everything is converted to int64 explicitly:
// SQLite integers are signed 64-bit, and the statx field types (__u32,
// __s64 = long long, ...) would otherwise be ambiguous among Bind()'s
// overloads. The unsigned 64-bit fields keep their bit pattern.
absl::Status BindAttrs(Statement &stmt, int first, const struct statx &stx) {
  const std::array<int64_t, kNumAttrColumns> values = {
      static_cast<int64_t>(stx.stx_mode),
      static_cast<int64_t>(stx.stx_nlink),
      static_cast<int64_t>(stx.stx_uid),
      static_cast<int64_t>(stx.stx_gid),
      static_cast<int64_t>(makedev(stx.stx_rdev_major, stx.stx_rdev_minor)),
      static_cast<int64_t>(stx.stx_size),
      static_cast<int64_t>(stx.stx_blocks),
      static_cast<int64_t>(stx.stx_blksize),
      static_cast<int64_t>(stx.stx_atime.tv_sec),
      static_cast<int64_t>(stx.stx_atime.tv_nsec),
      static_cast<int64_t>(stx.stx_mtime.tv_sec),
      static_cast<int64_t>(stx.stx_mtime.tv_nsec),
      static_cast<int64_t>(stx.stx_ctime.tv_sec),
      static_cast<int64_t>(stx.stx_ctime.tv_nsec),
      static_cast<int64_t>(stx.stx_btime.tv_sec),
      static_cast<int64_t>(stx.stx_btime.tv_nsec),
  };
  for (int i = 0; i < kNumAttrColumns; ++i) {
    ABSL_RETURN_IF_ERROR(stmt.Bind(first + i, values[i]));
  }
  return absl::OkStatus();
}

// Defined with the fill guards below: the atime-only dirty mark of a record
// of the attributes of an inode open for reading (Context::open_files).
absl::Status MarkIfOpen(Context &ctx, InodeId id);

absl::Status BindHandle(Statement &stmt, int first, const FileHandle &handle) {
  ABSL_RETURN_IF_ERROR(stmt.Bind(first, handle.handle_type));
  return stmt.Bind(first + 1, std::span<const uint8_t>(handle.bytes));
}

absl::StatusOr<FilesystemRow> ReadFilesystemRow(Statement &row) {
  FilesystemRow fs;
  ABSL_ASSIGN_OR_RETURN(fs.device,
                        DeviceId::Parse(row.Column<std::string>(0)));
  fs.fstype = row.Column<int64_t>(1);
  fs.parent_inode = row.Column<std::optional<int64_t>>(2);
  fs.boundary_name = row.Column<std::optional<std::string>>(3);
  return fs;
}

}  // namespace

// --- Reads ----------------------------------------------------------------

absl::StatusOr<LookupResult> Lookup(Context &ctx, InodeId parent,
                                    std::string_view name) {
  ABSL_ASSIGN_OR_RETURN(
      Statement * stmt,
      Query(ctx,
            "SELECT d.state, d.inode, s.id FROM dentries d "
            "LEFT JOIN stubs s ON s.parent = d.parent AND s.name = d.name "
            "WHERE d.parent = ? AND d.name = ?",
            parent, Blob(name)));
  LookupResult result;
  ABSL_ASSIGN_OR_RETURN(
      bool found, ReadOne(*stmt, [&](Statement &row) -> absl::Status {
        const std::string state = row.Column<std::string>(0);
        if (state == "present") {
          result = {LookupResult::Kind::kFound, row.Column<int64_t>(1)};
        } else if (state == "absent") {
          result = {LookupResult::Kind::kNegative, 0};
        } else if (state == "refused") {
          // schema.sql: a refused dentry always has its stub.
          std::optional<int64_t> stub = row.Column<std::optional<int64_t>>(2);
          RET_CHECK(stub.has_value())
              << "refused dentry " << EscapeBytes(name) << " of " << parent
              << " has no stub";
          result = {LookupResult::Kind::kRefused, *stub};
        } else {
          RET_CHECK_EQ(state, "unknown") << "bad dentries.state";
          result = {LookupResult::Kind::kUnknown, 0};
        }
        return absl::OkStatus();
      }));
  if (found) return result;
  // No row: the listing's completeness decides.
  ABSL_ASSIGN_OR_RETURN(bool complete, ChildrenComplete(ctx, parent));
  return LookupResult{complete ? LookupResult::Kind::kNegative
                               : LookupResult::Kind::kUnknown,
                      0};
}

// Decodes attrs_valid, fuse_gen, device_id, backing_ino, backing_gen and
// DCFS_ATTR_COLUMNS, starting at column `base` of `row`, into `attr`.
absl::Status DecodeAttr(Statement &row, int base, CachedAttr &attr) {
  attr.valid = row.Column<bool>(base);
  attr.fuse_gen = static_cast<uint32_t>(row.Column<int64_t>(base + 1));
  ABSL_ASSIGN_OR_RETURN(attr.device,
                        DeviceId::Parse(row.Column<std::string>(base + 2)));
  attr.backing_ino = row.Column<uint64_t>(base + 3);
  attr.backing_gen = row.Column<uint64_t>(base + 4);
  // Attribute columns are NULL until first set; Column<> reads NULL as 0,
  // which is what an unset `st` should hold anyway.
  struct stat &st = attr.st;
  const int c = base + 5;
  st.st_dev = 0;
  st.st_ino = attr.backing_ino;
  st.st_mode = row.Column<int64_t>(c);
  st.st_nlink = row.Column<int64_t>(c + 1);
  st.st_uid = row.Column<int64_t>(c + 2);
  st.st_gid = row.Column<int64_t>(c + 3);
  st.st_rdev = row.Column<uint64_t>(c + 4);
  st.st_size = row.Column<int64_t>(c + 5);
  st.st_blocks = row.Column<int64_t>(c + 6);
  st.st_blksize = row.Column<int64_t>(c + 7);
  st.st_atim = {row.Column<int64_t>(c + 8), row.Column<int64_t>(c + 9)};
  st.st_mtim = {row.Column<int64_t>(c + 10), row.Column<int64_t>(c + 11)};
  st.st_ctim = {row.Column<int64_t>(c + 12), row.Column<int64_t>(c + 13)};
  attr.btime = {row.Column<int64_t>(c + 14), row.Column<int64_t>(c + 15)};
  return absl::OkStatus();
}

absl::StatusOr<CachedAttr> GetAttr(Context &ctx, InodeId id) {
  ABSL_ASSIGN_OR_RETURN(
      Statement * stmt,
      Query(ctx,
            "SELECT attrs_valid, fuse_gen, device_id, backing_ino, "
            "backing_gen, " DCFS_ATTR_COLUMNS " FROM inodes WHERE id = ?",
            id));
  CachedAttr attr;
  ABSL_ASSIGN_OR_RETURN(
      bool found, ReadOne(*stmt, [&](Statement &row) -> absl::Status {
        return DecodeAttr(row, 0, attr);
      }));
  if (!found) return NoInode(id);
  return attr;
}

absl::StatusOr<StubRow> GetStub(Context &ctx, InodeId id) {
  ABSL_ASSIGN_OR_RETURN(
      Statement * stmt,
      Query(ctx,
            "SELECT parent, name, fuse_gen, " DCFS_ATTR_COLUMNS
            " FROM stubs WHERE id = ?",
            id));
  StubRow stub;
  ABSL_ASSIGN_OR_RETURN(
      bool found, ReadOne(*stmt, [&](Statement &row) -> absl::Status {
        stub.id = id;
        stub.parent = row.Column<int64_t>(0);
        stub.name = row.Column<std::string>(1);
        CachedAttr &attr = stub.attr;
        attr.valid = true;
        attr.fuse_gen = static_cast<uint32_t>(row.Column<int64_t>(2));
        attr.backing_ino = static_cast<uint64_t>(id);
        struct stat &st = attr.st;
        st.st_ino = static_cast<ino_t>(static_cast<uint64_t>(id));
        st.st_mode = row.Column<int64_t>(3);
        st.st_nlink = row.Column<int64_t>(4);
        st.st_uid = row.Column<int64_t>(5);
        st.st_gid = row.Column<int64_t>(6);
        st.st_rdev = row.Column<uint64_t>(7);
        st.st_size = row.Column<int64_t>(8);
        st.st_blocks = row.Column<int64_t>(9);
        st.st_blksize = row.Column<int64_t>(10);
        st.st_atim = {row.Column<int64_t>(11), row.Column<int64_t>(12)};
        st.st_mtim = {row.Column<int64_t>(13), row.Column<int64_t>(14)};
        st.st_ctim = {row.Column<int64_t>(15), row.Column<int64_t>(16)};
        attr.btime = {row.Column<int64_t>(17), row.Column<int64_t>(18)};
        return absl::OkStatus();
      }));
  if (!found) {
    return NotFoundErrorBuilder() << "No boundary stub " << id;
  }
  return stub;
}

CachedAttr WithStatx(CachedAttr attr, const struct statx &stx) {
  struct stat &st = attr.st;
  st.st_mode = stx.stx_mode;
  st.st_nlink = stx.stx_nlink;
  st.st_uid = stx.stx_uid;
  st.st_gid = stx.stx_gid;
  st.st_rdev = makedev(stx.stx_rdev_major, stx.stx_rdev_minor);
  st.st_size = static_cast<off_t>(stx.stx_size);
  st.st_blocks = static_cast<blkcnt_t>(stx.stx_blocks);
  st.st_blksize = static_cast<blksize_t>(stx.stx_blksize);
  st.st_atim = {stx.stx_atime.tv_sec, stx.stx_atime.tv_nsec};
  st.st_mtim = {stx.stx_mtime.tv_sec, stx.stx_mtime.tv_nsec};
  st.st_ctim = {stx.stx_ctime.tv_sec, stx.stx_ctime.tv_nsec};
  attr.btime = {stx.stx_btime.tv_sec, stx.stx_btime.tv_nsec};
  return attr;
}

bool SameAttrs(const CachedAttr &attr, const struct statx &stx) {
  const CachedAttr fresh = WithStatx(attr, stx);
  auto same_time = [](const struct timespec &a, const struct timespec &b) {
    return a.tv_sec == b.tv_sec && a.tv_nsec == b.tv_nsec;
  };
  const struct stat &a = attr.st;
  const struct stat &b = fresh.st;
  return a.st_mode == b.st_mode && a.st_nlink == b.st_nlink &&
         a.st_uid == b.st_uid && a.st_gid == b.st_gid &&
         a.st_rdev == b.st_rdev && a.st_size == b.st_size &&
         a.st_blocks == b.st_blocks && a.st_blksize == b.st_blksize &&
         same_time(a.st_atim, b.st_atim) && same_time(a.st_mtim, b.st_mtim) &&
         same_time(a.st_ctim, b.st_ctim) && same_time(attr.btime, fresh.btime);
}

absl::StatusOr<uint32_t> GetGeneration(Context &ctx, InodeId id) {
  ABSL_ASSIGN_OR_RETURN(
      Statement * stmt,
      Query(ctx, "SELECT fuse_gen FROM inodes WHERE id = ?", id));
  uint32_t gen = 0;
  ABSL_ASSIGN_OR_RETURN(bool found, ReadOne(*stmt, [&](Statement &row) {
                          gen = static_cast<uint32_t>(row.Column<int64_t>(0));
                          return absl::OkStatus();
                        }));
  if (!found) return NoInode(id);
  return gen;
}

absl::Status ListDir(Context &ctx, InodeId dir, int64_t cursor,
                     ListDirCallback cb) {
  return ListDir(ctx, dir, cursor,
                 [&](std::string_view name, InodeId child, int64_t next_cursor,
                     const CachedAttr *) { return cb(name, child, next_cursor); });
}

absl::Status ListDir(Context &ctx, InodeId dir, int64_t cursor,
                     ListDirAttrsCallback cb, int64_t batch_rows) {
  const int64_t limit = std::clamp<int64_t>(batch_rows, 1, kListDirBatch);
  struct Entry {
    int64_t rowid;
    std::string name;
    InodeId child;
    std::optional<CachedAttr> attr;  // only when the row's attributes are valid
  };
  std::vector<Entry> batch;
  while (true) {
    batch.clear();
    // Present names (with their inode rows' attributes: one join, not a
    // GetAttr per entry) and stubs, merged in rowid order. Each half keeps
    // the literal state of its partial index (dentries_present,
    // dentries_refused; see schema.sql), so each is a range scan with no
    // sort, and the merge reads only as far as the LIMIT. The attribute
    // columns of a stub are NULL: its row is not an inode's.
    ABSL_ASSIGN_OR_RETURN(
        Statement * stmt,
        Query(ctx,
              "SELECT d.rowid, d.name, d.inode, i.attrs_valid, i.fuse_gen, "
              "i.device_id, i.backing_ino, i.backing_gen, "
              DCFS_ATTR_COLUMNS  // no clash with dentries' columns
              " FROM dentries d JOIN inodes i ON i.id = d.inode "
              "WHERE d.parent = ?1 AND d.rowid > ?2 AND d.state = 'present' "
              "UNION ALL "
              "SELECT d.rowid, d.name, s.id, NULL, NULL, NULL, NULL, NULL, "
              DCFS_ATTR_NULLS " FROM dentries d "
              "JOIN stubs s ON s.parent = d.parent AND s.name = d.name "
              "WHERE d.parent = ?1 AND d.rowid > ?2 AND d.state = 'refused' "
              "ORDER BY 1 LIMIT ?3",
              dir, cursor, limit));
    ABSL_RETURN_IF_ERROR(stmt->ForEachRow([&](Statement &row) -> absl::Status {
      Entry entry{row.Column<int64_t>(0), row.Column<std::string>(1),
                  row.Column<int64_t>(2), std::nullopt};
      if (row.Column<bool>(3)) {  // attrs_valid (NULL for a stub: false)
        CachedAttr attr;
        ABSL_RETURN_IF_ERROR(DecodeAttr(row, 3, attr));
        entry.attr = std::move(attr);
      }
      batch.push_back(std::move(entry));
      return absl::OkStatus();
    }));
    // The statement is reset now, so callbacks may use the cache freely.
    for (const Entry &entry : batch) {
      ABSL_ASSIGN_OR_RETURN(
          bool more, cb(entry.name, entry.child, entry.rowid,
                        entry.attr.has_value() ? &*entry.attr : nullptr));
      if (!more) return absl::OkStatus();
    }
    if (static_cast<int64_t>(batch.size()) < limit) {
      return absl::OkStatus();
    }
    cursor = batch.back().rowid;
  }
}

absl::StatusOr<bool> ChildrenComplete(Context &ctx, InodeId dir) {
  ABSL_ASSIGN_OR_RETURN(
      Statement * stmt,
      Query(ctx, "SELECT children_complete FROM directories WHERE inode = ?",
            dir));
  bool complete = false;
  ABSL_RETURN_IF_ERROR(ReadOne(*stmt, [&](Statement &row) {
                         complete = row.Column<bool>(0);
                         return absl::OkStatus();
                       }).status());
  return complete;
}

absl::StatusOr<bool> IsDirComplete(Context &ctx, InodeId dir) {
  // ChildrenComplete, and a listing must not leave out (or list) a name
  // whose state is unknown: one statement (a missing directories row counts
  // as incomplete, as in ChildrenComplete).
  ABSL_ASSIGN_OR_RETURN(
      Statement * stmt,
      Query(ctx,
            "SELECT children_complete AND NOT EXISTS ("
            "SELECT 1 FROM dentries WHERE parent = ?1 AND state = 'unknown') "
            "FROM directories WHERE inode = ?1",
            dir));
  bool complete = false;
  ABSL_RETURN_IF_ERROR(ReadOne(*stmt, [&](Statement &row) {
                         complete = row.Column<bool>(0);
                         return absl::OkStatus();
                       }).status());
  return complete;
}

absl::StatusOr<std::string> Readlink(Context &ctx, InodeId id) {
  ABSL_ASSIGN_OR_RETURN(
      Statement * stmt,
      Query(ctx, "SELECT target FROM symlinks WHERE inode = ?", id));
  std::string target;
  ABSL_ASSIGN_OR_RETURN(bool found, ReadOne(*stmt, [&](Statement &row) {
                          target = row.Column<std::string>(0);
                          return absl::OkStatus();
                        }));
  if (!found) {
    return NotFoundErrorBuilder() << "No cached symlink target for " << id;
  }
  return target;
}

namespace {

// nullopt if there is no row for `id`.
absl::StatusOr<std::optional<bool>> XattrsComplete(Context &ctx, InodeId id) {
  ABSL_ASSIGN_OR_RETURN(
      Statement * stmt,
      Query(ctx, "SELECT xattrs_complete FROM inodes WHERE id = ?", id));
  std::optional<bool> complete;
  ABSL_RETURN_IF_ERROR(ReadOne(*stmt, [&](Statement &row) {
                         complete = row.Column<bool>(0);
                         return absl::OkStatus();
                       }).status());
  return complete;
}

}  // namespace

absl::StatusOr<std::optional<std::vector<std::string>>> ListXattrs(
    Context &ctx, InodeId id) {
  ABSL_ASSIGN_OR_RETURN(std::optional<bool> complete, XattrsComplete(ctx, id));
  if (!complete.has_value()) return NoInode(id);
  if (!*complete) return std::nullopt;
  // A listing must not leave out a name whose presence is unknown.
  ABSL_ASSIGN_OR_RETURN(
      Statement * unknown_stmt,
      Query(ctx,
            "SELECT 1 FROM xattrs WHERE inode = ? AND state = 'unknown' "
            "LIMIT 1",
            id));
  ABSL_ASSIGN_OR_RETURN(
      bool any_unknown,
      ReadOne(*unknown_stmt, [](Statement &) { return absl::OkStatus(); }));
  if (any_unknown) return std::nullopt;
  ABSL_ASSIGN_OR_RETURN(
      Statement * stmt,
      Query(ctx,
            "SELECT name FROM xattrs WHERE inode = ? AND state = 'present' "
            "ORDER BY name",
            id));
  std::vector<std::string> names;
  ABSL_RETURN_IF_ERROR(stmt->ForEachRow([&](Statement &row) {
    names.push_back(row.Column<std::string>(0));
    return absl::OkStatus();
  }));
  return names;
}

absl::StatusOr<std::optional<std::string>> GetXattr(Context &ctx, InodeId id,
                                                    std::string_view name) {
  ABSL_ASSIGN_OR_RETURN(
      Statement * stmt,
      Query(ctx,
            "SELECT state, value FROM xattrs WHERE inode = ? AND name = ?", id,
            Blob(name)));
  std::optional<std::string> state;
  std::optional<std::string> value;
  ABSL_RETURN_IF_ERROR(ReadOne(*stmt, [&](Statement &row) {
                         state = row.Column<std::string>(0);
                         value = row.Column<std::optional<std::string>>(1);
                         return absl::OkStatus();
                       }).status());
  auto absent = [&] {
    return NotFoundErrorBuilder()
           << "Inode " << id << " has no xattr " << EscapeBytes(name);
  };
  if (state.has_value()) {
    if (*state == "present") {
      RET_CHECK(value.has_value()) << "present xattr row without a value";
      return value;
    }
    if (*state == "absent") return absent();
    RET_CHECK_EQ(*state, "unknown") << "bad xattrs.state";
    return std::nullopt;
  }

  // No row: the set's completeness decides.
  ABSL_ASSIGN_OR_RETURN(std::optional<bool> complete, XattrsComplete(ctx, id));
  if (!complete.has_value()) return NoInode(id);
  if (*complete) return absent();
  return std::nullopt;
}

absl::StatusOr<FileHandle> GetHandle(Context &ctx, InodeId id) {
  ABSL_ASSIGN_OR_RETURN(
      Statement * stmt,
      Query(ctx, "SELECT device_id, handle_type, handle FROM inodes WHERE id = ?",
            id));
  std::optional<FileHandle> handle;
  ABSL_ASSIGN_OR_RETURN(
      bool found, ReadOne(*stmt, [&](Statement &row) -> absl::Status {
        if (row.ColumnIsNull(2)) return absl::OkStatus();
        FileHandle fh;
        ABSL_ASSIGN_OR_RETURN(fh.device,
                              DeviceId::Parse(row.Column<std::string>(0)));
        fh.handle_type = row.Column<int>(1);
        fh.bytes = row.Column<std::vector<uint8_t>>(2);
        handle = std::move(fh);
        return absl::OkStatus();
      }));
  if (!found) return NoInode(id);
  if (!handle.has_value()) {
    return NotFoundErrorBuilder() << "No cached handle for inode " << id;
  }
  return *std::move(handle);
}

absl::StatusOr<NamesToAsk> NamesToAskAbout(Context &ctx, InodeId id,
                                           size_t limit) {
  NamesToAsk result;
  auto collect = [&](Statement &row) {
    result.names.push_back(NamedIn{.parent = row.Column<int64_t>(0),
                                   .name = row.Column<std::string>(1)});
    return absl::OkStatus();
  };
  ABSL_ASSIGN_OR_RETURN(
      Statement * present,
      Query(ctx,
            "SELECT parent, name FROM dentries WHERE inode = ? AND "
            "state = 'present' LIMIT 1",
            id));
  ABSL_RETURN_IF_ERROR(present->ForEachRow(collect));
  if (!result.names.empty()) {
    result.present = true;
    return result;
  }
  ABSL_ASSIGN_OR_RETURN(
      Statement * unknown,
      Query(ctx,
            "SELECT parent, name FROM dentries WHERE state = 'unknown' "
            "LIMIT ?",
            static_cast<int64_t>(limit) + 1));
  ABSL_RETURN_IF_ERROR(unknown->ForEachRow(collect));
  if (result.names.size() > limit) {
    result.names.resize(limit);
    result.more = true;
  }
  return result;
}

absl::StatusOr<std::optional<InodeId>> ParentOf(Context &ctx, InodeId dir) {
  if (dir == kRootInode) return kRootInode;
  // One statement for "is there such an inode" (NotFound if not, as
  // RequireInode says) and its parents: the LEFT JOIN gives one NULL row for
  // an inode with no present dentry and none for a missing inode. LIMIT 2 is
  // enough to tell "one" from "more than one".
  ABSL_ASSIGN_OR_RETURN(
      Statement * stmt,
      Query(ctx,
            "SELECT d.parent FROM inodes i LEFT JOIN dentries d "
            "ON d.inode = i.id AND d.state = 'present' WHERE i.id = ? "
            "LIMIT 2",
            dir));
  bool inode_exists = false;
  std::vector<InodeId> parents;
  ABSL_RETURN_IF_ERROR(stmt->ForEachRow([&](Statement &row) {
    inode_exists = true;
    if (!row.ColumnIsNull(0)) parents.push_back(row.Column<int64_t>(0));
    return absl::OkStatus();
  }));
  if (!inode_exists) return NoInode(dir);
  if (parents.empty()) return std::nullopt;
  // Directories cannot be hard linked, so two cached dentries for one
  // directory means the cache is inconsistent.
  RET_CHECK_EQ(parents.size(), 1u)
      << "directory " << dir << " has more than one cached dentry";
  return parents.front();
}

absl::StatusOr<std::vector<FilesystemRow>> ListFilesystems(Context &ctx) {
  ABSL_ASSIGN_OR_RETURN(
      Statement * stmt,
      Query(ctx,
            "SELECT device_id, fstype, parent_inode, boundary_name "
            "FROM filesystems ORDER BY rowid"));
  std::vector<FilesystemRow> result;
  ABSL_RETURN_IF_ERROR(stmt->ForEachRow([&](Statement &row) -> absl::Status {
    ABSL_ASSIGN_OR_RETURN(FilesystemRow fs, ReadFilesystemRow(row));
    result.push_back(std::move(fs));
    return absl::OkStatus();
  }));
  return result;
}

absl::StatusOr<FilesystemRow> GetFilesystem(Context &ctx,
                                            const DeviceId &device) {
  ABSL_ASSIGN_OR_RETURN(
      Statement * stmt,
      Query(ctx,
            "SELECT device_id, fstype, parent_inode, boundary_name "
            "FROM filesystems WHERE device_id = ?",
            Blob(device.Serialize())));
  std::optional<FilesystemRow> result;
  ABSL_ASSIGN_OR_RETURN(
      bool found, ReadOne(*stmt, [&](Statement &row) -> absl::Status {
        ABSL_ASSIGN_OR_RETURN(result, ReadFilesystemRow(row));
        return absl::OkStatus();
      }));
  if (!found) {
    return NotFoundErrorBuilder() << "No filesystem " << device.ToString();
  }
  return *std::move(result);
}

// --- Writes ---------------------------------------------------------------

namespace {

// Every row UpsertInode already has for this backing inode number, split
// into at most one match (`existing`) and the rest (`stale`, left over from
// before the number was recycled).
//
// A row is the same object only if its generation (when both are known:
// nonzero), its stored handle bytes and (when both are known) its birth
// time all match too. The generation alone is not enough: it is 0
// ("unknown") for symlinks and special files and on filesystems without
// FS_IOC_GETVERSION, yet the backing handle still encodes the real
// generation (audit F5); and btrfs can reissue an identical (ino,
// generation, handle) after its own power loss rolled back a transaction,
// which only the birth time tells apart (audit F6). None of this costs a
// syscall: the handle and statx (with STATX_BTIME) are already in hand.
struct MatchingRows {
  std::optional<UpsertResult> existing;
  std::vector<InodeId> stale;
};

absl::StatusOr<MatchingRows> FindMatchingRows(Context &ctx,
                                              const std::string &device,
                                              uint64_t ino,
                                              const FileHandle &handle,
                                              uint64_t backing_gen,
                                              const struct statx &stx) {
  ABSL_ASSIGN_OR_RETURN(
      Statement * find,
      Query(ctx,
            "SELECT id, fuse_gen, backing_gen, handle_type, handle, "
            "btime_s, btime_ns FROM inodes "
            "WHERE device_id = ? AND backing_ino = ?",
            Blob(device), ino));
  const bool new_btime_known =
      (stx.stx_mask & STATX_BTIME) != 0 &&
      (stx.stx_btime.tv_sec != 0 || stx.stx_btime.tv_nsec != 0);
  MatchingRows rows;
  ABSL_RETURN_IF_ERROR(find->ForEachRow([&](Statement &row) {
    InodeId id = row.Column<int64_t>(0);
    // A generation of 0 is "unknown" (backing::ReadGeneration could not
    // read it), not a value, on either side: as VerifyBackingIdentity,
    // it does not tell objects apart by itself -- the handle below does.
    const uint64_t stored_gen = row.Column<uint64_t>(2);
    bool same = stored_gen == backing_gen || stored_gen == 0 ||
                backing_gen == 0;
    if (same && !row.ColumnIsNull(4)) {
      same = row.Column<int>(3) == handle.handle_type &&
             row.Column<std::vector<uint8_t>>(4) == handle.bytes;
    }
    if (same && new_btime_known && !row.ColumnIsNull(5)) {
      const int64_t btime_s = row.Column<int64_t>(5);
      const int64_t btime_ns = row.Column<int64_t>(6);
      if (btime_s != 0 || btime_ns != 0) {
        same = btime_s == static_cast<int64_t>(stx.stx_btime.tv_sec) &&
               btime_ns == static_cast<int64_t>(stx.stx_btime.tv_nsec);
      }
    }
    if (same) {
      rows.existing = UpsertResult{
          .id = id,
          .fuse_gen = static_cast<uint32_t>(row.Column<int64_t>(1)),
          .created = false};
    } else {
      rows.stale.push_back(id);
    }
    return absl::OkStatus();
  }));
  return rows;
}

// The handle already matches (or the row had none): update `id`'s row with
// the freshly probed handle and attributes.
absl::Status UpdateMatchingRow(Context &ctx, InodeId id,
                               const FileHandle &handle,
                               const struct statx &stx) {
  ABSL_ASSIGN_OR_RETURN(
      Statement * update,
      ctx.db.Prepared("UPDATE inodes SET handle_type = ?, handle = ?, "
                      DCFS_ATTR_ASSIGNMENTS " WHERE id = ?"));
  ABSL_RETURN_IF_ERROR(BindHandle(*update, 1, handle));
  ABSL_RETURN_IF_ERROR(BindAttrs(*update, 3, stx));
  ABSL_RETURN_IF_ERROR(update->Bind(3 + kNumAttrColumns, id));
  ABSL_RETURN_IF_ERROR(update->ExecuteOnce());
  return MarkIfOpen(ctx, id);
}

// No existing row matched: insert a fresh one with a new random fuse_gen.
absl::StatusOr<UpsertResult> InsertNewRow(Context &ctx,
                                          const std::string &device,
                                          uint64_t ino, uint64_t backing_gen,
                                          const FileHandle &handle,
                                          const struct statx &stx) {
  // Random, never 0 (the root's): see schema.sql's identity model.
  const uint32_t fuse_gen = absl::Uniform(absl::IntervalClosedClosed,
                                          ctx.rng, uint32_t{1}, UINT32_MAX);
  ABSL_ASSIGN_OR_RETURN(
      Statement * insert,
      ctx.db.Prepared("INSERT INTO inodes (device_id, backing_ino, "
                      "backing_gen, fuse_gen, handle_type, handle, "
                      "attrs_valid, " DCFS_ATTR_COLUMNS
                      ") VALUES (?, ?, ?, ?, ?, ?, 1, "
                      DCFS_ATTR_PLACEHOLDERS ")"));
  ABSL_RETURN_IF_ERROR(insert->BindAll(Blob(device), ino, backing_gen,
                                       static_cast<int64_t>(fuse_gen)));
  ABSL_RETURN_IF_ERROR(BindHandle(*insert, 5, handle));
  ABSL_RETURN_IF_ERROR(BindAttrs(*insert, 7, stx));
  ABSL_RETURN_IF_ERROR(insert->ExecuteOnce());
  return UpsertResult{.id = ctx.db.LastInsertRowId(),
                      .fuse_gen = fuse_gen,
                      .created = true};
}

}  // namespace

absl::Status ReinstateInode(Context &ctx, InodeId id, uint32_t fuse_gen,
                            const FileHandle &handle, const struct statx &stx,
                            uint64_t backing_gen) {
  ABSL_ASSIGN_OR_RETURN(
      Statement * insert,
      ctx.db.Prepared("INSERT INTO inodes (id, device_id, backing_ino, "
                      "backing_gen, fuse_gen, handle_type, handle, "
                      "attrs_valid, " DCFS_ATTR_COLUMNS
                      ") VALUES (?, ?, ?, ?, ?, ?, ?, 1, "
                      DCFS_ATTR_PLACEHOLDERS ")"));
  const std::string device = handle.device.Serialize();
  ABSL_RETURN_IF_ERROR(insert->BindAll(id, Blob(device),
                                       static_cast<uint64_t>(stx.stx_ino),
                                       backing_gen,
                                       static_cast<int64_t>(fuse_gen)));
  ABSL_RETURN_IF_ERROR(BindHandle(*insert, 6, handle));
  ABSL_RETURN_IF_ERROR(BindAttrs(*insert, 8, stx));
  return insert->ExecuteOnce();
}

absl::StatusOr<UpsertResult> UpsertInode(Context &ctx,
                                         const FileHandle &handle,
                                         const struct statx &stx,
                                         uint64_t backing_gen) {
  const std::string device = handle.device.Serialize();
  const uint64_t ino = stx.stx_ino;
  UpsertResult result;
  ABSL_RETURN_IF_ERROR(ctx.db.Transaction([&]() -> absl::Status {
    ABSL_ASSIGN_OR_RETURN(
        Statement * fs_stmt,
        Query(ctx, "SELECT 1 FROM filesystems WHERE device_id = ?",
              Blob(device)));
    ABSL_ASSIGN_OR_RETURN(
        bool fs_known,
        ReadOne(*fs_stmt, [](Statement &) { return absl::OkStatus(); }));
    RET_CHECK(fs_known) << "UpsertInode on unregistered filesystem "
                        << handle.device.ToString();

    // Collected up front so the cursor is closed before we write.
    ABSL_ASSIGN_OR_RETURN(
        MatchingRows rows,
        FindMatchingRows(ctx, device, ino, handle, backing_gen, stx));

    if (rows.existing.has_value()) {
      ABSL_RETURN_IF_ERROR(
          UpdateMatchingRow(ctx, rows.existing->id, handle, stx));
      result = *rows.existing;
      return absl::OkStatus();
    }

    // The backing filesystem reused this inode number for a new object: the
    // old rows describe something that no longer exists. Invalidated before
    // the insert, which may reuse the old row's exact identity triple.
    for (InodeId id : rows.stale) {
      ABSL_RETURN_IF_ERROR(InvalidateInode(ctx, id));
    }
    ABSL_ASSIGN_OR_RETURN(
        result, InsertNewRow(ctx, device, ino, backing_gen, handle, stx));
    return absl::OkStatus();
  }));
  return result;
}

absl::Status UpsertRoot(Context &ctx, const FileHandle &handle,
                        const struct statx &stx, uint64_t backing_gen) {
  return ctx.db.Transaction([&]() -> absl::Status {
    ABSL_ASSIGN_OR_RETURN(DeviceId source, GetSourceDeviceId(ctx.db));
    RET_CHECK(handle.device == source)
        << "UpsertRoot with a handle on " << handle.device.ToString()
        << ", not the source device " << source.ToString();
    ABSL_RETURN_IF_ERROR(RequireInode(ctx, kRootInode));
    // Any other row claiming the root's backing inode number is a leftover
    // from before the number was recycled into the root; it would also
    // collide with the root's new identity.
    const std::string device = handle.device.Serialize();
    ABSL_ASSIGN_OR_RETURN(
        Statement * find,
        Query(ctx,
              "SELECT id FROM inodes "
              "WHERE device_id = ? AND backing_ino = ? AND id != ?",
              Blob(device), static_cast<uint64_t>(stx.stx_ino), kRootInode));
    std::vector<InodeId> stale;
    ABSL_RETURN_IF_ERROR(find->ForEachRow([&](Statement &row) {
      stale.push_back(row.Column<int64_t>(0));
      return absl::OkStatus();
    }));
    for (InodeId id : stale) {
      ABSL_RETURN_IF_ERROR(InvalidateInode(ctx, id));
    }

    ABSL_ASSIGN_OR_RETURN(
        Statement * update,
        ctx.db.Prepared("UPDATE inodes SET backing_ino = ?, backing_gen = ?, "
                        "handle_type = ?, handle = ?, " DCFS_ATTR_ASSIGNMENTS
                        " WHERE id = ?"));
    ABSL_RETURN_IF_ERROR(
        update->BindAll(static_cast<uint64_t>(stx.stx_ino), backing_gen));
    ABSL_RETURN_IF_ERROR(BindHandle(*update, 3, handle));
    ABSL_RETURN_IF_ERROR(BindAttrs(*update, 5, stx));
    ABSL_RETURN_IF_ERROR(update->Bind(5 + kNumAttrColumns, kRootInode));
    return update->ExecuteOnce();
  });
}

namespace {

// Sets (parent, name) to `state` (pointing at `child` iff 'present'). An
// upsert rather than INSERT OR REPLACE: REPLACE deletes and reinserts,
// giving the entry a new rowid, which would make an in-progress ListDir
// (whose cursor is a rowid) see a renamed-over name twice.
absl::Status PutDentry(Context &ctx, InodeId parent, std::string_view name,
                       std::string_view state, std::optional<InodeId> child) {
  return Execute(ctx,
                 "INSERT INTO dentries (parent, name, state, inode) "
                 "VALUES (?, ?, ?, ?) "
                 "ON CONFLICT (parent, name) DO UPDATE SET "
                 "state = excluded.state, inode = excluded.inode",
                 parent, Blob(name), state, child)
      .status();
}

}  // namespace

absl::Status LinkDentry(Context &ctx, InodeId parent, std::string_view name,
                        InodeId child) {
  return ctx.db.Transaction([&]() -> absl::Status {
    ABSL_RETURN_IF_ERROR(RequireInode(ctx, parent));
    ABSL_RETURN_IF_ERROR(RequireInode(ctx, child));
    return PutDentry(ctx, parent, name, "present", child);
  });
}

absl::Status SetNegative(Context &ctx, InodeId parent, std::string_view name) {
  return ctx.db.Transaction([&]() -> absl::Status {
    ABSL_RETURN_IF_ERROR(RequireInode(ctx, parent));
    return PutDentry(ctx, parent, name, "absent", std::nullopt);
  });
}

absl::StatusOr<InodeId> SetRefused(Context &ctx, InodeId parent,
                                   std::string_view name,
                                   const struct statx &root) {
  InodeId stub = 0;
  ABSL_RETURN_IF_ERROR(ctx.db.Transaction([&]() -> absl::Status {
    ABSL_RETURN_IF_ERROR(RequireInode(ctx, parent));
    // The dentry first. A stub of the same name exists only if the dentry
    // was refused, or forgotten since (unknown): the triggers delete it
    // when the dentry is recorded present or absent. That stub is kept,
    // with its nodeid and generation (the kernel's revalidation of the name
    // must find the nodeid it holds).
    ABSL_RETURN_IF_ERROR(PutDentry(ctx, parent, name, "refused", std::nullopt));
    ABSL_ASSIGN_OR_RETURN(
        Statement * existing,
        Query(ctx, "SELECT id FROM stubs WHERE parent = ? AND name = ?",
              parent, Blob(name)));
    ABSL_ASSIGN_OR_RETURN(bool found,
                          ReadOne(*existing, [&](Statement &row) {
                            stub = row.Column<int64_t>(0);
                            return absl::OkStatus();
                          }));
    if (found) {
      ABSL_ASSIGN_OR_RETURN(
          Statement * update,
          ctx.db.Prepared("UPDATE stubs SET " DCFS_ATTR_VALUES
                          " WHERE id = ?"));
      ABSL_RETURN_IF_ERROR(BindAttrs(*update, 1, root));
      ABSL_RETURN_IF_ERROR(update->Bind(1 + kNumAttrColumns, stub));
      return update->ExecuteOnce();
    }
    // The next nodeid up from the highest ever handed out
    // (cache_state.last_stub_id), or kFirstStubId (2^63): never one a
    // stub that went had, which a kernel may still hold
    // (formal/lifetime.tla's NodeidStable).
    ABSL_ASSIGN_OR_RETURN(
        Statement * last,
        Query(ctx, "SELECT last_stub_id FROM cache_state WHERE id = 1"));
    std::optional<int64_t> highest;
    ABSL_ASSIGN_OR_RETURN(bool has_state, ReadOne(*last, [&](Statement &row) {
                            highest = row.Column<std::optional<int64_t>>(0);
                            return absl::OkStatus();
                          }));
    RET_CHECK(has_state) << "the cache_state row is missing";
    if (highest.has_value() && *highest == -1) {
      return ResourceExhaustedErrorBuilder() << "No boundary stub nodeid left";
    }
    stub = highest.has_value() ? *highest + 1 : kFirstStubId;
    ABSL_RETURN_IF_ERROR(
        Execute(ctx, "UPDATE cache_state SET last_stub_id = ? WHERE id = 1",
                stub)
            .status());
    const uint32_t fuse_gen = absl::Uniform(absl::IntervalClosedClosed,
                                            ctx.rng, uint32_t{1}, UINT32_MAX);
    ABSL_ASSIGN_OR_RETURN(
        Statement * insert,
        ctx.db.Prepared("INSERT INTO stubs (id, parent, name, fuse_gen, "
                        DCFS_ATTR_COLUMNS ") VALUES (?, ?, ?, ?, "
                        DCFS_ATTR_PLACEHOLDERS ")"));
    ABSL_RETURN_IF_ERROR(insert->BindAll(stub, parent, Blob(name),
                                         static_cast<int64_t>(fuse_gen)));
    ABSL_RETURN_IF_ERROR(BindAttrs(*insert, 5, root));
    return insert->ExecuteOnce();
  }));
  return stub;
}

absl::Status UnlinkDentry(Context &ctx, InodeId parent, std::string_view name) {
  return ctx.db.Transaction([&]() -> absl::Status {
    ABSL_RETURN_IF_ERROR(RequireInode(ctx, parent));
    return PutDentry(ctx, parent, name, "unknown", std::nullopt);
  });
}

absl::Status RenameDentry(Context &ctx, InodeId parent, std::string_view name,
                          InodeId newparent, std::string_view newname) {
  return ctx.db.Transaction([&]() -> absl::Status {
    ABSL_ASSIGN_OR_RETURN(LookupResult source, Lookup(ctx, parent, name));
    if (source.kind != LookupResult::Kind::kFound) {
      return NotFoundErrorBuilder()
             << "No cached positive dentry " << EscapeBytes(name) << " in "
             << parent << " to rename";
    }
    if (parent == newparent && name == newname) return absl::OkStatus();
    ABSL_RETURN_IF_ERROR(RequireInode(ctx, newparent));
    ABSL_RETURN_IF_ERROR(
        PutDentry(ctx, parent, name, "unknown", std::nullopt));
    return PutDentry(ctx, newparent, newname, "present", source.id);
  });
}

absl::Status MarkDirComplete(Context &ctx, InodeId dir, bool complete) {
  return ctx.db.Transaction([&]() -> absl::Status {
    ABSL_RETURN_IF_ERROR(RequireInode(ctx, dir));
    return Execute(ctx,
                   "INSERT INTO directories (inode, children_complete) "
                   "VALUES (?, ?) ON CONFLICT (inode) DO UPDATE SET "
                   "children_complete = excluded.children_complete, "
                   "epoch = epoch + (excluded.children_complete = 0)",
                   dir, complete)
        .status();
  });
}

absl::StatusOr<int64_t> DirEpoch(Context &ctx, InodeId dir) {
  ABSL_ASSIGN_OR_RETURN(
      Statement * stmt,
      Query(ctx, "SELECT epoch FROM directories WHERE inode = ?", dir));
  int64_t epoch = 0;
  ABSL_RETURN_IF_ERROR(ReadOne(*stmt, [&](Statement &row) {
                         epoch = row.Column<int64_t>(0);
                         return absl::OkStatus();
                       }).status());
  return epoch;
}

absl::Status EnsureDirectory(Context &ctx, InodeId dir) {
  return ctx.db.Transaction([&]() -> absl::Status {
    ABSL_RETURN_IF_ERROR(RequireInode(ctx, dir));
    return Execute(ctx,
                   "INSERT INTO directories (inode, children_complete) "
                   "VALUES (?, 0) ON CONFLICT (inode) DO NOTHING",
                   dir)
        .status();
  });
}

absl::Status PruneDentriesNotIn(Context &ctx, InodeId dir,
                                std::span<const std::string> names) {
  // The set difference is computed here rather than with NOT IN (...):
  // a directory can have more names than SQLite allows bound parameters,
  // and splitting a NOT IN into batches would not be a set difference.
  const absl::flat_hash_set<std::string_view> keep(names.begin(), names.end());
  return ctx.db.Transaction([&]() -> absl::Status {
    ABSL_ASSIGN_OR_RETURN(
        Statement * stmt,
        Query(ctx, "SELECT name FROM dentries WHERE parent = ?", dir));
    std::vector<std::string> doomed;
    ABSL_RETURN_IF_ERROR(stmt->ForEachRow([&](Statement &row) {
      std::string name = row.Column<std::string>(0);
      if (!keep.contains(name)) doomed.push_back(std::move(name));
      return absl::OkStatus();
    }));
    for (const std::string &name : doomed) {
      ABSL_RETURN_IF_ERROR(
          Execute(ctx, "DELETE FROM dentries WHERE parent = ? AND name = ?",
                  dir, Blob(name))
              .status());
    }
    return absl::OkStatus();
  });
}

absl::Status MarkUnknown(Context &ctx, InodeId parent,
                         std::span<const std::string> names) {
  return ctx.db.Transaction([&]() -> absl::Status {
    ABSL_RETURN_IF_ERROR(RequireInode(ctx, parent));
    for (const std::string &name : names) {
      ABSL_RETURN_IF_ERROR(
          PutDentry(ctx, parent, name, "unknown", std::nullopt));
    }
    return absl::OkStatus();
  });
}

absl::Status ForgetNegativeDentries(Context &ctx, InodeId dir) {
  return ctx.db.Transaction([&]() -> absl::Status {
    ABSL_RETURN_IF_ERROR(RequireInode(ctx, dir));
    ABSL_RETURN_IF_ERROR(
        Execute(ctx,
                "DELETE FROM dentries WHERE parent = ? AND state = 'absent'",
                dir)
            .status());
    // A refusal is forgotten, not deleted: its stub stays (schema.sql), so
    // that the relisting, if it finds the name refused again, keeps its
    // nodeid and generation.
    ABSL_RETURN_IF_ERROR(
        Execute(ctx,
                "UPDATE dentries SET state = 'unknown' "
                "WHERE parent = ? AND state = 'refused'",
                dir)
            .status());
    return MarkIncomplete(ctx, dir);
  });
}

absl::Status MarkAttrsUnknown(Context &ctx, InodeId id) {
  return ctx.db.Transaction([&]() -> absl::Status {
    ABSL_ASSIGN_OR_RETURN(
        int64_t changed,
        Execute(ctx, "UPDATE inodes SET attrs_valid = 0 WHERE id = ?", id));
    if (changed == 0) return NoInode(id);
    return absl::OkStatus();
  });
}

absl::Status UpdateAttr(Context &ctx, InodeId id, const struct statx &stx) {
  return ctx.db.Transaction([&]() -> absl::Status {
    ABSL_ASSIGN_OR_RETURN(
        Statement * update,
        ctx.db.Prepared("UPDATE inodes SET " DCFS_ATTR_ASSIGNMENTS
                        " WHERE id = ?"));
    ABSL_RETURN_IF_ERROR(BindAttrs(*update, 1, stx));
    ABSL_RETURN_IF_ERROR(update->Bind(1 + kNumAttrColumns, id));
    ABSL_RETURN_IF_ERROR(update->ExecuteOnce());
    if (ctx.db.Changes() == 0) return NoInode(id);
    return MarkIfOpen(ctx, id);
  });
}

absl::Status DropAtimeStamp(Context &ctx, InodeId id) {
  return Execute(ctx,
                 "UPDATE inodes SET atime_s = ?, atime_ns = 0 "
                 "WHERE id = ? AND attrs_valid = 0 AND (mode & ?) IN (?, ?)",
                 std::numeric_limits<int64_t>::min(), id,
                 static_cast<int64_t>(S_IFMT), static_cast<int64_t>(S_IFDIR),
                 static_cast<int64_t>(S_IFLNK))
      .status();
}

absl::StatusOr<bool> TouchAtime(Context &ctx, InodeId id,
                                const struct timespec &now) {
  // Read first, outside any transaction: almost every listing or readlink
  // finds the atime recent (relatime), and a write transaction for each
  // would take the database's write lock for nothing (review L7).
  ABSL_ASSIGN_OR_RETURN(
      Statement * stmt,
      Query(ctx,
            "SELECT attrs_valid, atime_s, atime_ns, mtime_s, mtime_ns, "
            "ctime_s, ctime_ns FROM inodes WHERE id = ?",
            id));
  bool valid = false;
  struct timespec atime {}, mtime {}, ctime {};
  ABSL_ASSIGN_OR_RETURN(bool found, ReadOne(*stmt, [&](Statement &row) {
                          valid = row.Column<bool>(0);
                          atime = {row.Column<int64_t>(1),
                                   row.Column<int64_t>(2)};
                          mtime = {row.Column<int64_t>(3),
                                   row.Column<int64_t>(4)};
                          ctime = {row.Column<int64_t>(5),
                                   row.Column<int64_t>(6)};
                          return absl::OkStatus();
                        }));
  if (!found) return NoInode(id);
  if (!valid || ctx.atime == AtimePolicy::kNever) return false;
  // fs/inode.c relatime_need_update: an atime not after mtime or ctime,
  // or 24 hours old or more, is updated.
  auto not_after = [](const struct timespec &a, const struct timespec &b) {
    return a.tv_sec < b.tv_sec ||
           (a.tv_sec == b.tv_sec && a.tv_nsec <= b.tv_nsec);
  };
  const bool update =
      ctx.atime == AtimePolicy::kStrict || not_after(atime, mtime) ||
      not_after(atime, ctime) || now.tv_sec - atime.tv_sec >= 24 * 60 * 60;
  if (!update) return false;
  // The write only if the row still holds what was read (nothing runs in
  // between today; under coroutines nothing suspends between these SQLite
  // calls either, and the compare keeps it right regardless).
  int64_t changed = 0;
  ABSL_RETURN_IF_ERROR(ctx.db.Transaction([&]() -> absl::Status {
    ABSL_ASSIGN_OR_RETURN(
        changed,
        Execute(ctx,
                "UPDATE inodes SET atime_s = ?, atime_ns = ? "
                "WHERE id = ? AND attrs_valid = 1 AND atime_s = ? "
                "AND atime_ns = ?",
                static_cast<int64_t>(now.tv_sec),
                static_cast<int64_t>(now.tv_nsec), id,
                static_cast<int64_t>(atime.tv_sec),
                static_cast<int64_t>(atime.tv_nsec)));
    return absl::OkStatus();
  }));
  return changed > 0;
}

absl::Status SetSymlink(Context &ctx, InodeId id, std::string_view target) {
  return ctx.db.Transaction([&]() -> absl::Status {
    ABSL_RETURN_IF_ERROR(RequireInode(ctx, id));
    return Execute(ctx,
                   "INSERT INTO symlinks (inode, target) VALUES (?, ?) "
                   "ON CONFLICT (inode) DO UPDATE SET target = excluded.target",
                   id, Blob(target))
        .status();
  });
}

namespace {

// Sets the state of `id`'s xattr `name`: present with `value`, or (value
// nullopt) `state` 'absent' or 'unknown'.
absl::Status PutXattr(Context &ctx, InodeId id, std::string_view name,
                      std::string_view state,
                      std::optional<std::string_view> value) {
  ABSL_ASSIGN_OR_RETURN(
      Statement * stmt,
      Query(ctx,
            "INSERT INTO xattrs (inode, name, state, value) "
            "VALUES (?, ?, ?, ?) "
            "ON CONFLICT (inode, name) DO UPDATE SET "
            "state = excluded.state, value = excluded.value",
            id, Blob(name), state));
  if (value.has_value()) {
    ABSL_RETURN_IF_ERROR(stmt->Bind(4, Blob(*value)));
  } else {
    ABSL_RETURN_IF_ERROR(stmt->Bind(4, std::nullopt));
  }
  return stmt->ExecuteOnce();
}

absl::Status SetXattrsComplete(Context &ctx, InodeId id, bool complete) {
  return Execute(ctx, "UPDATE inodes SET xattrs_complete = ? WHERE id = ?",
                 complete, id)
      .status();
}

}  // namespace

absl::Status ReplaceXattrs(
    Context &ctx, InodeId id,
    std::span<const std::pair<std::string, std::string>> xattrs) {
  // Unguarded: a population must have checked CanFill in the same
  // transaction (FillXattrs does), so that it cannot overwrite a newer
  // mutation's result.
  return ctx.db.Transaction([&]() -> absl::Status {
    ABSL_RETURN_IF_ERROR(RequireInode(ctx, id));
    ABSL_RETURN_IF_ERROR(
        Execute(ctx, "DELETE FROM xattrs WHERE inode = ?", id).status());
    for (const auto &[name, value] : xattrs) {
      ABSL_RETURN_IF_ERROR(PutXattr(ctx, id, name, "present", value));
    }
    return SetXattrsComplete(ctx, id, true);
  });
}

absl::Status SetXattr(Context &ctx, InodeId id, std::string_view name,
                      std::string_view value) {
  return ctx.db.Transaction([&]() -> absl::Status {
    ABSL_RETURN_IF_ERROR(RequireInode(ctx, id));
    return PutXattr(ctx, id, name, "present", value);
  });
}

absl::Status RemoveXattr(Context &ctx, InodeId id, std::string_view name) {
  return ctx.db.Transaction([&]() -> absl::Status {
    ABSL_RETURN_IF_ERROR(RequireInode(ctx, id));
    return PutXattr(ctx, id, name, "absent", std::nullopt);
  });
}

absl::Status ForgetXattr(Context &ctx, InodeId id, std::string_view name) {
  return ctx.db.Transaction([&]() -> absl::Status {
    ABSL_RETURN_IF_ERROR(RequireInode(ctx, id));
    return PutXattr(ctx, id, name, "unknown", std::nullopt);
  });
}

absl::Status MarkXattrsUnknown(Context &ctx, InodeId id) {
  return ctx.db.Transaction([&]() -> absl::Status {
    ABSL_RETURN_IF_ERROR(RequireInode(ctx, id));
    ABSL_RETURN_IF_ERROR(
        Execute(ctx, "DELETE FROM xattrs WHERE inode = ?", id).status());
    return SetXattrsComplete(ctx, id, false);
  });
}

absl::Status InvalidateInode(Context &ctx, InodeId id) {
  RET_CHECK_NE(id, kRootInode) << "the root inode cannot be invalidated";
  ABSL_RETURN_IF_ERROR(ctx.db.Transaction([&]() -> absl::Status {
    ABSL_RETURN_IF_ERROR(RequireInode(ctx, id));
    ctx.events->InodeForgetting(ctx, id);
    // The schema's inodes_delete_unknowns trigger makes every dentry that
    // pointed at it unknown; its own dentries (if a directory) cascade.
    return Execute(ctx, "DELETE FROM inodes WHERE id = ?", id).status();
  }));
  // Not a step of the protocol model: see ProtocolEvents::InodeForgotten.
  ctx.events->InodeForgotten(ctx, id);
  return absl::OkStatus();
}

absl::Status DeleteInode(Context &ctx, InodeId id) {
  return InvalidateInode(ctx, id);
}

absl::Status AddFilesystem(Context &ctx, const DeviceId &device,
                           int64_t fstype, std::optional<InodeId> parent,
                           std::optional<std::string> boundary_name) {
  return ctx.db.Transaction([&]() -> absl::Status {
    absl::StatusOr<FilesystemRow> existing = GetFilesystem(ctx, device);
    if (existing.ok()) {
      return AlreadyExistsErrorBuilder()
             << "Filesystem " << device.ToString() << " already exists";
    }
    if (!absl::IsNotFound(existing.status())) return existing.status();
    if (parent.has_value()) {
      ABSL_RETURN_IF_ERROR(RequireInode(ctx, *parent));
    }
    std::optional<std::span<const uint8_t>> name;
    if (boundary_name.has_value()) name = Blob(*boundary_name);
    return Execute(ctx,
                   "INSERT INTO filesystems "
                   "(device_id, fstype, parent_inode, boundary_name) "
                   "VALUES (?, ?, ?, ?)",
                   Blob(device.Serialize()), fstype, parent, name)
        .status();
  });
}

absl::Status PurgeFilesystem(Context &ctx, const DeviceId &device) {
  return ctx.db.Transaction([&]() -> absl::Status {
    ABSL_ASSIGN_OR_RETURN(DeviceId source, GetSourceDeviceId(ctx.db));
    RET_CHECK(device != source) << "cannot purge the source filesystem";
    ABSL_ASSIGN_OR_RETURN(FilesystemRow fs, GetFilesystem(ctx, device));
    ABSL_RETURN_IF_ERROR(
        Execute(ctx, "DELETE FROM filesystems WHERE device_id = ?",
                Blob(device.Serialize()))
            .status());
    // The cascade above deleted the filesystem's inodes, and the schema's
    // inodes_delete_unknowns trigger made the boundary dentry pointing at
    // its root unknown (the mount point is not absent, merely forgotten), so
    // the parent rediscovers it.
    return absl::OkStatus();
  });
}

// --- Fills vs. concurrent mutations ----------------------------------------

namespace {

// Past FillGuards::max_touched entries, FillGuards::touched is cleared
// (raising the floor, which only makes fills that are running right now
// skip caching, and a sync point running now keep every row).
void Touch(FillGuards &fills, InodeId id) {
  if (fills.touched.size() >= fills.max_touched &&
      !fills.touched.contains(id)) {
    fills.touched.clear();
    fills.floor = ++fills.seq;
  }
  fills.touched[id] = ++fills.seq;
}

// Phase 1 of a mutation of `ids` has committed: it is in flight on each.
void RegisterMutation(Context &ctx, std::span<const InodeId> ids,
                      std::vector<std::pair<InodeId, uint64_t>> &out) {
  FillGuards &fills = ctx.fills;
  for (InodeId id : ids) {
    bool seen = false;
    for (const auto &entry : out) seen = seen || entry.first == id;
    if (seen) continue;
    Touch(fills, id);
    ++fills.inflight[id];
    out.emplace_back(id, fills.seq);
  }
}

}  // namespace

absl::Status MarkAtimeDirty(Context &ctx, InodeId id, GuardTouch touch) {
  // A mutation's row stays one (its reason is the stronger).
  ABSL_RETURN_IF_ERROR(Execute(ctx,
                               "INSERT INTO dirty (inode, atime_only) "
                               "VALUES (?, 1) ON CONFLICT (inode) DO NOTHING",
                               id)
                           .status());
  ctx.dirty.atime = true;
  ++ctx.dirty.inserts;
  if (ctx.dirty.atime_since == absl::InfiniteFuture()) {
    ctx.dirty.atime_since = ctx.clock->TimeNow();
  }
  // A sync point whose BeginSync came before this record keeps the row
  // (ClearDirty: CanFill fails): the access time recorded may come from a
  // read after its syncfs began. A mutation of `id` in flight touches it at
  // its end anyway, and its phase 3 must still see that it Owns `id`,
  // unless the caller is a held fill that could not record (see the
  // declaration).
  if (touch == GuardTouch::kAlways ||
      (touch == GuardTouch::kUnlessInFlight &&
       !ctx.fills.inflight.contains(id))) {
    Touch(ctx.fills, id);
  }
  return absl::OkStatus();
}

namespace {

absl::Status MarkIfOpen(Context &ctx, InodeId id) {
  if (ctx.open_files == nullptr || !ctx.open_files->contains(id)) {
    return absl::OkStatus();
  }
  // A file open for writing is dirty since its phase 1, its attributes are
  // kept unknown, and its last release ends its writes (EndWrites).
  if (ctx.open_for_write != nullptr && ctx.open_for_write->contains(id)) {
    return absl::OkStatus();
  }
  return MarkAtimeDirty(ctx, id);
}

}  // namespace

FillSnapshot BeginFill(const Context &ctx) { return {.seq = ctx.fills.seq}; }

void EndWrites(Context &ctx, InodeId id) {
  Touch(ctx.fills, id);
  // The guard event is done (no suspension point between it and the
  // caller's removal of `id` from ctx.open_for_write).
  ctx.events->WritesEnded(ctx, id);
}

bool CanFill(const Context &ctx, FillSnapshot snapshot, InodeId id) {
  const FillGuards &fills = ctx.fills;
  if (snapshot.seq < fills.floor) return false;
  if (fills.inflight.contains(id)) return false;
  auto it = fills.touched.find(id);
  return it == fills.touched.end() || it->second <= snapshot.seq;
}

Mutation::Mutation(Mutation &&other) noexcept
    : ctx_(other.ctx_), ids_(std::move(other.ids_)) {
  other.ids_.clear();
}

Mutation::~Mutation() { End(); }

bool Mutation::Owns(InodeId id) const {
  const FillGuards &fills = ctx_->fills;
  for (const auto &[mine, seq] : ids_) {
    if (mine != id) continue;
    auto inflight = fills.inflight.find(id);
    auto touched = fills.touched.find(id);
    return inflight != fills.inflight.end() && inflight->second == 1 &&
           touched != fills.touched.end() && touched->second == seq;
  }
  return false;
}

void Mutation::End() {
  if (ids_.empty()) return;
  auto each_id = [&](absl::FunctionRef<void(InodeId)> each) {
    for (const auto &entry : ids_) each(entry.first);
  };
  // Owns as the phase 3 the caller has just committed (if any) used it.
  ctx_->events->MutationEnding(*ctx_, each_id,
                               [&](InodeId id) { return Owns(id); });
  FillGuards &fills = ctx_->fills;
  for (const auto &[id, seq] : ids_) {
    auto it = fills.inflight.find(id);
    if (it != fills.inflight.end() && --it->second <= 0) {
      fills.inflight.erase(it);
    }
    Touch(fills, id);
  }
  // Model: the phase-3 action (commit, End, the refresh's snapshot) or a
  // failure's End.
  ctx_->events->MutationEnded(*ctx_, each_id);
  ids_.clear();
}

absl::StatusOr<bool> FillAttr(Context &ctx, FillSnapshot snapshot, InodeId id,
                              const struct statx &stx) {
  bool filled = false;
  ABSL_RETURN_IF_ERROR(ctx.db.Transaction([&]() -> absl::Status {
    if (!CanFill(ctx, snapshot, id)) return absl::OkStatus();
    ABSL_RETURN_IF_ERROR(UpdateAttr(ctx, id, stx));
    filled = true;
    return absl::OkStatus();
  }));
  return filled;
}

absl::StatusOr<bool> FillXattrs(
    Context &ctx, FillSnapshot snapshot, InodeId id,
    std::span<const std::pair<std::string, std::string>> xattrs) {
  bool filled = false;
  ABSL_RETURN_IF_ERROR(ctx.db.Transaction([&]() -> absl::Status {
    if (!CanFill(ctx, snapshot, id)) return absl::OkStatus();
    ABSL_RETURN_IF_ERROR(ReplaceXattrs(ctx, id, xattrs));
    filled = true;
    return absl::OkStatus();
  }));
  return filled;
}

absl::StatusOr<bool> FillXattr(Context &ctx, FillSnapshot snapshot, InodeId id,
                               std::string_view name,
                               std::optional<std::string_view> value) {
  bool filled = false;
  ABSL_RETURN_IF_ERROR(ctx.db.Transaction([&]() -> absl::Status {
    if (!CanFill(ctx, snapshot, id)) return absl::OkStatus();
    if (value.has_value()) {
      ABSL_RETURN_IF_ERROR(SetXattr(ctx, id, name, *value));
    } else {
      ABSL_RETURN_IF_ERROR(RemoveXattr(ctx, id, name));
    }
    filled = true;
    return absl::OkStatus();
  }));
  return filled;
}

absl::StatusOr<bool> FillSymlink(Context &ctx, FillSnapshot snapshot,
                                 InodeId id, std::string_view target) {
  bool filled = false;
  ABSL_RETURN_IF_ERROR(ctx.db.Transaction([&]() -> absl::Status {
    if (!CanFill(ctx, snapshot, id)) return absl::OkStatus();
    ABSL_RETURN_IF_ERROR(SetSymlink(ctx, id, target));
    filled = true;
    return absl::OkStatus();
  }));
  return filled;
}

// --- The durable dirty set --------------------------------------------------

namespace {

absl::Status InsertDirty(Context &ctx, std::span<const InodeId> ids) {
  for (InodeId id : ids) {
    ABSL_RETURN_IF_ERROR(
        Execute(ctx,
                "INSERT INTO dirty (inode, atime_only) VALUES (?, 0) "
                "ON CONFLICT (inode) DO UPDATE SET atime_only = 0",
                id)
            .status());
  }
  return absl::OkStatus();
}

}  // namespace

absl::StatusOr<Mutation> BeginMutation(
    Context &ctx, std::span<const InodeId> ids,
    absl::FunctionRef<absl::Status()> body) {
  bool known = true;
  for (InodeId id : ids) known = known && ctx.dirty.durable.contains(id);
  auto each_id = [&](absl::FunctionRef<void(InodeId)> each) {
    for (InodeId id : ids) each(id);
  };
  absl::Status committed = ctx.db.Transaction(
      [&]() -> absl::Status {
        ABSL_RETURN_IF_ERROR(body());
        if (known) return absl::OkStatus();
        return InsertDirty(ctx, ids);
      },
      known ? sqlite3::Durability::kNormal : sqlite3::Durability::kSync);
  // kAborted is BeginRemove's or BeginRename's verification (nothing was
  // written; the caller resolves again): the model's retry/EAGAIN branch.
  if (absl::IsAborted(committed)) ctx.events->MutationAborted(ctx, each_id);
  ABSL_RETURN_IF_ERROR(committed);
  ctx.dirty.any = true;
  if (!known) ctx.dirty.durable.insert(ids.begin(), ids.end());
  Mutation mutation(&ctx);
  RegisterMutation(ctx, ids, mutation.ids_);
  // Model: the BeginMutation branch of the mutation's first step (after
  // RegisterMutation, as the model's BeginMutation moves the guards in the
  // same step).
  ctx.events->MutationBegun(ctx, each_id, !known);
  return mutation;
}

absl::StatusOr<Mutation> BeginCreate(Context &ctx, InodeId parent, std::string_view name) {
  const std::string names[] = {std::string(name)};
  const InodeId ids[] = {parent};
  return BeginMutation(ctx, ids, [&]() -> absl::Status {
    ABSL_RETURN_IF_ERROR(MarkUnknown(ctx, parent, names));
    return MarkAttrsUnknown(ctx, parent);
  });
}

absl::StatusOr<Mutation> BeginRemove(Context &ctx, InodeId parent,
                                     std::string_view name, InodeId child,
                                     FillSnapshot resolved) {
  const std::string names[] = {std::string(name)};
  const InodeId ids[] = {parent, child};
  return BeginMutation(ctx, ids, [&]() -> absl::Status {
    // First, before writing anything: the verification (review of R4,
    // finding 2; as in BeginRename). (parent, name) can only stop holding
    // `child` through dcfs by a mutation naming `parent`, which touches it;
    // so if CanFill holds for both, `name` still holds `child` and the
    // unlinkat will remove it. From here on Mutation::Owns takes over.
    // Otherwise the transaction rolls back, the mutation never begins, and
    // the caller resolves again.
    for (InodeId id : ids) {
      if (!CanFill(ctx, resolved, id)) {
        return AbortedErrorBuilder()
               << "Removal of " << EscapeBytes(name) << " in " << parent
               << ": inode " << id << " changed since it was resolved";
      }
    }
    ABSL_RETURN_IF_ERROR(MarkUnknown(ctx, parent, names));
    ABSL_RETURN_IF_ERROR(MarkAttrsUnknown(ctx, parent));
    return MarkAttrsUnknown(ctx, child);
  });
}

absl::StatusOr<Mutation> BeginRename(Context &ctx, InodeId parent, std::string_view name,
                         InodeId newparent, std::string_view newname,
                         InodeId src, std::optional<InodeId> dst,
                         FillSnapshot resolved) {
  const std::string names[] = {std::string(name)};
  const std::string newnames[] = {std::string(newname)};
  std::vector<InodeId> ids = {parent, newparent, src};
  if (dst.has_value()) ids.push_back(*dst);
  return BeginMutation(ctx, ids, [&]() -> absl::Status {
    // First, before writing anything: the verification (formal/ finding
    // rename_stale_source). If no mutation of any inode this rename names
    // began or ended since `resolved`, or is in flight, then neither name's
    // entry changed through dcfs since the caller resolved src and dst, so
    // both are still what the names hold: phase 3 may link them. From
    // here on Mutation::Owns takes over. Otherwise the transaction rolls
    // back, the mutation never begins, and the caller resolves again.
    for (InodeId id : ids) {
      if (!CanFill(ctx, resolved, id)) {
        return AbortedErrorBuilder()
               << "Rename of " << EscapeBytes(name) << " in " << parent
               << ": inode " << id
               << " changed since its source and destination were resolved";
      }
    }
    ABSL_RETURN_IF_ERROR(MarkUnknown(ctx, parent, names));
    ABSL_RETURN_IF_ERROR(MarkUnknown(ctx, newparent, newnames));
    for (InodeId id : ids) ABSL_RETURN_IF_ERROR(MarkAttrsUnknown(ctx, id));
    return absl::OkStatus();
  });
}

absl::StatusOr<Mutation> BeginLink(Context &ctx, InodeId src, InodeId newparent,
                       std::string_view newname) {
  const std::string names[] = {std::string(newname)};
  const InodeId ids[] = {newparent, src};
  return BeginMutation(ctx, ids, [&]() -> absl::Status {
    ABSL_RETURN_IF_ERROR(MarkUnknown(ctx, newparent, names));
    ABSL_RETURN_IF_ERROR(MarkAttrsUnknown(ctx, newparent));
    return MarkAttrsUnknown(ctx, src);
  });
}

absl::StatusOr<Mutation> BeginAttrChange(Context &ctx, InodeId id,
                             std::span<const std::string_view> xattrs) {
  const InodeId ids[] = {id};
  return BeginMutation(ctx, ids, [&]() -> absl::Status {
    for (std::string_view name : xattrs) {
      ABSL_RETURN_IF_ERROR(ForgetXattr(ctx, id, name));
    }
    return MarkAttrsUnknown(ctx, id);
  });
}

absl::StatusOr<Mutation> BeginAttrChanges(Context &ctx,
                                          std::span<const InodeId> ids) {
  return BeginMutation(ctx, ids, [&]() -> absl::Status {
    for (InodeId id : ids) {
      // A row gone meanwhile (invalidated) has nothing to mark; the others
      // still need their phase 1. Its dirty row is harmless (the next sync
      // point clears it; recovery finds nothing to forget).
      if (absl::Status marked = MarkAttrsUnknown(ctx, id);
          !marked.ok() && !absl::IsNotFound(marked)) {
        return marked;
      }
    }
    return absl::OkStatus();
  });
}

absl::StatusOr<Mutation> BeginXattrChange(Context &ctx, InodeId id,
                              std::string_view name) {
  const InodeId ids[] = {id};
  return BeginMutation(ctx, ids, [&]() -> absl::Status {
    ABSL_RETURN_IF_ERROR(ForgetXattr(ctx, id, name));
    return MarkAttrsUnknown(ctx, id);
  });
}

absl::Status MarkDirty(Context &ctx, std::span<const InodeId> ids) {
  ABSL_RETURN_IF_ERROR(ctx.db.Transaction([&] { return InsertDirty(ctx, ids); }));
  ctx.dirty.any = true;
  return absl::OkStatus();
}

absl::StatusOr<bool> IsDirty(Context &ctx, InodeId id) {
  ABSL_ASSIGN_OR_RETURN(
      Statement * stmt, Query(ctx, "SELECT 1 FROM dirty WHERE inode = ?", id));
  return ReadOne(*stmt, [](Statement &) { return absl::OkStatus(); });
}

absl::StatusOr<std::vector<InodeId>> ListDirty(Context &ctx,
                                               bool mutations_only) {
  ABSL_ASSIGN_OR_RETURN(
      Statement * stmt,
      mutations_only
          ? Query(ctx,
                  "SELECT inode FROM dirty WHERE atime_only = 0 ORDER BY inode")
          : Query(ctx, "SELECT inode FROM dirty ORDER BY inode"));
  std::vector<InodeId> ids;
  ABSL_RETURN_IF_ERROR(stmt->ForEachRow([&](Statement &row) {
    ids.push_back(row.Column<int64_t>(0));
    return absl::OkStatus();
  }));
  return ids;
}

absl::StatusOr<SyncSnapshot> BeginSync(Context &ctx) {
  SyncSnapshot snapshot{.fills = BeginFill(ctx)};
  ABSL_ASSIGN_OR_RETURN(
      Statement * rows,
      Query(ctx, "SELECT inode, atime_only FROM dirty ORDER BY inode"));
  ABSL_RETURN_IF_ERROR(rows->ForEachRow([&](Statement &row) {
    const InodeId id = row.Column<int64_t>(0);
    snapshot.dirty.push_back(id);
    if (row.Column<int64_t>(1) != 0) snapshot.atime_only.push_back(id);
    return absl::OkStatus();
  }));
  if (ctx.open_for_write != nullptr) {
    snapshot.open_for_write.assign(ctx.open_for_write->begin(),
                                   ctx.open_for_write->end());
  }
  if (ctx.open_files != nullptr) {
    snapshot.open_files.assign(ctx.open_files->begin(),
                               ctx.open_files->end());
  }
  snapshot.inserts = ctx.dirty.inserts;
  return snapshot;
}

namespace {

// What Context::dirty.any and .atime are for the table as it is now (in
// the caller's transaction, if any: assign them only once it committed, so
// that a rollback cannot leave them false over rows that came back).
struct DirtyFlags {
  bool any = true;
  bool atime = true;
};
absl::StatusOr<DirtyFlags> CountDirty(Context &ctx) {
  ABSL_ASSIGN_OR_RETURN(
      Statement * stmt,
      Query(ctx,
            "SELECT EXISTS (SELECT 1 FROM dirty WHERE atime_only = 0), "
            "EXISTS (SELECT 1 FROM dirty WHERE atime_only = 1)"));
  DirtyFlags flags;
  ABSL_RETURN_IF_ERROR(ReadOne(*stmt, [&](Statement &row) {
                         flags.any = row.Column<int64_t>(0) != 0;
                         flags.atime = row.Column<int64_t>(1) != 0;
                         return absl::OkStatus();
                       }).status());
  return flags;
}

}  // namespace

absl::Status ClearDirty(Context &ctx, const SyncSnapshot &synced,
                        std::span<const InodeId> keep, int64_t *cleared) {
  int64_t removed = 0;
  absl::flat_hash_set<InodeId> kept(keep.begin(), keep.end());
  // Open for writing when the syncfs began: the kernel may have written to
  // it after that (and released it since, which EndWrites also records in
  // the guards; this does not depend on it).
  kept.insert(synced.open_for_write.begin(), synced.open_for_write.end());
  // Open at all when it began, or now (step 23.8): the kernel may have read
  // it after the syncfs began, and the access time those reads gave is not
  // covered; its last release records it (a held fill, which touches it).
  // What the syncfs did cover is everything else the row stood for, so such
  // a row stays as atime-only (a mutation's row of a file held open would
  // otherwise drive every sync point until the release, each clearing
  // nothing).
  absl::flat_hash_set<InodeId> kept_open(synced.open_files.begin(),
                                         synced.open_files.end());
  if (ctx.open_files != nullptr) {
    kept_open.insert(ctx.open_files->begin(), ctx.open_files->end());
  }
  // The fast path: if the clock has not moved since BeginSync and no
  // mutation is in flight, then the per-row loop below would delete exactly
  // the rows of the table that are not kept, and one bulk delete does it.
  // Why that is exact:
  //  - The table holds no row that is not in `synced.dirty`. Rows are
  //    added only by a phase 1 (BeginMutation, whose RegisterMutation then
  //    advances the clock with nothing in between) and by MarkDirty in a
  //    phase 3 (before its mutation's End, which advances it), and by
  //    MarkAtimeDirty, which counts itself in Context::dirty.inserts (a
  //    cold open's advances no clock). Only
  //    ClearDirty deletes rows (RecoverDirty keeps them, step 12.6b), and
  //    another sync point's ClearDirty in between could only have removed
  //    rows (then putting a kept one back below is merely conservative).
  //  - CanFill(synced.fills, id) holds for every id: nothing was touched
  //    after the snapshot (every touch advances the clock), the floor is at
  //    most the clock (a prune sets it to a value of the clock, and also
  //    advances it), and nothing is in flight.
  // So the rows to delete are those not in `kept`. Compared with the
  // per-row loop: one statement instead of one per row (each also a
  // lookup in `kept`), and the case of every sync point under today's
  // single thread, since nothing can run during one.
  const bool nothing_moved = ctx.fills.seq == synced.fills.seq &&
                             ctx.fills.inflight.empty() &&
                             ctx.dirty.inserts == synced.inserts;
  DirtyFlags flags;
  ABSL_RETURN_IF_ERROR(ctx.db.Transaction([&]() -> absl::Status {
    removed = 0;
    if (nothing_moved) {
      ABSL_RETURN_IF_ERROR(Execute(ctx, "DELETE FROM dirty").status());
      removed = static_cast<int64_t>(synced.dirty.size());
      // Put back the kept rows that were there (and only those: a kept
      // inode that was not dirty must not become dirty), each with its
      // reason (nothing moved: the snapshot's is the row's), or atime-only
      // if only being open keeps it (see above).
      absl::flat_hash_set<InodeId> put_back = kept;
      put_back.insert(kept_open.begin(), kept_open.end());
      for (InodeId id : put_back) {
        if (!std::binary_search(synced.dirty.begin(), synced.dirty.end(),
                                id)) {
          continue;
        }
        const int64_t atime_only =
            !kept.contains(id) ||
                    std::binary_search(synced.atime_only.begin(),
                                       synced.atime_only.end(), id)
                ? 1
                : 0;
        ABSL_RETURN_IF_ERROR(
            Execute(ctx,
                    "INSERT INTO dirty (inode, atime_only) VALUES (?, ?)", id,
                    atime_only)
                .status());
        --removed;
      }
    } else {
      for (InodeId id : synced.dirty) {
        if (kept.contains(id)) continue;
        // A mutation of `id` in flight now, or one that began or ended
        // since the snapshot, or the end of a writable open of it since,
        // may have issued its backing syscall (or written) after the
        // syncfs started: the syncfs does not cover it, so its row stays
        // until a later sync point's does. (CanFill is exactly "no
        // mutation of `id` began or ended since the snapshot, and none is
        // in flight"; a snapshot older than the guards' floor keeps every
        // row.)
        if (!CanFill(ctx, synced.fills, id)) continue;
        if (kept_open.contains(id)) {
          ABSL_RETURN_IF_ERROR(
              Execute(ctx, "UPDATE dirty SET atime_only = 1 WHERE inode = ?",
                      id)
                  .status());
          continue;
        }
        ABSL_RETURN_IF_ERROR(
            Execute(ctx, "DELETE FROM dirty WHERE inode = ?", id).status());
        ++removed;
      }
    }
    ABSL_ASSIGN_OR_RETURN(flags, CountDirty(ctx));
    return absl::OkStatus();
  }));
  // This transaction only deleted rows (the fast path deletes and puts
  // back kept rows in one transaction, which a crash keeps whole or not at
  // all), so a kept row that was durable still is; but ctx.dirty.durable
  // is not tracked per row across a clear: start over, which at worst
  // costs a phase 1 a kSync commit it did not need.
  ctx.dirty.durable.clear();
  ctx.dirty.any = flags.any;
  ctx.dirty.atime = flags.atime;
  // The atime-only rows left are open files' (kept) or recorded since the
  // snapshot: their age counts from now.
  ctx.dirty.atime_since =
      flags.atime ? ctx.clock->TimeNow() : absl::InfiniteFuture();
  if (cleared != nullptr) *cleared = removed;
  return absl::OkStatus();
}

absl::StatusOr<int64_t> ForgetUnnamedRows(Context &ctx) {
  int64_t count = 0;
  ABSL_RETURN_IF_ERROR(ctx.db.Transaction([&]() -> absl::Status {
    ABSL_ASSIGN_OR_RETURN(
        count,
        Execute(ctx,
                "DELETE FROM inodes WHERE id != ? AND nlink = 0 "
                "AND (mode & ?) != ? "
                "AND NOT EXISTS (SELECT 1 FROM dentries "
                "WHERE dentries.inode = inodes.id AND state = 'present')",
                kRootInode, static_cast<int64_t>(S_IFMT),
                static_cast<int64_t>(S_IFDIR)));
    return absl::OkStatus();
  }));
  return count;
}

absl::StatusOr<int64_t> RecoverDirty(Context &ctx) {
  int64_t count = 0;
  ABSL_RETURN_IF_ERROR(ctx.db.Transaction([&]() -> absl::Status {
    ABSL_ASSIGN_OR_RETURN(Statement * stmt,
                          Query(ctx, "SELECT COUNT(*) FROM dirty"));
    ABSL_RETURN_IF_ERROR(ReadOne(*stmt, [&](Statement &row) {
                           count = row.Column<int64_t>(0);
                           return absl::OkStatus();
                         }).status());
    if (count == 0) return absl::OkStatus();
    // A dirty directory's listing: any name may have changed, including
    // ones nothing cached (only a directory has dentries of its own).
    ABSL_RETURN_IF_ERROR(
        Execute(ctx,
                "UPDATE directories SET children_complete = 0, "
                "epoch = epoch + 1 "
                "WHERE inode IN (SELECT inode FROM dirty WHERE atime_only = 0)")
            .status());
    ABSL_RETURN_IF_ERROR(
        Execute(ctx,
                "DELETE FROM dentries WHERE parent IN "
                "(SELECT inode FROM dirty WHERE atime_only = 0)")
            .status());
    // A dirty inode's own names elsewhere (it may have been renamed or
    // unlinked): unknown.
    ABSL_RETURN_IF_ERROR(
        Execute(ctx,
                "UPDATE dentries SET state = 'unknown', inode = NULL "
                "WHERE inode IN (SELECT inode FROM dirty WHERE atime_only = 0)")
            .status());
    ABSL_RETURN_IF_ERROR(
        Execute(ctx,
                "UPDATE inodes SET attrs_valid = 0, xattrs_complete = 0 "
                "WHERE id IN (SELECT inode FROM dirty WHERE atime_only = 0)")
            .status());
    // An atime-only row (step 23.8: a file that was only read, or held open
    // while recorded) stands for its attributes alone (the access time the
    // backing filesystem may have lost or kept): nothing else is forgotten.
    ABSL_RETURN_IF_ERROR(
        Execute(ctx,
                "UPDATE inodes SET attrs_valid = 0 "
                "WHERE id IN (SELECT inode FROM dirty WHERE atime_only = 1)")
            .status());
    // GetXattr serves a present or absent row even when the set is
    // incomplete, so the rows must go, not just the completeness flag.
    ABSL_RETURN_IF_ERROR(
        Execute(ctx,
                "DELETE FROM xattrs WHERE inode IN "
                "(SELECT inode FROM dirty WHERE atime_only = 0)")
            .status());
    ABSL_RETURN_IF_ERROR(
        Execute(ctx,
                "DELETE FROM symlinks WHERE inode IN "
                "(SELECT inode FROM dirty WHERE atime_only = 0)")
            .status());
    // The dirty set itself stays until a sync point's syncfs and ClearDirty
    // (step 12.6b): the crashed run's backing changes may not be durable
    // yet, and the start's probe (backing::Startup) reads the same rows.
    return absl::OkStatus();
  }));
  ctx.dirty.durable.clear();
  // Rows left: the next sync point must run (FinishRun, the timer, for a
  // mutation's; an atime-only row waits for one: step 23.8); only its
  // ClearDirty, which recomputes the flags, takes them out.
  ABSL_ASSIGN_OR_RETURN(const DirtyFlags flags, CountDirty(ctx));
  ctx.dirty.any = flags.any;
  ctx.dirty.atime = flags.atime;
  ctx.dirty.atime_since =
      flags.atime ? ctx.clock->TimeNow() : absl::InfiniteFuture();
  return count;
}

#undef DCFS_ATTR_ASSIGNMENTS
#undef DCFS_KEEP_STAMP
#undef DCFS_ATTR_VALUES
#undef DCFS_ATTR_COLUMNS
#undef DCFS_ATTR_PLACEHOLDERS

}  // namespace dcfs::cache
