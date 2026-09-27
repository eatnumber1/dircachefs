#include "dcfs/metadata_cache.h"

#include <sys/stat.h>
#include <sys/sysmacros.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "absl/functional/function_ref.h"
#include "absl/status/status.h"
#include "absl/status/status_macros.h"
#include "absl/status/statusor.h"
#include "absl/strings/str_cat.h"
#include "dcfs/context.h"
#include "dcfs/device_id.h"
#include "dcfs/file_handle.h"
#include "dcfs/migrate.h"
#include "dcfs/ret_check.h"
#include "dcfs/sqlite.h"

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

// Runs `stmt`, which must produce at most one row, calling `fn` on that row
// if there is one. Returns whether there was. Always leaves `stmt` reset,
// so no read cursor outlives the call.
absl::StatusOr<bool> ReadOne(Statement &stmt,
                             absl::FunctionRef<absl::Status(Statement &)> fn) {
  bool found = false;
  ABSL_RETURN_IF_ERROR(stmt.ForEachRow([&](Statement &row) -> absl::Status {
    RET_CHECK(!found) << "expected at most one row from: " << row.Sql();
    found = true;
    return fn(row);
  }));
  return found;
}

absl::Status NoInode(InodeId id) {
  return absl::NotFoundError(absl::StrCat("no cached inode ", id));
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
                 "UPDATE directories SET children_complete = 0 "
                 "WHERE inode = ?",
                 dir)
      .status();
}

// The attribute columns, in the order BindAttrs() binds them. A macro
// (rather than a constant) so it can be spliced into SQL string literals,
// which keeps each statement's text a compile-time constant.
#define DCFS_ATTR_ASSIGNMENTS                                              \
  "attrs_valid = 1, mode = ?, nlink = ?, uid = ?, gid = ?, rdev = ?, "     \
  "size = ?, blocks = ?, blksize = ?, atime_s = ?, atime_ns = ?, "         \
  "mtime_s = ?, mtime_ns = ?, ctime_s = ?, ctime_ns = ?, btime_s = ?, "    \
  "btime_ns = ?"
#define DCFS_ATTR_COLUMNS                                                  \
  "mode, nlink, uid, gid, rdev, size, blocks, blksize, atime_s, atime_ns, " \
  "mtime_s, mtime_ns, ctime_s, ctime_ns, btime_s, btime_ns"
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
      Query(ctx, "SELECT inode FROM dentries WHERE parent = ? AND name = ?",
            parent, Blob(name)));
  LookupResult result;
  ABSL_ASSIGN_OR_RETURN(
      bool found, ReadOne(*stmt, [&](Statement &row) {
        std::optional<int64_t> inode = row.Column<std::optional<int64_t>>(0);
        if (inode.has_value()) {
          result = {LookupResult::kFound, *inode};
        } else {
          result = {LookupResult::kNegative, 0};
        }
        return absl::OkStatus();
      }));
  if (!found) result = {LookupResult::kUnknown, 0};
  return result;
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
        attr.valid = row.Column<bool>(0);
        attr.fuse_gen = static_cast<uint32_t>(row.Column<int64_t>(1));
        ABSL_ASSIGN_OR_RETURN(attr.device,
                              DeviceId::Parse(row.Column<std::string>(2)));
        attr.backing_ino = row.Column<uint64_t>(3);
        attr.backing_gen = row.Column<uint64_t>(4);
        // Attribute columns are NULL until first set; Column<> reads NULL
        // as 0, which is what an unset `st` should hold anyway.
        struct stat &st = attr.st;
        st.st_dev = 0;
        st.st_ino = attr.backing_ino;
        st.st_mode = row.Column<int64_t>(5);
        st.st_nlink = row.Column<int64_t>(6);
        st.st_uid = row.Column<int64_t>(7);
        st.st_gid = row.Column<int64_t>(8);
        st.st_rdev = row.Column<uint64_t>(9);
        st.st_size = row.Column<int64_t>(10);
        st.st_blocks = row.Column<int64_t>(11);
        st.st_blksize = row.Column<int64_t>(12);
        st.st_atim = {row.Column<int64_t>(13), row.Column<int64_t>(14)};
        st.st_mtim = {row.Column<int64_t>(15), row.Column<int64_t>(16)};
        st.st_ctim = {row.Column<int64_t>(17), row.Column<int64_t>(18)};
        attr.btime = {row.Column<int64_t>(19), row.Column<int64_t>(20)};
        return absl::OkStatus();
      }));
  if (!found) return NoInode(id);
  return attr;
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
  struct Entry {
    int64_t rowid;
    std::string name;
    InodeId child;
  };
  std::vector<Entry> batch;
  while (true) {
    batch.clear();
    ABSL_ASSIGN_OR_RETURN(
        Statement * stmt,
        Query(ctx,
              "SELECT rowid, name, inode FROM dentries "
              "WHERE parent = ? AND rowid > ? AND inode IS NOT NULL "
              "ORDER BY rowid LIMIT ?",
              dir, cursor, kListDirBatch));
    ABSL_RETURN_IF_ERROR(stmt->ForEachRow([&](Statement &row) {
      batch.push_back({row.Column<int64_t>(0), row.Column<std::string>(1),
                       row.Column<int64_t>(2)});
      return absl::OkStatus();
    }));
    // The statement is reset now, so callbacks may use the cache freely.
    for (const Entry &entry : batch) {
      ABSL_ASSIGN_OR_RETURN(bool more, cb(entry.name, entry.child, entry.rowid));
      if (!more) return absl::OkStatus();
    }
    if (static_cast<int64_t>(batch.size()) < kListDirBatch) {
      return absl::OkStatus();
    }
    cursor = batch.back().rowid;
  }
}

absl::StatusOr<bool> IsDirComplete(Context &ctx, InodeId dir) {
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
    return absl::NotFoundError(absl::StrCat("no cached symlink target for ", id));
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
  ABSL_ASSIGN_OR_RETURN(
      Statement * stmt,
      Query(ctx, "SELECT name FROM xattrs WHERE inode = ? ORDER BY name", id));
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
      Query(ctx, "SELECT value FROM xattrs WHERE inode = ? AND name = ?", id,
            Blob(name)));
  std::optional<std::string> value;
  ABSL_RETURN_IF_ERROR(ReadOne(*stmt, [&](Statement &row) {
                         value = row.Column<std::string>(0);
                         return absl::OkStatus();
                       }).status());
  if (value.has_value()) return value;

  ABSL_ASSIGN_OR_RETURN(std::optional<bool> complete, XattrsComplete(ctx, id));
  if (!complete.has_value()) return NoInode(id);
  if (*complete) {
    return absl::NotFoundError(
        absl::StrCat("inode ", id, " has no xattr ", name));
  }
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
    return absl::NotFoundError(absl::StrCat("no cached handle for inode ", id));
  }
  return *std::move(handle);
}

absl::StatusOr<InodeId> ParentOf(Context &ctx, InodeId dir) {
  if (dir == kRootInode) return kRootInode;
  // LIMIT 2 is enough to tell "one" from "more than one".
  ABSL_ASSIGN_OR_RETURN(
      Statement * stmt,
      Query(ctx, "SELECT parent FROM dentries WHERE inode = ? LIMIT 2", dir));
  std::vector<InodeId> parents;
  ABSL_RETURN_IF_ERROR(stmt->ForEachRow([&](Statement &row) {
    parents.push_back(row.Column<int64_t>(0));
    return absl::OkStatus();
  }));
  if (parents.empty()) {
    return absl::NotFoundError(
        absl::StrCat("no cached dentry for directory ", dir));
  }
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
    return absl::NotFoundError(
        absl::StrCat("no filesystem ", device.ToString()));
  }
  return *std::move(result);
}

// --- Writes ---------------------------------------------------------------

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

    // Every row for this backing inode number: at most one with the wanted
    // generation, plus any left over from before the number was recycled.
    // Collected up front so the cursor is closed before we write.
    ABSL_ASSIGN_OR_RETURN(
        Statement * find,
        Query(ctx,
              "SELECT id, fuse_gen, backing_gen FROM inodes "
              "WHERE device_id = ? AND backing_ino = ?",
              Blob(device), ino));
    std::optional<UpsertResult> existing;
    std::vector<InodeId> stale;
    ABSL_RETURN_IF_ERROR(find->ForEachRow([&](Statement &row) {
      InodeId id = row.Column<int64_t>(0);
      if (row.Column<uint64_t>(2) == backing_gen) {
        existing = UpsertResult{
            .id = id,
            .fuse_gen = static_cast<uint32_t>(row.Column<int64_t>(1)),
            .created = false};
      } else {
        stale.push_back(id);
      }
      return absl::OkStatus();
    }));

    if (existing.has_value()) {
      ABSL_ASSIGN_OR_RETURN(
          Statement * update,
          ctx.db.Prepared("UPDATE inodes SET handle_type = ?, handle = ?, "
                          DCFS_ATTR_ASSIGNMENTS " WHERE id = ?"));
      ABSL_RETURN_IF_ERROR(BindHandle(*update, 1, handle));
      ABSL_RETURN_IF_ERROR(BindAttrs(*update, 3, stx));
      ABSL_RETURN_IF_ERROR(update->Bind(3 + kNumAttrColumns, existing->id));
      ABSL_RETURN_IF_ERROR(update->ExecuteOnce());
      result = *existing;
      return absl::OkStatus();
    }

    // The backing filesystem reused this inode number for a new object: the
    // old rows describe something that no longer exists.
    for (InodeId id : stale) {
      ABSL_RETURN_IF_ERROR(InvalidateInode(ctx, id));
    }

    ABSL_ASSIGN_OR_RETURN(uint32_t fuse_gen, MintFuseGeneration(ctx.db));
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
    result = UpsertResult{.id = ctx.db.LastInsertRowId(),
                          .fuse_gen = fuse_gen,
                          .created = true};
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

// Points (parent, name) at `child` (nullopt for a negative entry). An
// upsert rather than INSERT OR REPLACE: REPLACE deletes and reinserts,
// giving the entry a new rowid, which would make an in-progress ListDir
// (whose cursor is a rowid) see a renamed-over name twice.
absl::Status PutDentry(Context &ctx, InodeId parent, std::string_view name,
                       std::optional<InodeId> child) {
  return Execute(ctx,
                 "INSERT INTO dentries (parent, name, inode) VALUES (?, ?, ?) "
                 "ON CONFLICT (parent, name) DO UPDATE SET inode = "
                 "excluded.inode",
                 parent, Blob(name), child)
      .status();
}

}  // namespace

absl::Status LinkDentry(Context &ctx, InodeId parent, std::string_view name,
                        InodeId child) {
  return ctx.db.Transaction([&]() -> absl::Status {
    ABSL_RETURN_IF_ERROR(RequireInode(ctx, parent));
    ABSL_RETURN_IF_ERROR(RequireInode(ctx, child));
    return PutDentry(ctx, parent, name, child);
  });
}

absl::Status SetNegative(Context &ctx, InodeId parent, std::string_view name) {
  return ctx.db.Transaction([&]() -> absl::Status {
    ABSL_RETURN_IF_ERROR(RequireInode(ctx, parent));
    return PutDentry(ctx, parent, name, std::nullopt);
  });
}

absl::Status UnlinkDentry(Context &ctx, InodeId parent, std::string_view name) {
  return ctx.db.Transaction([&]() -> absl::Status {
    return Execute(ctx, "DELETE FROM dentries WHERE parent = ? AND name = ?",
                   parent, Blob(name))
        .status();
  });
}

absl::Status RenameDentry(Context &ctx, InodeId parent, std::string_view name,
                          InodeId newparent, std::string_view newname) {
  return ctx.db.Transaction([&]() -> absl::Status {
    ABSL_ASSIGN_OR_RETURN(LookupResult source, Lookup(ctx, parent, name));
    if (source.kind != LookupResult::kFound) {
      return absl::NotFoundError(absl::StrCat(
          "no cached positive dentry ", name, " in ", parent, " to rename"));
    }
    if (parent == newparent && name == newname) return absl::OkStatus();
    ABSL_RETURN_IF_ERROR(RequireInode(ctx, newparent));
    ABSL_RETURN_IF_ERROR(
        Execute(ctx, "DELETE FROM dentries WHERE parent = ? AND name = ?",
                parent, Blob(name))
            .status());
    return PutDentry(ctx, newparent, newname, source.id);
  });
}

absl::Status MarkDirComplete(Context &ctx, InodeId dir, bool complete) {
  return ctx.db.Transaction([&]() -> absl::Status {
    ABSL_RETURN_IF_ERROR(RequireInode(ctx, dir));
    return Execute(ctx,
                   "INSERT INTO directories (inode, children_complete) "
                   "VALUES (?, ?) ON CONFLICT (inode) DO UPDATE SET "
                   "children_complete = excluded.children_complete",
                   dir, complete)
        .status();
  });
}

absl::Status MarkUnknown(Context &ctx, InodeId parent,
                         std::span<const std::string> names) {
  return ctx.db.Transaction([&]() -> absl::Status {
    for (const std::string &name : names) {
      ABSL_RETURN_IF_ERROR(
          Execute(ctx, "DELETE FROM dentries WHERE parent = ? AND name = ?",
                  parent, Blob(name))
              .status());
    }
    return MarkIncomplete(ctx, parent);
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
    return absl::OkStatus();
  });
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

absl::Status PutXattr(Context &ctx, InodeId id, std::string_view name,
                      std::string_view value) {
  return Execute(ctx,
                 "INSERT INTO xattrs (inode, name, value) VALUES (?, ?, ?) "
                 "ON CONFLICT (inode, name) DO UPDATE SET value = "
                 "excluded.value",
                 id, Blob(name), Blob(value))
      .status();
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
  return ctx.db.Transaction([&]() -> absl::Status {
    ABSL_RETURN_IF_ERROR(RequireInode(ctx, id));
    ABSL_RETURN_IF_ERROR(
        Execute(ctx, "DELETE FROM xattrs WHERE inode = ?", id).status());
    for (const auto &[name, value] : xattrs) {
      ABSL_RETURN_IF_ERROR(PutXattr(ctx, id, name, value));
    }
    return SetXattrsComplete(ctx, id, true);
  });
}

absl::Status SetXattr(Context &ctx, InodeId id, std::string_view name,
                      std::string_view value) {
  return ctx.db.Transaction([&]() -> absl::Status {
    ABSL_RETURN_IF_ERROR(RequireInode(ctx, id));
    return PutXattr(ctx, id, name, value);
  });
}

absl::Status RemoveXattr(Context &ctx, InodeId id, std::string_view name) {
  return ctx.db.Transaction([&]() -> absl::Status {
    return Execute(ctx, "DELETE FROM xattrs WHERE inode = ? AND name = ?", id,
                   Blob(name))
        .status();
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
  return ctx.db.Transaction([&]() -> absl::Status {
    ABSL_RETURN_IF_ERROR(RequireInode(ctx, id));
    // Parents first: once the dentries are gone we can no longer find them.
    ABSL_RETURN_IF_ERROR(
        Execute(ctx,
                "UPDATE directories SET children_complete = 0 "
                "WHERE inode IN (SELECT parent FROM dentries WHERE inode = ?)",
                id)
            .status());
    // Deleted explicitly, before the row: the schema's ON DELETE SET NULL
    // would otherwise turn them into negative entries, claiming those names
    // are absent when in fact we no longer know.
    ABSL_RETURN_IF_ERROR(
        Execute(ctx, "DELETE FROM dentries WHERE inode = ?", id).status());
    return Execute(ctx, "DELETE FROM inodes WHERE id = ?", id).status();
  });
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
      return absl::AlreadyExistsError(
          absl::StrCat("filesystem ", device.ToString(), " already exists"));
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
    if (!fs.parent_inode.has_value() || !fs.boundary_name.has_value()) {
      return absl::OkStatus();
    }
    // The cascade above deleted the filesystem's root inode, and the
    // schema's ON DELETE SET NULL turned the boundary dentry pointing at it
    // into a negative entry. The mount point is not absent, merely
    // forgotten, so drop that entry and make the parent rediscover it.
    ABSL_RETURN_IF_ERROR(
        Execute(ctx,
                "DELETE FROM dentries "
                "WHERE parent = ? AND name = ? AND inode IS NULL",
                *fs.parent_inode, Blob(*fs.boundary_name))
            .status());
    return MarkIncomplete(ctx, *fs.parent_inode);
  });
}

#undef DCFS_ATTR_ASSIGNMENTS
#undef DCFS_ATTR_COLUMNS
#undef DCFS_ATTR_PLACEHOLDERS

}  // namespace dcfs::cache
