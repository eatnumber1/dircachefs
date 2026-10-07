#include "dcfs/testonly/invariant_checker.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "absl/container/flat_hash_set.h"
#include "absl/log/log.h"
#include "absl/status/status.h"
#include "absl/status/status_macros.h"
#include "absl/status/statusor.h"
#include "absl/strings/str_cat.h"
#include "dcfs/context.h"
#include "dcfs/dir_cache_fs.h"
#include "dcfs/escape.h"
#include "dcfs/metadata_cache.h"
#include "dcfs/protocol_events.h"
#include "dcfs/sqlite.h"
#include "dcfs/status.h"
#include "dcfs/syscalls.h"
#include "dcfs/testonly/dir_cache_fs_peer.h"
#include "sqlite3.h"

namespace dcfs::testonly {
namespace {

// The invariants' names, as a violation reports them (see the header).
constexpr std::string_view kNoTransactionAtBackingCall =
    "no-transaction-at-backing-call";
constexpr std::string_view kNoTransactionAtRequestEnd =
    "no-transaction-at-request-end";
constexpr std::string_view kTriState = "tri-state";
constexpr std::string_view kIdentity = "identity";
constexpr std::string_view kDirtySet = "dirty-set";
constexpr std::string_view kWritableOpen = "writable-open";
constexpr std::string_view kLookupCount = "lookup-count";
constexpr std::string_view kHeldFds = "held-fds";
constexpr std::string_view kRemovedRecord = "removed-record";

// "Some attribute column is NULL", for an inodes row. A macro, as
// metadata_cache.cc's DCFS_ATTR_COLUMNS, so that it splices into a literal.
#define DCFS_ATTR_MISSING                                                 \
  "(mode IS NULL OR nlink IS NULL OR uid IS NULL OR gid IS NULL OR "      \
  "rdev IS NULL OR size IS NULL OR blocks IS NULL OR blksize IS NULL OR " \
  "atime_s IS NULL OR atime_ns IS NULL OR mtime_s IS NULL OR "            \
  "mtime_ns IS NULL OR ctime_s IS NULL OR ctime_ns IS NULL OR "           \
  "btime_s IS NULL OR btime_ns IS NULL)"

// A violation of `invariant`: its name, then what was found.
template <typename... Args>
absl::Status Violation(std::string_view invariant, const Args &...what) {
  return FailedPreconditionErrorBuilder()
         << absl::StrCat(invariant, ": ", what...);
}

absl::StatusOr<bool> HasDirtyRow(Context &ctx, InodeId id) {
  ABSL_ASSIGN_OR_RETURN(sqlite3::Statement * stmt,
                        ctx.db.Prepared("SELECT 1 FROM dirty WHERE inode = ?"));
  ABSL_RETURN_IF_ERROR(stmt->Bind(1, id));
  bool found = false;
  ABSL_RETURN_IF_ERROR(stmt->ForEachRow([&](sqlite3::Statement &) {
    found = true;
    return absl::OkStatus();
  }));
  return found;
}

// No transaction open, and no statement part way through its rows (a read
// cursor holds a read transaction: a snapshot of the database).
absl::Status NoTransaction(Context &ctx, std::string_view invariant) {
  if (ctx.db.InTransaction()) {
    return Violation(invariant, "a transaction is open");
  }
  ::sqlite3 *db = ctx.db.Get();
  for (sqlite3_stmt *stmt = sqlite3_next_stmt(db, nullptr); stmt != nullptr;
       stmt = sqlite3_next_stmt(db, stmt)) {
    if (sqlite3_stmt_busy(stmt) != 0) {
      return Violation(invariant, "a statement is part way through its rows: ",
                       sqlite3_sql(stmt));
    }
  }
  return absl::OkStatus();
}

// dirty-set: every inode with a mutation in flight has its dirty row.
absl::Status InFlightAreDirty(Context &ctx) {
  for (const auto &[id, count] : ctx.fills.inflight) {
    ABSL_ASSIGN_OR_RETURN(bool dirty, HasDirtyRow(ctx, id));
    if (!dirty) {
      return Violation(kDirtySet, "inode ", id,
                       " has a mutation in flight but no dirty row");
    }
  }
  return absl::OkStatus();
}

// dirty-set: Context::dirty.any false means the table is empty.
absl::Status NoneDirtyUnlessAny(Context &ctx) {
  if (ctx.dirty.any) return absl::OkStatus();
  ABSL_ASSIGN_OR_RETURN(sqlite3::Statement * stmt,
                        ctx.db.Prepared("SELECT 1 FROM dirty LIMIT 1"));
  bool found = false;
  ABSL_RETURN_IF_ERROR(stmt->ForEachRow([&](sqlite3::Statement &) {
    found = true;
    return absl::OkStatus();
  }));
  if (!found) return absl::OkStatus();
  return Violation(kDirtySet,
                   "Context::dirty.any is false but the dirty table has rows");
}

std::string_view OpName(events::Op op) {
  using events::Op;
  switch (op) {
    case Op::kOther: return "OTHER";
    case Op::kLookup: return "LOOKUP";
    case Op::kGetattr: return "GETATTR";
    case Op::kSetattr: return "SETATTR";
    case Op::kReadlink: return "READLINK";
    case Op::kMknod: return "MKNOD";
    case Op::kMkdir: return "MKDIR";
    case Op::kUnlink: return "UNLINK";
    case Op::kRmdir: return "RMDIR";
    case Op::kSymlink: return "SYMLINK";
    case Op::kRename: return "RENAME";
    case Op::kLink: return "LINK";
    case Op::kOpen: return "OPEN";
    case Op::kRead: return "READ";
    case Op::kWrite: return "WRITE";
    case Op::kFlush: return "FLUSH";
    case Op::kRelease: return "RELEASE";
    case Op::kFsync: return "FSYNC";
    case Op::kOpendir: return "OPENDIR";
    case Op::kReaddir: return "READDIR";
    case Op::kReaddirplus: return "READDIRPLUS";
    case Op::kReleasedir: return "RELEASEDIR";
    case Op::kFsyncdir: return "FSYNCDIR";
    case Op::kStatfs: return "STATFS";
    case Op::kSetxattr: return "SETXATTR";
    case Op::kGetxattr: return "GETXATTR";
    case Op::kListxattr: return "LISTXATTR";
    case Op::kRemovexattr: return "REMOVEXATTR";
    case Op::kAccess: return "ACCESS";
    case Op::kCreate: return "CREATE";
    case Op::kFallocate: return "FALLOCATE";
    case Op::kCopyFileRange: return "COPY_FILE_RANGE";
    case Op::kIoctl: return "IOCTL";
    case Op::kTmpfile: return "TMPFILE";
    case Op::kLinkTmpfile: return "LINK";
    case Op::kForget: return "FORGET";
    case Op::kBatchForget: return "BATCH_FORGET";
  }
  return "?";
}

}  // namespace

InvariantChecker::InvariantChecker(int console_fd) : console_fd_(console_fd) {}

InvariantChecker::~InvariantChecker() {
  if (db_ != nullptr) sqlite3_update_hook(db_, nullptr, nullptr);
}

void InvariantChecker::Attach(Context &ctx) {
  ::sqlite3 *db = ctx.db.Get();
  if (db == db_ || db == nullptr) return;
  if (db_ != nullptr) sqlite3_update_hook(db_, nullptr, nullptr);
  db_ = db;
  sqlite3_update_hook(db_, &InvariantChecker::OnUpdate, this);
}

void InvariantChecker::OnUpdate(void *self, int op, const char *db,
                                const char *table, sqlite3_int64 rowid) {
  auto *checker = static_cast<InvariantChecker *>(self);
  if (std::string_view(db) != "main") return;
  const std::string_view name(table);
  if (name == "inodes") {
    checker->inodes_.insert(rowid);
  } else if (name == "dentries") {
    checker->dentries_.insert(rowid);
  } else if (name == "stubs") {
    checker->stubs_.insert(rowid);
  } else if (name == "dirty") {
    checker->dirty_.insert(rowid);
  } else if (name == "xattrs") {
    checker->xattrs_.insert(rowid);
  } else if (name == "directories") {
    checker->directories_.insert(rowid);
  } else if (name == "symlinks") {
    checker->symlinks_.insert(rowid);
  }
}

std::string InvariantChecker::Where() const {
  if (frames_.empty()) return "outside any request";
  std::string where;
  for (auto it = frames_.rbegin(); it != frames_.rend(); ++it) {
    absl::StrAppend(&where, where.empty() ? "in " : ", inside ");
    if (!it->label.empty()) {
      absl::StrAppend(&where, it->label);
    } else {
      absl::StrAppend(&where, "request ", OpName(it->op), " nodeid ",
                      it->nodeid);
    }
  }
  return where;
}

void InvariantChecker::Fail(const absl::Status &violation) {
  const std::string what = absl::StrCat(
      violation.code() == absl::StatusCode::kFailedPrecondition
          ? ""
          : "invariant-checker: a check could not run: ",
      violation.message(), " (", Where(), ")");
  if (console_fd_ >= 0) {
    const std::string line =
        absl::StrCat("DCFS-INVARIANT-VIOLATION ", what, "\n");
    // Best effort: the abort below says it all again on stderr.
    (void)syscalls::write(console_fd_, line.data(), line.size());
  }
  LOG(FATAL) << "invariant violated: " << what;
}

void InvariantChecker::BackingCall(Context &ctx, std::string_view what) {
  Attach(ctx);
  absl::Status status = CheckBackingCall(ctx);
  if (!status.ok()) {
    Fail(absl::Status(status.code(),
                      absl::StrCat(status.message(), ", at ", what)));
  }
}

void InvariantChecker::RequestBegin(Context &ctx, const DirCacheFS &fs,
                                    const events::Request &request) {
  Attach(ctx);
  Frame frame{.op = request.op, .nodeid = static_cast<uint64_t>(request.ino)};
  if (request.ino != 0) frame.ids.push_back(request.ino);
  if (request.newparent != 0) frame.ids.push_back(request.newparent);
  frames_.push_back(std::move(frame));
}

void InvariantChecker::RequestEnd(Context &ctx, const DirCacheFS &fs,
                                  const events::Request &request) {
  Attach(ctx);
  FailIfNotOk(CheckChanged(ctx, &fs, frames_.back().ids));
  frames_.pop_back();
}

void InvariantChecker::Forgetting(Context &ctx, const DirCacheFS &fs,
                                  uint64_t ino, uint64_t nlookup) {
  Attach(ctx);
  const InodeId id = static_cast<InodeId>(ino);
  // What this request forgets of `id` so far (a BATCH_FORGET could name it
  // twice), before DirCacheFS counts any of it down.
  uint64_t forgotten = nlookup;
  if (!frames_.empty()) {
    frames_.back().ids.push_back(id);
    forgotten = frames_.back().forgotten[id] += nlookup;
  }
  // lookup-count: the count never goes below 0. DirCacheFS logs such a
  // FORGET and drops the count (DropLookups); here it is a violation: some
  // reply that handed out the nodeid was not counted.
  const auto &lookups = DirCacheFSPeer::Lookups(fs);
  auto it = lookups.find(id);
  const uint64_t counted = it == lookups.end() ? 0 : it->second;
  if (counted < forgotten) {
    Fail(Violation(kLookupCount, "FORGET of ", forgotten,
                   " lookups of nodeid ", ino, ", but ", counted,
                   " counted"));
  }
}

void InvariantChecker::RunStarted(Context &ctx) {
  Attach(ctx);
  frames_.push_back(Frame{.label = "StartRun"});
  // A run starts with no open file: main.cc makes the DirCacheFS after
  // StartRun. The harness restarts the run under a live DirCacheFS to
  // stand for a crash, whose opens are the crashed run's, not this one's:
  // they are left out until they are released.
  stale_opens_.clear();
  if (ctx.open_for_write != nullptr) {
    for (InodeId id : *ctx.open_for_write) stale_opens_.insert(id);
  }
  FailIfNotOk(CheckAll(ctx, nullptr));
  frames_.pop_back();
}

bool InvariantChecker::OpenForWrite(const Context &ctx, InodeId id) const {
  return ctx.open_for_write != nullptr && ctx.open_for_write->contains(id) &&
         !stale_opens_.contains(id);
}

void InvariantChecker::ForgetReleasedStaleOpens(const Context &ctx) {
  absl::erase_if(stale_opens_, [&](InodeId id) {
    return ctx.open_for_write == nullptr || !ctx.open_for_write->contains(id);
  });
}

void InvariantChecker::Destroyed(Context &ctx, const DirCacheFS &fs) {
  Attach(ctx);
  frames_.push_back(Frame{.label = "DESTROY"});
  FailIfNotOk(CheckEverything(ctx, &fs, /*destroyed=*/true));
  frames_.pop_back();
}

absl::Status InvariantChecker::CheckBackingCall(Context &ctx) {
  ABSL_RETURN_IF_ERROR(NoTransaction(ctx, kNoTransactionAtBackingCall));
  return InFlightAreDirty(ctx);
}

absl::Status InvariantChecker::CheckChanged(Context &ctx, const DirCacheFS *fs,
                                            std::span<const InodeId> ids) {
  ABSL_RETURN_IF_ERROR(NoTransaction(ctx, kNoTransactionAtRequestEnd));
  // Taken (and forgotten) first: the checker's own queries only read, but
  // a violation must not leave rows behind for the next check either.
  absl::flat_hash_set<int64_t> inodes = std::exchange(inodes_, {});
  absl::flat_hash_set<int64_t> dentries = std::exchange(dentries_, {});
  absl::flat_hash_set<int64_t> stubs = std::exchange(stubs_, {});
  absl::flat_hash_set<int64_t> dirty = std::exchange(dirty_, {});
  absl::flat_hash_set<int64_t> xattrs = std::exchange(xattrs_, {});
  absl::flat_hash_set<int64_t> directories = std::exchange(directories_, {});
  absl::flat_hash_set<int64_t> symlinks = std::exchange(symlinks_, {});

  ForgetReleasedStaleOpens(ctx);
  ABSL_RETURN_IF_ERROR(InFlightAreDirty(ctx));
  ABSL_RETURN_IF_ERROR(NoneDirtyUnlessAny(ctx));

  // The inodes to look at: those the request named, and every inode whose
  // rows (its own, a dentry in it or naming it, its xattrs, directory,
  // symlink or dirty rows) it changed.
  absl::flat_hash_set<InodeId> interest;
  for (InodeId id : ids) interest.insert(id);
  for (const auto *rows : {&inodes, &dirty, &directories, &symlinks}) {
    for (int64_t id : *rows) interest.insert(id);
  }
  for (int64_t rowid : dentries) {
    ABSL_RETURN_IF_ERROR(CheckDentry(ctx, rowid, interest));
  }
  for (int64_t id : stubs) ABSL_RETURN_IF_ERROR(CheckStub(ctx, id));
  for (int64_t rowid : xattrs) {
    ABSL_ASSIGN_OR_RETURN(sqlite3::Statement * stmt,
                          ctx.db.Prepared("SELECT inode FROM xattrs "
                                          "WHERE rowid = ?"));
    ABSL_RETURN_IF_ERROR(stmt->Bind(1, rowid));
    ABSL_RETURN_IF_ERROR(stmt->ForEachRow([&](sqlite3::Statement &row) {
      interest.insert(row.Column<int64_t>(0));
      return absl::OkStatus();
    }));
  }
  for (InodeId id : interest) {
    ABSL_RETURN_IF_ERROR(CheckInode(ctx, fs, id, inodes.contains(id)));
  }
  if (fs != nullptr) {
    ABSL_RETURN_IF_ERROR(
        CheckBookkeeping(ctx, *fs, &interest, /*destroyed=*/false));
  }
  return absl::OkStatus();
}

absl::Status InvariantChecker::CheckAll(Context &ctx, const DirCacheFS *fs) {
  return CheckEverything(ctx, fs, /*destroyed=*/false);
}

absl::Status InvariantChecker::CheckEverything(Context &ctx,
                                               const DirCacheFS *fs,
                                               bool destroyed) {
  ABSL_RETURN_IF_ERROR(NoTransaction(ctx, kNoTransactionAtRequestEnd));
  inodes_.clear();
  dentries_.clear();
  stubs_.clear();
  dirty_.clear();
  xattrs_.clear();
  directories_.clear();
  symlinks_.clear();
  ForgetReleasedStaleOpens(ctx);
  ABSL_RETURN_IF_ERROR(InFlightAreDirty(ctx));
  ABSL_RETURN_IF_ERROR(NoneDirtyUnlessAny(ctx));

  // Every inode row's own columns.
  {
    ABSL_ASSIGN_OR_RETURN(
        sqlite3::Statement * stmt,
        ctx.db.Prepared("SELECT id, attrs_valid, fuse_gen, nlink, "
                        DCFS_ATTR_MISSING " FROM inodes"));
    absl::Status found;
    ABSL_RETURN_IF_ERROR(stmt->ForEachRow([&](sqlite3::Statement &row) {
      if (found.ok()) found = CheckInodeRow(ctx, fs, row);
      return absl::OkStatus();
    }));
    ABSL_RETURN_IF_ERROR(found);
  }
  // Every dentry that is refused without a stub, or has a stub without
  // being refused; every stub without a refused dentry.
  {
    ABSL_ASSIGN_OR_RETURN(
        sqlite3::Statement * stmt,
        ctx.db.Prepared(
            "SELECT rowid FROM dentries AS d WHERE (d.state = 'refused') != "
            "EXISTS (SELECT 1 FROM stubs AS s WHERE s.parent = d.parent AND "
            "s.name = d.name)"));
    std::vector<int64_t> rowids;
    ABSL_RETURN_IF_ERROR(stmt->ForEachRow([&](sqlite3::Statement &row) {
      rowids.push_back(row.Column<int64_t>(0));
      return absl::OkStatus();
    }));
    absl::flat_hash_set<InodeId> unused;
    for (int64_t rowid : rowids) {
      ABSL_RETURN_IF_ERROR(CheckDentry(ctx, rowid, unused));
    }
  }
  {
    ABSL_ASSIGN_OR_RETURN(sqlite3::Statement * stmt,
                          ctx.db.Prepared("SELECT id FROM stubs"));
    std::vector<int64_t> ids;
    ABSL_RETURN_IF_ERROR(stmt->ForEachRow([&](sqlite3::Statement &row) {
      ids.push_back(row.Column<int64_t>(0));
      return absl::OkStatus();
    }));
    for (int64_t id : ids) ABSL_RETURN_IF_ERROR(CheckStub(ctx, id));
  }
  // Every inode with in-memory state about it.
  absl::flat_hash_set<InodeId> interest;
  for (InodeId id : ctx.dirty.durable) interest.insert(id);
  if (ctx.open_for_write != nullptr) {
    for (InodeId id : *ctx.open_for_write) interest.insert(id);
  }
  // (Every row's columns were checked above.)
  for (InodeId id : interest) {
    ABSL_RETURN_IF_ERROR(CheckInode(ctx, fs, id, /*row_changed=*/false));
  }
  if (fs != nullptr) {
    ABSL_RETURN_IF_ERROR(CheckBookkeeping(ctx, *fs, nullptr, destroyed));
  }
  return absl::OkStatus();
}

absl::Status InvariantChecker::CheckInode(Context &ctx, const DirCacheFS *fs,
                                          InodeId id, bool row_changed) {
  // A row's own columns can only have broken their rules when it changed
  // (the update hook saw it, or the full check reads it); what else needs
  // the row is being open for writing.
  bool exists = false;
  if (row_changed || OpenForWrite(ctx, id)) {
    ABSL_ASSIGN_OR_RETURN(
        sqlite3::Statement * stmt,
        ctx.db.Prepared("SELECT id, attrs_valid, fuse_gen, nlink, "
                        DCFS_ATTR_MISSING " FROM inodes WHERE id = ?"));
    ABSL_RETURN_IF_ERROR(stmt->Bind(1, id));
    absl::Status found;
    ABSL_RETURN_IF_ERROR(stmt->ForEachRow([&](sqlite3::Statement &row) {
      exists = true;
      found = CheckInodeRow(ctx, fs, row);
      return absl::OkStatus();
    }));
    ABSL_RETURN_IF_ERROR(found);
  }
  const bool removed = fs != nullptr && DirCacheFSPeer::IsRemoved(*fs, id);
  if (exists && !removed && OpenForWrite(ctx, id)) {
    ABSL_ASSIGN_OR_RETURN(bool dirty, HasDirtyRow(ctx, id));
    if (!dirty) {
      return Violation(kWritableOpen, "inode ", id,
                       " is open for writing but has no dirty row");
    }
  }
  if (ctx.dirty.durable.contains(id)) {
    ABSL_ASSIGN_OR_RETURN(bool dirty, HasDirtyRow(ctx, id));
    if (!dirty) {
      return Violation(kDirtySet, "inode ", id,
                       " is in Context::dirty.durable but has no dirty row");
    }
  }
  return absl::OkStatus();
}

absl::Status InvariantChecker::CheckInodeRow(const Context &ctx,
                                             const DirCacheFS *fs,
                                             sqlite3::Statement &row) {
  const InodeId id = row.Column<int64_t>(0);
  const bool valid = row.Column<bool>(1);
  const int64_t fuse_gen = row.Column<int64_t>(2);
  const std::optional<int64_t> nlink = row.Column<std::optional<int64_t>>(3);
  const bool missing = row.Column<bool>(4);
  if (valid && missing) {
    return Violation(kTriState, "inode ", id,
                     ": attributes recorded as current with a column NULL");
  }
  if (valid && nlink.value_or(0) <= 0) {
    return Violation(kTriState, "inode ", id,
                     ": attributes recorded as current with nlink ",
                     nlink.value_or(0));
  }
  if (id != cache::kRootInode && (fuse_gen <= 0 || fuse_gen > 0xffffffff)) {
    return Violation(kIdentity, "inode ", id, " has FUSE generation ",
                     fuse_gen, " (only the root's is 0; others are 1 to "
                               "2^32-1)");
  }
  const bool removed = fs != nullptr && DirCacheFSPeer::IsRemoved(*fs, id);
  if (valid && !removed && OpenForWrite(ctx, id)) {
    return Violation(kWritableOpen, "inode ", id,
                     " is open for writing but its attributes are recorded "
                     "as current");
  }
  return absl::OkStatus();
}

absl::Status InvariantChecker::CheckDentry(
    Context &ctx, int64_t rowid, absl::flat_hash_set<InodeId> &interest) {
  ABSL_ASSIGN_OR_RETURN(
      sqlite3::Statement * stmt,
      ctx.db.Prepared("SELECT d.parent, d.name, d.state, d.inode, EXISTS "
                      "(SELECT 1 FROM stubs AS s WHERE s.parent = d.parent "
                      "AND s.name = d.name) FROM dentries AS d "
                      "WHERE d.rowid = ?"));
  ABSL_RETURN_IF_ERROR(stmt->Bind(1, rowid));
  absl::Status found;
  ABSL_RETURN_IF_ERROR(stmt->ForEachRow([&](sqlite3::Statement &row) {
    const InodeId parent = row.Column<int64_t>(0);
    const std::string name = row.Column<std::string>(1);
    const std::string state = row.Column<std::string>(2);
    const std::optional<int64_t> inode = row.Column<std::optional<int64_t>>(3);
    const bool stub = row.Column<bool>(4);
    interest.insert(parent);
    if (inode.has_value()) interest.insert(*inode);
    if (state == "refused" && !stub) {
      found = Violation(kTriState, "dentry \"", EscapeBytes(name),
                        "\" of inode ", parent,
                        " is refused but has no stub");
    } else if (state != "refused" && stub) {
      found = Violation(kTriState, "dentry \"", EscapeBytes(name),
                        "\" of inode ", parent, " is ", state,
                        " but has a stub");
    }
    return absl::OkStatus();
  }));
  return found;
}

absl::Status InvariantChecker::CheckStub(Context &ctx, int64_t id) {
  ABSL_ASSIGN_OR_RETURN(
      sqlite3::Statement * stmt,
      ctx.db.Prepared("SELECT s.parent, s.name, d.state FROM stubs AS s "
                      "LEFT JOIN dentries AS d ON d.parent = s.parent AND "
                      "d.name = s.name WHERE s.id = ?"));
  ABSL_RETURN_IF_ERROR(stmt->Bind(1, id));
  absl::Status found;
  ABSL_RETURN_IF_ERROR(stmt->ForEachRow([&](sqlite3::Statement &row) {
    const std::optional<std::string> state =
        row.Column<std::optional<std::string>>(2);
    if (state != "refused") {
      found = Violation(kTriState, "stub ", static_cast<uint64_t>(id),
                        " of dentry \"",
                        EscapeBytes(row.Column<std::string>(1)),
                        "\" of inode ", row.Column<int64_t>(0),
                        " whose dentry is ", state.value_or("missing"));
    }
    return absl::OkStatus();
  }));
  return found;
}

absl::Status InvariantChecker::CheckBookkeeping(
    const Context &ctx, const DirCacheFS &fs,
    const absl::flat_hash_set<InodeId> *interest, bool destroyed) {
  using Peer = DirCacheFSPeer;
  const auto &lookups = Peer::Lookups(fs);
  const auto &written = Peer::Written(fs);
  const auto &open_for_write = Peer::OpenForWrite(fs);
  // Calls `each` for every id of `all` (a map or set keyed by inode id)
  // that is to be looked at: all of them, or those in `interest`.
  auto for_each_id = [&](const auto &all, size_t limit,
                         auto each) -> absl::Status {
    if (interest == nullptr || all.size() <= limit) {
      for (const auto &entry : all) {
        if constexpr (requires { entry.first; }) {
          ABSL_RETURN_IF_ERROR(each(entry.first));
        } else {
          ABSL_RETURN_IF_ERROR(each(entry));
        }
      }
      return absl::OkStatus();
    }
    for (InodeId id : *interest) {
      if (all.contains(id)) {
        ABSL_RETURN_IF_ERROR(each(id));
      }
    }
    return absl::OkStatus();
  };

  // lookup-count: a count of 0 is erased (DropLookups), never kept.
  ABSL_RETURN_IF_ERROR(for_each_id(lookups, 0, [&](InodeId id) {
    if (lookups.at(id) > 0) return absl::OkStatus();
    return Violation(kLookupCount, "nodeid ", static_cast<uint64_t>(id),
                     " has a lookup count of 0");
  }));

  // held-fds.
  const size_t held = Peer::HeldFds(fs);
  if (held > Peer::MaxHeldFds(fs)) {
    return Violation(kHeldFds, "held_fds_ is ", held, ", above max_held_fds_ ",
                     Peer::MaxHeldFds(fs));
  }
  if (interest == nullptr || written.size() <= kRecountLimit) {
    size_t holding = 0;
    for (const auto &[id, fd] : written) holding += fd.has_value() ? 1 : 0;
    if (holding != held) {
      return Violation(kHeldFds, "held_fds_ is ", held, " but ", holding,
                       " entries of written_ hold a descriptor");
    }
  }

  // removed-record.
  if (interest == nullptr || Peer::RemovedCount(fs) <= kRecountLimit) {
    absl::Status found;
    Peer::ForEachRemoved(fs, [&](InodeId id) {
      if (found.ok()) found = CheckRemoved(fs, id);
    });
    ABSL_RETURN_IF_ERROR(found);
  } else {
    for (InodeId id : *interest) {
      if (Peer::IsRemoved(fs, id)) {
        ABSL_RETURN_IF_ERROR(CheckRemoved(fs, id));
      }
    }
  }

  // writable-open.
  if (ctx.open_for_write != &open_for_write) {
    return Violation(kWritableOpen,
                     "Context::open_for_write is not DirCacheFS's set");
  }
  ABSL_RETURN_IF_ERROR(
      for_each_id(open_for_write, kRecountLimit, [&](InodeId id) {
        // DirCacheFS::Destroy reconciles and empties written_ (no FORGET
        // follows), also of a file still open (after a lazy unmount).
        if (destroyed || Peer::IsRemoved(fs, id) || written.contains(id)) {
          return absl::OkStatus();
        }
        return Violation(kWritableOpen, "inode ", id,
                         " is open for writing but not in written_");
      }));
  auto check_shared = [&](InodeId id) -> absl::Status {
    std::optional<Peer::Shared> shared = Peer::SharedFile(fs, id);
    if (!shared.has_value()) return absl::OkStatus();
    if (shared->refs <= 0 || shared->writable_refs < 0 ||
        shared->writable_refs > shared->refs) {
      return Violation(kWritableOpen, "inode ", id,
                       "'s shared backing file has refs ", shared->refs,
                       " and writable_refs ", shared->writable_refs);
    }
    if (shared->writable_refs > 0 && !open_for_write.contains(id)) {
      return Violation(kWritableOpen, "inode ", id,
                       " has writable opens but is not open for writing");
    }
    return absl::OkStatus();
  };
  if (interest == nullptr) {
    absl::Status found;
    Peer::ForEachSharedFile(fs, [&](InodeId id, const Peer::Shared &) {
      if (found.ok()) found = check_shared(id);
    });
    ABSL_RETURN_IF_ERROR(found);
  } else {
    for (InodeId id : *interest) ABSL_RETURN_IF_ERROR(check_shared(id));
  }
  return absl::OkStatus();
}

absl::Status InvariantChecker::CheckRemoved(const DirCacheFS &fs, InodeId id) {
  const auto &lookups = DirCacheFSPeer::Lookups(fs);
  auto it = lookups.find(id);
  if (it == lookups.end() || it->second == 0) {
    return Violation(kRemovedRecord, "nodeid ", static_cast<uint64_t>(id),
                     " has a removed record but the kernel holds no lookup "
                     "of it");
  }
  if (DirCacheFSPeer::Written(fs).contains(id)) {
    return Violation(kRemovedRecord, "nodeid ", static_cast<uint64_t>(id),
                     " has both a removed record and a written_ entry");
  }
  return absl::OkStatus();
}

}  // namespace dcfs::testonly
