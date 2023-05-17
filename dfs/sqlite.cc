#include "dfs/sqlite.h"

#include <utility>

#include "absl/log/log.h"
#include "absl/log/check.h"
#include "absl/strings/str_format.h"
#include "dfs/status.h"

namespace dfs {

absl::StatusCode Sqlite3ErrorCodeToCanonical(int err) {
  switch (err) {
    case SQLITE_OK:
    case SQLITE_ROW:
    case SQLITE_DONE:
      return absl::StatusCode::kOk;
    case SQLITE_ABORT:
    case SQLITE_INTERRUPT:
      return absl::StatusCode::kCancelled;
    case SQLITE_ERROR:
    // TODO remove
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
    C(SQLITE_CANTOPEN);
    C(SQLITE_ERROR);

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

Sqlite3::~Sqlite3() {
  if (db_ == nullptr) return;
  absl::Status st =
    Prepend(Sqlite3ErrorCodeToStatus(sqlite3_close(db_)), "sqlite3_close");
  LOG_IF(WARNING, !st.ok()) << st;
}

absl::StatusOr<Sqlite3> Sqlite3::Open(
    const char *filename, int flags, const char *vfs) {
  sqlite3 *db = nullptr;
  // TODO use SQLITE_OPEN_EXRESCODE
  // TODO use SQLITE_OPEN_NOMUTEX
  absl::Status st =
    Prepend(
        Sqlite3ErrorCodeToStatus(sqlite3_open_v2(filename, &db, flags | SQLITE_OPEN_EXRESCODE, vfs)),
        "sqlite3_open_v2");
  if (db == nullptr) {
    CHECK(!st.ok());
    return st;
  }
  // Failure to open can still allocate an sqlite3 object. So create the
  // Sqlite3 so its destructor will clean things up even if the open failed.
  Sqlite3 sdb(*db);
  if (!st.ok()) return st;
  return sdb;
}

Sqlite3::Sqlite3(sqlite3 &db) : db_(&db) {}

Sqlite3::Sqlite3(Sqlite3 &&o)
    : Sqlite3() {
  *this = std::move(o);
}

Sqlite3 &Sqlite3::operator=(Sqlite3 &&o) {
  using std::swap;
  swap(db_, o.db_);
  return *this;
}

sqlite3 &Sqlite3::operator*() { return *db_; }
sqlite3 *Sqlite3::Get() { return db_; }

absl::Status Sqlite3::LastError() {
  // Db operations can stomp on the error code and message, so grab them before
  // doing anything else.
  int err = sqlite3_extended_errcode(db_);
  std::string errmsg = sqlite3_errmsg(db_);
  if (int offset = sqlite3_error_offset(db_); offset != -1) {
    errmsg = absl::StrCat(errmsg, " (at offset ", offset, ")");
  }
  return Append(Sqlite3ErrorCodeToStatus(err), errmsg, /*joiner=*/": ");
}

absl::Status Sqlite3::Exec(
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

  ASSIGN_OR_RETURN(Sqlite3Stmt stmt, Sqlite3Stmt::Prepare(*this, sql, flags));

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
    ASSIGN_OR_RETURN(Sqlite3Stmt::StepResult res, stmt.Step());
    if (res == Sqlite3Stmt::StepResult::kDone) break;
    CHECK_EQ(res, Sqlite3Stmt::StepResult::kRow);

    std::vector<std::string_view> colvals;
    colvals.reserve(ncols);

    for (int i = 0; i < ncols; i++) {
      const unsigned char *colval = sqlite3_column_text(stmt.Get(), i);
      if (colval == nullptr) {
        // Either NULL in db, or OOM. LastError will tell us.
        RETURN_IF_ERROR(LastError());
      }
      int colval_len = sqlite3_column_bytes(stmt.Get(), i);
      colvals.emplace_back(reinterpret_cast<const char *>(colval), colval_len);
    }

    RETURN_IF_ERROR((*callback)(colnames, std::move(colvals)));
  }

  return absl::OkStatus();
}

Sqlite3Stmt::~Sqlite3Stmt() {
  absl::Status st = std::move(*this).Finalize();
  // TODO adjust other destructors to log at warning
  LOG_IF(WARNING, !st.ok()) << st;
}

absl::Status Sqlite3Stmt::Finalize() && {
  absl::Status st = Sqlite3ErrorCodeToStatus(sqlite3_finalize(stmt_));
  stmt_ = nullptr;
  return st;
}

absl::StatusOr<Sqlite3Stmt> Sqlite3Stmt::Prepare(Sqlite3 &db, std::string_view sql, unsigned int flags) {
  sqlite3_stmt *stmt = nullptr;
  const char *tail = nullptr;
  sqlite3_prepare_v3(db.Get(), sql.data(), sql.size(), flags, &stmt, &tail);
  RETURN_IF_ERROR(Prepend(db.LastError(), "sqlite3_prepare_v3"));
  if (tail != sql.data() + sql.size()) {
    return absl::InvalidArgumentError(absl::StrCat("Extra data at end of sql: ", tail));
  }
  CHECK_NE(stmt, nullptr);
  return Sqlite3Stmt(*stmt);
}

absl::StatusOr<Sqlite3Stmt::StepResult> Sqlite3Stmt::Step() {
  int ret = sqlite3_step(stmt_);
  switch (ret) {
    case SQLITE_OK:
      return StepResult::kOk;
    case SQLITE_DONE:
      return StepResult::kDone;
    case SQLITE_ROW:
      return StepResult::kRow;
    default:
      return Prepend(Sqlite3ErrorCodeToStatus(ret), "sqlite3_step");
  }
}

sqlite3_stmt &Sqlite3Stmt::operator*() { return *stmt_; }
sqlite3_stmt *Sqlite3Stmt::Get() { return stmt_; }

Sqlite3Stmt::Sqlite3Stmt(Sqlite3Stmt &&o) : Sqlite3Stmt() { *this = std::move(o); }
Sqlite3Stmt &Sqlite3Stmt::operator=(Sqlite3Stmt &&o) {
  using std::swap;
  swap(stmt_, o.stmt_);
  return *this;
}

Sqlite3Stmt::Sqlite3Stmt(sqlite3_stmt &stmt) : stmt_(&stmt) {}

std::ostream &operator<<(std::ostream &os, Sqlite3Stmt::StepResult res) {
  using StepResult = Sqlite3Stmt::StepResult;
  switch (res) {
    case StepResult::kOk:
      return os << "StepResult::kOk";
    case StepResult::kDone:
      return os << "StepResult::kDone";
    case StepResult::kRow:
      return os << "StepResult::kRow";
    default:
      return os << absl::StrFormat("StepResult::kUnknown (%d)", static_cast<int>(res));
  }
}

}  // namespace dfs
