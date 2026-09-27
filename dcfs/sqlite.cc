#include "dcfs/sqlite.h"

#include <bit>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include "absl/cleanup/cleanup.h"
#include "absl/log/log.h"
#include "absl/status/status.h"
#include "absl/status/status_builder.h"
#include "absl/status/status_macros.h"
#include "absl/status/statusor.h"
#include "absl/strings/cord.h"
#include "absl/strings/numbers.h"
#include "absl/strings/str_cat.h"
#include "dcfs/ret_check.h"

namespace dcfs {
namespace sqlite3 {

namespace {

// Builds the Status for a (nonzero) sqlite3 extended result code, with
// `message` (typically from sqlite3_errmsg) as the Status's message and the
// extended code attached as a payload (see GetSqliteCodeFromStatus).
absl::Status MakeSqliteStatus(int extended_code, std::string_view message) {
  absl::Status status(Sqlite3ErrorCodeToCanonical(extended_code), message);
  status.SetPayload(kSqliteTypeUrl, absl::Cord(absl::StrCat(extended_code)));
  return status;
}

}  // namespace

absl::StatusCode Sqlite3ErrorCodeToCanonical(int err) {
  // With SQLITE_OPEN_EXRESCODE (always set by ConnectionFactory::Open), `err`
  // may be an extended result code; its low byte is always the primary
  // result code. See https://www.sqlite.org/rescode.html.
  switch (err & 0xFF) {
    case SQLITE_OK:
    case SQLITE_ROW:
    case SQLITE_DONE:
      return absl::StatusCode::kOk;
    case SQLITE_BUSY:
    case SQLITE_LOCKED:
      return absl::StatusCode::kUnavailable;
    case SQLITE_NOTFOUND:
      return absl::StatusCode::kNotFound;
    case SQLITE_MISUSE:
      return absl::StatusCode::kInternal;
    case SQLITE_RANGE:
    case SQLITE_MISMATCH:
      return absl::StatusCode::kInvalidArgument;
    case SQLITE_FULL:
      return absl::StatusCode::kResourceExhausted;
    case SQLITE_CANTOPEN:
    case SQLITE_IOERR:
    case SQLITE_READONLY:
      return absl::StatusCode::kUnavailable;
    case SQLITE_CONSTRAINT:
      switch (err) {
        case SQLITE_CONSTRAINT_UNIQUE:
        case SQLITE_CONSTRAINT_PRIMARYKEY:
          return absl::StatusCode::kAlreadyExists;
        default:
          return absl::StatusCode::kFailedPrecondition;
      }
    default:
      return absl::StatusCode::kInternal;
  }
}

absl::Status Sqlite3ErrorCodeToStatus(int err) {
  if (err == SQLITE_OK) return absl::OkStatus();
  return MakeSqliteStatus(err, sqlite3_errstr(err));
}

absl::StatusOr<int> GetSqliteCodeFromStatus(const absl::Status &status) {
  std::optional<absl::Cord> payload = status.GetPayload(kSqliteTypeUrl);
  if (!payload) {
    return absl::NotFoundError(
        absl::StrCat(
          "Cannot get sqlite code from Status: no payload in Status: ",
          status));
  }
  int code = 0;
  if (!absl::SimpleAtoi(std::string(*payload), &code)) {
    return absl::InternalError(
        absl::StrCat("Malformed sqlite status payload: ", *payload));
  }
  return code;
}

// --- Statement --------------------------------------------------------

Statement::Statement(sqlite3_stmt &stmt) : stmt_(&stmt) {}

Statement::Statement(Statement &&o) : Statement() { *this = std::move(o); }

Statement &Statement::operator=(Statement &&o) {
  using std::swap;
  swap(stmt_, o.stmt_);
  return *this;
}

Statement::~Statement() {
  if (stmt_ == nullptr) return;
  // sqlite3_finalize's return code just mirrors the error (if any) of the
  // statement's last Step(), which was already surfaced to the caller from
  // Step() itself; nothing new to report here, and destructors can't return
  // errors anyway.
  sqlite3_finalize(stmt_);
}

absl::StatusOr<Statement> Statement::Prepare(
    Connection &db, std::string_view sql, unsigned int flags) {
  sqlite3_stmt *stmt = nullptr;
  const char *tail = nullptr;
  int rc = sqlite3_prepare_v3(
      db.Get(), sql.data(), static_cast<int>(sql.size()), flags, &stmt,
      &tail);
  if (rc != SQLITE_OK) {
    return absl::StatusBuilder(db.LastErrorStatus())
        << "; while preparing SQL: " << sql;
  }
  RET_CHECK_NE(stmt, nullptr);
  if (tail != sql.data() + sql.size()) {
    // Statement::Prepare only prepares a single SQL statement.
    sqlite3_finalize(stmt);
    return absl::InvalidArgumentError(
        absl::StrCat("Extra SQL text after first statement: ", tail));
  }
  return Statement(*stmt);
}

absl::Status Statement::StatusFromRc(int rc) const {
  if (rc == SQLITE_OK) return absl::OkStatus();
  ::sqlite3 *db = sqlite3_db_handle(stmt_);
  if (db == nullptr) return Sqlite3ErrorCodeToStatus(rc);
  int code = sqlite3_extended_errcode(db);
  return MakeSqliteStatus(code, sqlite3_errmsg(db));
}

absl::Status Statement::Reset() { return StatusFromRc(sqlite3_reset(stmt_)); }

absl::Status Statement::ClearBindings() {
  return StatusFromRc(sqlite3_clear_bindings(stmt_));
}

absl::Status Statement::Bind(int index, int64_t value) {
  return StatusFromRc(sqlite3_bind_int64(stmt_, index, value));
}

absl::Status Statement::Bind(int index, int value) {
  return Bind(index, static_cast<int64_t>(value));
}

absl::Status Statement::Bind(int index, uint64_t value) {
  return Bind(index, std::bit_cast<int64_t>(value));
}

absl::Status Statement::Bind(int index, double value) {
  return StatusFromRc(sqlite3_bind_double(stmt_, index, value));
}

absl::Status Statement::Bind(int index, bool value) {
  return Bind(index, value ? int64_t{1} : int64_t{0});
}

absl::Status Statement::Bind(int index, std::string_view value) {
  // A NULL third argument to sqlite3_bind_text64 binds SQL NULL regardless
  // of length, so route empty (but non-NULL) values through a valid,
  // never-dereferenced (length 0) pointer to bind an empty string instead.
  static constexpr char kEmpty = '\0';
  const char *ptr = value.empty() ? &kEmpty : value.data();
  return StatusFromRc(
      sqlite3_bind_text64(
        stmt_, index, ptr, value.size(), SQLITE_TRANSIENT, SQLITE_UTF8));
}

absl::Status Statement::Bind(int index, std::span<const uint8_t> value) {
  // See Bind(string_view) above -- same NULL-pointer-means-NULL hazard.
  static constexpr uint8_t kEmpty = 0;
  const void *ptr =
      value.empty() ? static_cast<const void *>(&kEmpty) : value.data();
  return StatusFromRc(
      sqlite3_bind_blob64(stmt_, index, ptr, value.size(), SQLITE_TRANSIENT));
}

absl::Status Statement::Bind(int index, std::nullopt_t) { return Null(index); }

absl::Status Statement::Null(int index) {
  return StatusFromRc(sqlite3_bind_null(stmt_, index));
}

bool Statement::ColumnIsNull(int index) {
  return sqlite3_column_type(stmt_, index) == SQLITE_NULL;
}

absl::StatusOr<bool> Statement::Step() {
  VLOG(2) << "sqlite3_step: " << ExpandedSql();
  int rc = sqlite3_step(stmt_);
  if (rc == SQLITE_ROW) return true;
  if (rc == SQLITE_DONE) return false;
  return StatusFromRc(rc);
}

absl::Status Statement::ForEachRow(
    absl::FunctionRef<absl::Status(Statement &)> fn) {
  absl::Cleanup reset_when_done = [this] { Reset().IgnoreError(); };
  while (true) {
    ABSL_ASSIGN_OR_RETURN(bool has_row, Step());
    if (!has_row) return absl::OkStatus();
    ABSL_RETURN_IF_ERROR(fn(*this));
  }
}

absl::Status Statement::ExecuteOnce() {
  absl::Cleanup reset_when_done = [this] { Reset().IgnoreError(); };
  ABSL_ASSIGN_OR_RETURN(bool has_row, Step());
  RET_CHECK(!has_row)
      << "ExecuteOnce: statement produced a row (expected none): " << Sql();
  return absl::OkStatus();
}

std::string_view Statement::Sql() const {
  const char *sql = sqlite3_sql(stmt_);
  return sql != nullptr ? std::string_view(sql) : std::string_view();
}

std::string Statement::ExpandedSql() const {
  char *sql = sqlite3_expanded_sql(stmt_);
  if (sql == nullptr) return std::string();
  absl::Cleanup free_sql([sql] { sqlite3_free(sql); });
  return std::string(sql);
}

sqlite3_stmt *Statement::Get() const { return stmt_; }

// --- Connection ---------------------------------------------------------

Connection::Connection(::sqlite3 &db) : db_(&db) {}

Connection::Connection(Connection &&o) : Connection() { *this = std::move(o); }

Connection &Connection::operator=(Connection &&o) {
  using std::swap;
  swap(db_, o.db_);
  swap(statement_cache_, o.statement_cache_);
  swap(savepoint_depth_, o.savepoint_depth_);
  return *this;
}

Connection::~Connection() { Close().IgnoreError(); }

::sqlite3 *Connection::Get() const { return db_; }

absl::Status Connection::LastErrorStatus() const {
  int code = sqlite3_extended_errcode(db_);
  return MakeSqliteStatus(code, sqlite3_errmsg(db_));
}

absl::Status Connection::Close() {
  if (db_ == nullptr) return absl::OkStatus();
  // Finalize every statement we cached before closing, so sqlite3_close
  // doesn't fail with SQLITE_BUSY because of our own outstanding statements.
  statement_cache_.clear();
  int rc = sqlite3_close(db_);
  if (rc != SQLITE_OK) {
    // The handle is still open and usable when sqlite3_close fails (e.g.
    // something outside our own cache -- a backup object, say -- still
    // references it); leave db_ set so a retried Close() can try again
    // rather than silently leaking it.
    return LastErrorStatus();
  }
  db_ = nullptr;
  return absl::OkStatus();
}

absl::Status Connection::Exec(std::string_view sql) {
  ABSL_ASSIGN_OR_RETURN(Statement * stmt, Prepared(sql));
  return stmt->ForEachRow([](Statement &) { return absl::OkStatus(); });
}

absl::StatusOr<Statement *> Connection::Prepared(std::string_view sql) {
  if (auto it = statement_cache_.find(sql); it != statement_cache_.end()) {
    Statement &stmt = *it->second;
    ABSL_RETURN_IF_ERROR(stmt.Reset());
    ABSL_RETURN_IF_ERROR(stmt.ClearBindings());
    return &stmt;
  }

  ABSL_ASSIGN_OR_RETURN(Statement new_stmt, Statement::Prepare(*this, sql));
  auto owned = std::make_unique<Statement>(std::move(new_stmt));
  Statement *raw = owned.get();
  auto [it, inserted] =
      statement_cache_.emplace(std::string(sql), std::move(owned));
  RET_CHECK(inserted);
  return raw;
}

absl::Status Connection::Transaction(absl::FunctionRef<absl::Status()> body) {
  int depth = savepoint_depth_;
  if (depth == 0) {
    ABSL_RETURN_IF_ERROR(Exec("BEGIN IMMEDIATE"));
  } else {
    ABSL_RETURN_IF_ERROR(Exec(absl::StrCat("SAVEPOINT sp_", depth)));
  }
  ++savepoint_depth_;

  absl::Status body_status = body();

  if (savepoint_depth_ != depth + 1) {
    // Invariant violation: body() should only ever change savepoint_depth_
    // via its own correctly-nested Transaction() calls, which always
    // restore it themselves before returning. Restore our own bookkeeping
    // to a known state and unwind rather than risk leaking an open
    // transaction/savepoint because of it.
    absl::Status invariant_status =
        absl::StatusBuilder(absl::StatusCode::kInternal)
        << "Connection::Transaction: savepoint depth changed unexpectedly "
           "while running body (expected "
        << (depth + 1) << ", got " << savepoint_depth_ << ")";
    savepoint_depth_ = depth;
    return UnwindFailedTransaction(depth, std::move(invariant_status));
  }
  --savepoint_depth_;

  if (body_status.ok()) {
    absl::Status commit_status = Exec(
        depth == 0 ? std::string("COMMIT")
                   : absl::StrCat("RELEASE SAVEPOINT sp_", depth));
    if (commit_status.ok()) return absl::OkStatus();
    // A failing COMMIT/RELEASE (e.g. SQLITE_BUSY racing a reader) leaves the
    // transaction/savepoint open; unwind it so this connection doesn't stay
    // stuck "in a transaction" for every later Transaction() call.
    return UnwindFailedTransaction(depth, std::move(commit_status));
  }

  return UnwindFailedTransaction(depth, std::move(body_status));
}

absl::Status Connection::UnwindFailedTransaction(
    int depth, absl::Status status) {
  absl::Status rollback_status = Exec(
      depth == 0 ? std::string("ROLLBACK")
                 : absl::StrCat("ROLLBACK TO SAVEPOINT sp_", depth));
  if (depth != 0 && rollback_status.ok()) {
    // ROLLBACK TO leaves the savepoint on the stack (so further statements
    // can retry within it); we always treat a failed Transaction() as fully
    // unwound, so also release it.
    rollback_status = Exec(absl::StrCat("RELEASE SAVEPOINT sp_", depth));
  }
  if (!rollback_status.ok()) {
    return absl::StatusBuilder(status)
        << "; additionally, rolling back the transaction failed: "
        << rollback_status;
  }
  return status;
}

int64_t Connection::LastInsertRowId() const {
  return sqlite3_last_insert_rowid(db_);
}

int64_t Connection::Changes() const { return sqlite3_changes64(db_); }

bool Connection::InTransaction() const {
  return sqlite3_get_autocommit(db_) == 0;
}

// --- ConnectionFactory ----------------------------------------------------

namespace {

absl::Status ApplyOpenPragmas(Connection &conn) {
  // journal_mode=WAL doesn't apply to in-memory/temp databases; sqlite just
  // reports back a different mode ("memory") rather than failing, but
  // tolerate an outright failure here too and ignore it either way.
  conn.Exec("PRAGMA journal_mode=WAL").IgnoreError();

  ABSL_RETURN_IF_ERROR(conn.Exec("PRAGMA synchronous=NORMAL"));
  ABSL_RETURN_IF_ERROR(conn.Exec("PRAGMA foreign_keys=ON"));
  ABSL_RETURN_IF_ERROR(conn.Exec("PRAGMA busy_timeout=5000"));
  ABSL_RETURN_IF_ERROR(conn.Exec("PRAGMA temp_store=MEMORY"));
  return absl::OkStatus();
}

}  // namespace

absl::StatusOr<Connection> ConnectionFactory::Open() const {
  ::sqlite3 *db = nullptr;
  int rc = sqlite3_open_v2(
      path.c_str(), &db, flags | SQLITE_OPEN_NOMUTEX | SQLITE_OPEN_EXRESCODE,
      vfs_name.empty() ? nullptr : vfs_name.c_str());
  if (db == nullptr) {
    // sqlite3_open_v2 failed before it could even allocate a handle (e.g.
    // OOM) -- there's no db handle left to pull a detailed errmsg from.
    RET_CHECK_NE(rc, SQLITE_OK);
    return Sqlite3ErrorCodeToStatus(rc);
  }
  // Wrap the handle immediately, even on failure, so ~Connection closes it --
  // sqlite3_open_v2 can allocate a (broken) db object even when it reports
  // an error.
  Connection conn(*db);
  if (rc != SQLITE_OK) return conn.LastErrorStatus();

  ABSL_RETURN_IF_ERROR(ApplyOpenPragmas(conn));
  return conn;
}

}  // namespace sqlite3
}  // namespace dcfs
