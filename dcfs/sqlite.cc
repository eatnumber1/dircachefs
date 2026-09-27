#include "dcfs/sqlite.h"

#include <utility>

#include "absl/cleanup/cleanup.h"
#include "absl/log/check.h"
#include "absl/log/log.h"
#include "absl/status/status_builder.h"
#include "absl/status/status_macros.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/str_format.h"
#include "dcfs/status.h"
#include "dcfs/ret_check.h"

namespace dcfs {
namespace sqlite3 {

absl::StatusCode Sqlite3ErrorCodeToCanonical(int err) {
  switch (err) {
    case SQLITE_OK:
    case SQLITE_ROW:
    case SQLITE_DONE:
      return absl::StatusCode::kOk;
    case SQLITE_INTERNAL:
      return absl::StatusCode::kInternal;
    case SQLITE_AUTH:
    case SQLITE_PERM:
    case SQLITE_READONLY:
      return absl::StatusCode::kPermissionDenied;
    case SQLITE_ABORT:
      return absl::StatusCode::kAborted;
    case SQLITE_BUSY:
    case SQLITE_LOCKED:
    case SQLITE_INTERRUPT:
      return absl::StatusCode::kUnavailable;
    case SQLITE_NOMEM:
    case SQLITE_TOOBIG:
    case SQLITE_FULL:
      return absl::StatusCode::kResourceExhausted;
    case SQLITE_CORRUPT:
      return absl::StatusCode::kDataLoss;
    case SQLITE_NOTFOUND:
      return absl::StatusCode::kNotFound;
    case SQLITE_NOTADB:
    case SQLITE_MISUSE:
      return absl::StatusCode::kInvalidArgument;
    case SQLITE_RANGE:
      return absl::StatusCode::kOutOfRange;
    case SQLITE_CANTOPEN:
    case SQLITE_CONSTRAINT:
    case SQLITE_EMPTY:
    case SQLITE_ERROR:
    case SQLITE_FORMAT:
    case SQLITE_IOERR:
    case SQLITE_MISMATCH:
    case SQLITE_NOLFS:
    case SQLITE_NOTICE:
    case SQLITE_PROTOCOL:
    case SQLITE_SCHEMA:
    case SQLITE_WARNING:
    default:
      return absl::StatusCode::kUnknown;
  }
}

std::string Sqlite3ErrorCodeToString(int err) {
  switch (err) {
#define C(ec) \
    case ec: return absl::StrFormat("%s (%d)", #ec, ec)

    C(SQLITE_OK);
    C(SQLITE_ROW);
    C(SQLITE_DONE);
    C(SQLITE_ERROR);
    C(SQLITE_INTERNAL);
    C(SQLITE_PERM);
    C(SQLITE_ABORT);
    C(SQLITE_BUSY);
    C(SQLITE_LOCKED);
    C(SQLITE_NOMEM);
    C(SQLITE_READONLY);
    C(SQLITE_INTERRUPT);
    C(SQLITE_IOERR);
    C(SQLITE_CORRUPT);
    C(SQLITE_NOTFOUND);
    C(SQLITE_FULL);
    C(SQLITE_CANTOPEN);
    C(SQLITE_PROTOCOL);
    C(SQLITE_EMPTY);
    C(SQLITE_SCHEMA);
    C(SQLITE_TOOBIG);
    C(SQLITE_CONSTRAINT);
    C(SQLITE_MISMATCH);
    C(SQLITE_MISUSE);
    C(SQLITE_NOLFS);
    C(SQLITE_AUTH);
    C(SQLITE_FORMAT);
    C(SQLITE_RANGE);
    C(SQLITE_NOTADB);
    C(SQLITE_NOTICE);
    C(SQLITE_WARNING);

#undef C

    default:
      return absl::StrFormat("UNKNOWN (%d)", err);
  }
}

absl::Status Sqlite3ErrorCodeToStatus(int err) {
  absl::StatusCode code = Sqlite3ErrorCodeToCanonical(err);
  std::string errstr = absl::StrCat(Sqlite3ErrorCodeToString(err), ": ", sqlite3_errstr(err));
  return absl::Status(code, errstr);
}

Connection::~Connection() {
  if (db_ == nullptr) return;
  absl::Status st =
    absl::StatusBuilder(Sqlite3ErrorCodeToStatus(sqlite3_close(db_)))
        .SetPrepend()
      << "sqlite3_close: ";
  LOG_IF(WARNING, !st.ok()) << st;
}

absl::StatusOr<Connection> Connection::Open(
    std::string_view filename, int flags, std::optional<std::string_view> vfs) {
  ::sqlite3 *db = nullptr;
  // TODO use SQLITE_OPEN_EXRESCODE
  // TODO use SQLITE_OPEN_NOMUTEX
  absl::Status st =
    absl::StatusBuilder(
        Sqlite3ErrorCodeToStatus(
          sqlite3_open_v2(
            std::string(filename).c_str(), &db,
            flags | SQLITE_OPEN_EXRESCODE | SQLITE_OPEN_NOMUTEX,
            vfs ? std::string(*vfs).c_str() : nullptr)))
        .SetPrepend()
      << "sqlite3_open_v2: ";
  if (db == nullptr) {
    RET_CHECK(!st.ok());
    return st;
  }
  // Failure to open can still allocate an sqlite3 object. So create the
  // Connection so its destructor will clean things up even if the open failed.
  Connection sdb(*db);
  if (!st.ok()) return st;

  // TODO move to separate file and syntax check+lint?
  ABSL_RETURN_IF_ERROR(sdb.Exec("PRAGMA foreign_keys = ON"));

  return sdb;
}

Connection::Connection(::sqlite3 &db)
  : db_(&db) {}

Connection::Connection(Connection &&o)
    : Connection() {
  *this = std::move(o);
}

Connection &Connection::operator=(Connection &&o) {
  using std::swap;
  swap(db_, o.db_);
  return *this;
}

::sqlite3 &Connection::operator*() { return *db_; }
::sqlite3 *Connection::Get() { return db_; }

absl::Status Connection::LastError() {
  // Db operations can stomp on the error code and message, so grab them before
  // doing anything else.
  int err = sqlite3_extended_errcode(db_);
  std::string errmsg = sqlite3_errmsg(db_);
  if (int offset = sqlite3_error_offset(db_); offset != -1) {
    errmsg = absl::StrCat(errmsg, " (at offset ", offset, ")");
  }
  return absl::StatusBuilder(Sqlite3ErrorCodeToStatus(err)).SetAppend()
      << ": " << errmsg;
}

absl::Status Connection::Exec(
    std::string_view sql,
    std::optional<
      absl::FunctionRef<
        absl::Status(
          const std::vector<std::string_view> &colnames,
          std::vector<std::string_view> colvals)>>
      callback,
    unsigned int flags) {
  if (!callback) {
    callback.emplace(
        [](const std::vector<std::string_view> &,
           std::vector<std::string_view>) -> absl::Status {
          return absl::OkStatus();
        });
  }

  ABSL_ASSIGN_OR_RETURN(Statement stmt, Statement::Prepare(*this, sql, flags));

  int ncols = sqlite3_column_count(stmt.Get());

  std::vector<std::string_view> colnames;
  colnames.reserve(ncols);
  for (int i = 0; i < ncols; i++) {
    const char *name = sqlite3_column_name(stmt.Get(), i);
    if (name == nullptr) {
      return absl::InternalError("sqlite3_malloc failed");
    }
    colnames.push_back(name);
  }

  while (true) {
    ABSL_ASSIGN_OR_RETURN(Statement::StepResult res, stmt.Step());
    if (res == Statement::StepResult::kDone) break;
    RET_CHECK_EQ(res, Statement::StepResult::kRow);

    std::vector<std::string_view> colvals;
    colvals.reserve(ncols);

    for (int i = 0; i < ncols; i++) {
      const unsigned char *colval = sqlite3_column_text(stmt.Get(), i);
      if (colval == nullptr) {
        // Either NULL in db, or OOM. LastError will tell us.
        ABSL_RETURN_IF_ERROR(LastError());
      }
      int colval_len = sqlite3_column_bytes(stmt.Get(), i);
      colvals.emplace_back(reinterpret_cast<const char *>(colval), colval_len);
    }

    ABSL_RETURN_IF_ERROR((*callback)(colnames, std::move(colvals)));
  }

  return absl::OkStatus();
}

int64_t Connection::LastInsertRowID() {
  return sqlite3_last_insert_rowid(db_);
}

absl::StatusOr<WithSavepoint> WithSavepoint::Create(
    Connection *absl_nonnull db, std::string name) {
  WithSavepoint ws(std::move(name));
  auto &s = ws.statements_;

  ABSL_ASSIGN_OR_RETURN(
      s.savepoint, Statement::Prepare(*db, "SAVEPOINT @name"));
  ABSL_RETURN_IF_ERROR(s.savepoint.BindBlobUnowned("@name", ws.name_));

  ABSL_ASSIGN_OR_RETURN(
      s.release, Statement::Prepare(*db, "RELEASE SAVEPOINT @name"));
  ABSL_RETURN_IF_ERROR(s.release.BindBlobUnowned("@name", ws.name_));

  ABSL_ASSIGN_OR_RETURN(
      s.rollback, Statement::Prepare(*db, "ROLLBACK TO SAVEPOINT @name"));
  ABSL_RETURN_IF_ERROR(s.rollback.BindBlobUnowned("@name", ws.name_));

  ABSL_RETURN_IF_ERROR(ws.Savepoint());

  return ws;
}

absl::StatusOr<WithSavepoint> WithSavepoint::Create(
    Connection *absl_nonnull db,
    std::source_location loc) {
  // Used to generate unique ids for WithSavepoint so that otherwise identically
  // named instances don't get mixed up ordering when running RELEASE.
  static std::atomic_int unique_id = 0;

  return Create(
      db,
      absl::StrFormat(
        "%s:%d: `%s` #%d",
        loc.file_name(), loc.line(), loc.function_name(), unique_id++));
}

WithSavepoint::WithSavepoint(std::string name) : name_(std::move(name)) {}

WithSavepoint::~WithSavepoint() {
  if (!active_) return;
  LOG(WARNING)
    << "WithSavepoint destructor called on savepoint named " << name_
    << " with the savepoint still active. Reverting...";
  absl::Status revert_status = std::move(*this).Revert();
  LOG_IF(ERROR, !revert_status.ok()) << revert_status;
}

WithSavepoint::WithSavepoint(WithSavepoint &&o)
  : WithSavepoint() {
  *this = std::move(o);
}
WithSavepoint &WithSavepoint::operator=(WithSavepoint &&o) {
  using std::swap;
  swap(active_, o.active_);
  swap(statements_.savepoint, o.statements_.savepoint);
  swap(statements_.release, o.statements_.release);
  swap(statements_.rollback, o.statements_.rollback);
  return *this;
}

absl::Status WithSavepoint::Commit() && {
  return std::move(*this).Release();
}

absl::Status WithSavepoint::Rollback() {
  return statements_.rollback.StepThenDone();
}

absl::Status WithSavepoint::Revert() && {
  ABSL_RETURN_IF_ERROR(Rollback());
  return std::move(*this).Release();
}

absl::Status WithSavepoint::Release() && {
  absl::Status st = statements_.release.StepThenDone();
  active_ = false;
  return st;
}

absl::Status WithSavepoint::Savepoint() {
  ABSL_RETURN_IF_ERROR(statements_.savepoint.StepThenDone());
  active_ = true;
  return absl::OkStatus();
}

Statement::~Statement() {
  if (stmt_ == nullptr) return;
  absl::Status st = std::move(*this).Finalize();
  LOG_IF(WARNING, !st.ok()) << st;
}

std::string_view Statement::GetSql() const {
  if (stmt_ == nullptr) return "null statement";
  const char *sql = sqlite3_sql(stmt_);
  if (sql == nullptr) return "sqlite3_sql returned null";
  return sql;
}

std::string Statement::GetExpandedSql() const {
  if (stmt_ == nullptr) return "null statement";
  char *sql = sqlite3_expanded_sql(stmt_);
  if (sql == nullptr) return "sqlite3_expanded_sql returned null";
  absl::Cleanup free_sql([sql]() { sqlite3_free(sql); });
  return std::string(sql);
}

absl::Status Statement::Finalize() && {
  absl::Status st = Sqlite3ErrorCodeToStatus(sqlite3_finalize(stmt_));
  stmt_ = nullptr;
  return st;
}

absl::StatusOr<Statement> Statement::Prepare(
    Connection &db, std::string_view sql, unsigned int flags) {
  sqlite3_stmt *stmt = nullptr;
  const char *tail = nullptr;
  sqlite3_prepare_v3(db.Get(), sql.data(), sql.size(), flags, &stmt, &tail);
  absl::Status prepare_status =
      absl::StatusBuilder(db.LastError()).SetPrepend() << "sqlite3_prepare_v3; ";
  if (!prepare_status.ok()) {
    absl::Status full_status =
        absl::StatusBuilder(std::move(prepare_status)) << "with SQL " << sql;
    return full_status;
  }
  if (tail != sql.data() + sql.size()) {
    return absl::InvalidArgumentError(
        absl::StrCat("Extra data at end of sql: ", tail));
  }
  RET_CHECK_NE(stmt, nullptr);
  return Statement(*stmt);
}

absl::StatusOr<Statement::StepResult> Statement::Step() {
  LOG(INFO) << "Statement::Step() count=" << step_count_++ << " " << *this;
  int ret = sqlite3_step(stmt_);
  switch (ret) {
    case SQLITE_DONE:
      return StepResult::kDone;
    case SQLITE_ROW:
      return StepResult::kRow;
    case SQLITE_OK:
      // Docs seem to imply this can never happen.
      [[fallthrough]];
    default: {
      absl::Status step_status =
          absl::StatusBuilder(Sqlite3ErrorCodeToStatus(ret)).SetPrepend()
              << "sqlite3_step; ";
      absl::Status full_status =
          absl::StatusBuilder(std::move(step_status)) << "for SQL: " << *this;
      return full_status;
    }
  }
}

absl::Status Statement::StepThenDone() {
  ABSL_ASSIGN_OR_RETURN(StepResult res, Step());
  if (res != StepResult::kDone) {
    absl::Status reset_status = Reset();
    LOG_IF(WARNING, !reset_status.ok()) << reset_status;
    return absl::InternalError(
        absl::StrCat(
          "StepThenDone called, but Step did not return kDone. Instead, "
          "it returned ", res));
  }
  return absl::OkStatus();
}

absl::Status Statement::Reset() {
  step_count_ = 0;
  return Sqlite3ErrorCodeToStatus(sqlite3_reset(stmt_));
}

absl::Status Statement::ClearBindings() {
  return Sqlite3ErrorCodeToStatus(sqlite3_clear_bindings(stmt_));
}

absl::StatusOr<std::string_view> Statement::GetParameterName(
    int index) const {
  const char *n = sqlite3_bind_parameter_name(stmt_, index);
  if (n == nullptr) {
    return absl::NotFoundError(
        absl::StrFormat(
          "Parameter at index %d not found or is unnamed", index));
  }
  return n;
}

absl::StatusOr<int> Statement::GetParameterIndex(
    std::string_view name) const {
  int idx = sqlite3_bind_parameter_index(stmt_, std::string(name).c_str());
  if (idx == 0) {
    return absl::NotFoundError(
        absl::StrFormat("Parameter with name %s not found", name));
  }
  return idx;
}

int Statement::GetParameterCount() const {
  return sqlite3_bind_parameter_count(stmt_);
}

absl::Status Statement::Bind(int index, int64_t value) {
  return Sqlite3ErrorCodeToStatus(sqlite3_bind_int64(stmt_, index, value));
}

absl::Status Statement::Bind(int index, nullptr_t) {
  return Sqlite3ErrorCodeToStatus(sqlite3_bind_null(stmt_, index));
}

absl::Status Statement::BindDouble(int index, double value) {
  return Sqlite3ErrorCodeToStatus(sqlite3_bind_double(stmt_, index, value));
}

absl::Status Statement::BindTextUnowned(int index, std::string_view data) {
  return Sqlite3ErrorCodeToStatus(
      sqlite3_bind_text64(
        stmt_, index, data.data(), data.size(), SQLITE_STATIC, SQLITE_UTF8));
}

absl::Status Statement::BindBlobUnowned(int index, std::string_view data) {
  return Sqlite3ErrorCodeToStatus(
      sqlite3_bind_blob64(
        stmt_, index, data.data(), data.size(), SQLITE_STATIC));
}

absl::Status Statement::BindText(int index, std::string_view data) {
  return Sqlite3ErrorCodeToStatus(
      sqlite3_bind_text64(
        stmt_, index, data.data(), data.size(), SQLITE_TRANSIENT, SQLITE_UTF8));
}

absl::Status Statement::BindBlob(int index, std::string_view data) {
  return Sqlite3ErrorCodeToStatus(
      sqlite3_bind_blob64(
        stmt_, index, data.data(), data.size(), SQLITE_TRANSIENT));
}

absl::Status Statement::Bind(std::string_view name, int64_t value) {
  ABSL_ASSIGN_OR_RETURN(int idx, GetParameterIndex(name));
  return Bind(idx, value);
}

absl::Status Statement::BindDouble(std::string_view name, double value) {
  ABSL_ASSIGN_OR_RETURN(int idx, GetParameterIndex(name));
  return Bind(idx, value);
}

absl::Status Statement::Bind(std::string_view name, nullptr_t) {
  ABSL_ASSIGN_OR_RETURN(int idx, GetParameterIndex(name));
  return Bind(idx, nullptr);
}

absl::Status Statement::BindTextUnowned(std::string_view name, std::string_view data) {
  ABSL_ASSIGN_OR_RETURN(int idx, GetParameterIndex(name));
  return BindTextUnowned(idx, data);
}

absl::Status Statement::BindBlobUnowned(std::string_view name, std::string_view data) {
  ABSL_ASSIGN_OR_RETURN(int idx, GetParameterIndex(name));
  return BindBlobUnowned(idx, data);
}

absl::Status Statement::BindText(std::string_view name, std::string_view data) {
  ABSL_ASSIGN_OR_RETURN(int idx, GetParameterIndex(name));
  return BindText(idx, data);
}

absl::Status Statement::BindBlob(std::string_view name, std::string_view data) {
  ABSL_ASSIGN_OR_RETURN(int idx, GetParameterIndex(name));
  return BindBlob(idx, data);
}

int Statement::GetColumnCount() const {
  return sqlite3_column_count(stmt_);
}

int Statement::GetDataCount() const {
  return sqlite3_data_count(stmt_);
}

absl::StatusOr<std::string_view> Statement::GetColumnName(int index) const {
  const char *cname = sqlite3_column_name(stmt_, index);
  if (cname == nullptr) return absl::InternalError("sqlite3_column_name");
  return cname;
}

absl::StatusOr<
    std::reference_wrapper<const absl::flat_hash_map<std::string, int>>>
    Statement::GetColumnIndices() const {
  auto &mthis = *const_cast<Statement *>(this);
  return mthis.GetColumnIndices();
}

absl::StatusOr<std::reference_wrapper<absl::flat_hash_map<std::string, int>>>
Statement::GetColumnIndices() {
  if (column_indices_ != std::nullopt) return *column_indices_;

  absl::flat_hash_map<std::string, int> column_indices;
  for (int i = 0; i < GetColumnCount(); i++) {
    ABSL_ASSIGN_OR_RETURN(std::string_view column_name, GetColumnName(i));
    column_indices.emplace(std::string(column_name), i);
  }

  column_indices_ = std::move(column_indices);
  return *column_indices_;
}

absl::StatusOr<int> Statement::GetColumnIndex(std::string_view name) const {
  ABSL_ASSIGN_OR_RETURN(const auto &ices, GetColumnIndices());
  const absl::flat_hash_map<std::string, int> &indices = ices;
  auto it = indices.find(name);
  if (it == indices.end()) {
    return absl::NotFoundError(
        absl::StrCat("Could not find column with name ", name));
  }
  return it->second;
}

absl::StatusOr<std::string_view> Statement::ColumnBlob(std::string_view name) {
  ABSL_ASSIGN_OR_RETURN(int index, GetColumnIndex(name));
  return ColumnBlob(index);
}

absl::StatusOr<std::string_view> Statement::ColumnBlob(int index) {
  const void *blob = sqlite3_column_blob(stmt_, index);
  if (blob == nullptr) {
    return absl::NotFoundError(
        absl::StrFormat("Column with at index %d not found", index));
  }
  return std::string_view(
      static_cast<const char *>(blob),
      sqlite3_column_bytes(stmt_, index));
}

absl::StatusOr<double> Statement::ColumnDouble(std::string_view name) {
  ABSL_ASSIGN_OR_RETURN(int index, GetColumnIndex(name));
  return ColumnDouble(index);
}

double Statement::ColumnDouble(int index) {
  return sqlite3_column_double(stmt_, index);
}

absl::StatusOr<int64_t> Statement::ColumnInt64(std::string_view name) {
  ABSL_ASSIGN_OR_RETURN(int index, GetColumnIndex(name));
  return ColumnInt64(index);
}

int64_t Statement::ColumnInt64(int index) {
  return sqlite3_column_int64(stmt_, index);
}

absl::StatusOr<int> Statement::ColumnInt(std::string_view name) {
  ABSL_ASSIGN_OR_RETURN(int index, GetColumnIndex(name));
  return ColumnInt(index);
}

int Statement::ColumnInt(int index) {
  return sqlite3_column_int(stmt_, index);
}

sqlite3_stmt &Statement::operator*() { return *stmt_; }
sqlite3_stmt *Statement::Get() { return stmt_; }

Statement::Statement(Statement &&o) : Statement() {
  *this = std::move(o);
}

Statement &Statement::operator=(Statement &&o) {
  using std::swap;
  swap(stmt_, o.stmt_);
  swap(column_indices_, o.column_indices_);
  return *this;
}

Statement::Statement(sqlite3_stmt &stmt) : stmt_(&stmt) {}

std::ostream &operator<<(std::ostream &os, Statement::StepResult res) {
  return os << absl::StrCat(res);
}

std::ostream &operator<<(std::ostream &os, Statement stmt) {
  return os << absl::StrCat(stmt);
}

WithStatementReset::WithStatementReset(Statement *stmt)
  : stmt_(stmt) {}

WithStatementReset::~WithStatementReset() {
  if (stmt_ == nullptr) return;
  absl::Status reset_status = std::move(*this).Reset();
  LOG_IF(ERROR, !reset_status.ok()) << reset_status;
}

absl::Status WithStatementReset::Reset() && {
  Statement &stmt = *stmt_;
  return stmt.Reset();
}

}  // namespace sqlite3
}  // namespace dcfs
