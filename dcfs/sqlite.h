#ifndef DCFS_SQLITE_H_
#define DCFS_SQLITE_H_

#include <functional>
#include <string>
#include <concepts>
#include <string_view>
#include <optional>
#include <ostream>
#include <source_location>

#include "absl/container/flat_hash_map.h"
#include "absl/functional/function_ref.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/str_format.h"
#include "sqlite3.h"
#include "dcfs/attributes.h"
#include "dcfs/status.h"

// The C sqlite3 API declares a type named `sqlite3` (see <sqlite3.h>) at
// global scope, so this namespace cannot also be named `sqlite3` at global
// scope. Nest it under `dcfs` instead.
namespace dcfs {
namespace sqlite3 {

class Connection;

// See https://www.sqlite.org/c3ref/stmt.html
class Statement {
 public:
  Statement() = default;
  ~Statement();

  static absl::StatusOr<Statement> Prepare(
      Connection &db, std::string_view sql, unsigned int flags = 0);

  // The return status from Reset indicates whether or not the previous
  // evaluation of this prepared statement completed successfully.
  //
  // Any SQL statement variables that had values bound to them retain their
  // values.
  absl::Status Reset();

  // Use this routine to reset all host parameters to NULL.
  absl::Status ClearBindings();

  // Name of a host parameter
  //
  // https://www.sqlite.org/c3ref/bind_parameter_name.html
  //
  // The first host parameter has an index of 1, not 0.
  // Returns nullopt of no parameter with the given index is found, or is
  // nameless.
  absl::StatusOr<std::string_view> GetParameterName(int index) const;

  // Index of a parameter with a given name
  //
  // https://www.sqlite.org/c3ref/bind_parameter_index.html
  //
  // The first host parameter has an index of 1, not 0.
  // Returns nullopt if no parameter with the given name is found.
  absl::StatusOr<int> GetParameterIndex(std::string_view name) const;

  // Number of SQL parameters
  //
  // https://www.sqlite.org/c3ref/bind_parameter_count.html
  //
  // This routine actually returns the index of the largest (rightmost)
  // parameter. For all forms except ?NNN, this will correspond to the number of
  // unique parameters. If parameters of the ?NNN form are used, there may be
  // gaps in the list.
  int GetParameterCount() const;

  absl::Status Bind(int index, int64_t value);
  absl::Status Bind(int index, std::floating_point auto value);
  absl::Status Bind(int index, nullptr_t);

  // Text must be a UTF-8 encoded string.
  //
  // The data argument must remain valid until either the prepared statement is
  // finalized or the same SQL parameter is bound to something else, whichever
  // occurs sooner
  absl::Status BindTextUnowned(int index, std::string_view data);

  // The data argument must remain valid until either the prepared statement is
  // finalized or the same SQL parameter is bound to something else, whichever
  // occurs sooner
  absl::Status BindBlobUnowned(int index, std::string_view data);

  // Text must be a UTF-8 encoded string.
  absl::Status BindText(int index, std::string_view data);

  absl::Status BindBlob(int index, std::string_view data);

  // Same as the index-based Bind calls above, but they take the name of a
  // parameter instead of its index.
  absl::Status Bind(std::string_view name, int64_t value);
  absl::Status Bind(std::string_view name, std::floating_point auto value);
  absl::Status Bind(std::string_view name, nullptr_t);
  absl::Status BindTextUnowned(std::string_view name, std::string_view data);
  absl::Status BindBlobUnowned(std::string_view name, std::string_view data);
  absl::Status BindText(std::string_view name, std::string_view data);
  absl::Status BindBlob(std::string_view name, std::string_view data);

  // Number of columns in a result set
  //
  // https://www.sqlite.org/c3ref/column_count.html
  //
  // A SELECT statement will always have a positive column count but depending
  // on the WHERE clause constraints and the table content, it might return no
  // rows.
  int GetColumnCount() const;

  // Number of columns in a result set
  //
  // Returns the number of columns in the current row of the result set.
  int GetDataCount() const;

  // Name of the column in the result of a SELECT statement at a given index.
  //
  // https://sqlite.org/c3ref/column_name.html
  //
  // The returned string_view is valid until either the prepared statement is
  // destroyed by Finalize or until the statement is automatically re-prepared
  // by the first call to Step for a particular run or until the next call to
  // GetColumnName on the same column.
  absl::StatusOr<std::string_view> GetColumnName(int index) const;

  // Index of the column in the result of a SELECT statement with a given name.
  absl::StatusOr<int> GetColumnIndex(std::string_view name) const;

  // If the SQL statement does not currently point to a valid row, or if the
  // column index is out of range, the result is undefined.
  //
  // The leftmost column of the result set has the index 0.
  template <std::same_as<double> T>
  constexpr inline T Column(int index);
  template <std::same_as<int> T>
  constexpr inline T Column(int index);
  template <std::same_as<int64_t> T>
  constexpr inline T Column(int index);
  // No support for text, only blob right now.
  //
  // The returned string_view is valid until a type conversion, or until Step,
  // Reset, or Finalize is called.
  template <std::same_as<std::string_view> T>
  constexpr inline absl::StatusOr<T> Column(int index);

  // The leftmost column of the result set has the index 0.
  template <std::same_as<double> T>
  constexpr inline absl::StatusOr<T> Column(std::string_view name);
  template <std::same_as<int> T>
  constexpr inline absl::StatusOr<T> Column(std::string_view name);
  template <std::same_as<int64_t> T>
  constexpr inline absl::StatusOr<T> Column(std::string_view name);
  // No support for text, only blob right now.
  //
  // The returned string_view is valid until a type conversion, or unti Step,
  // Reset, or Finalize is called.
  template <std::same_as<std::string_view> T>
  constexpr inline absl::StatusOr<T> Column(std::string_view name);

  absl::Status Finalize() &&;

  enum class StepResult {
    kDone, // SQLITE_DONE
    kRow,  // SQLITE_ROW
  };
  absl::StatusOr<StepResult> Step();
  absl::Status StepThenDone();

  template <typename T>
  absl::StatusOr<T> StepOneCellThenDone(std::string_view column_name);
  template <typename T>
  absl::StatusOr<T> StepOneCellThenDone(int column_index);

  std::string GetExpandedSql() const;
  std::string_view GetSql() const;

  template <typename Sink>
  friend void AbslStringify(Sink &sink, const Statement &stmt);

  sqlite3_stmt &operator*();
  sqlite3_stmt *Get();

  Statement(Statement &&);
  Statement(const Statement &) = delete;
  Statement &operator=(Statement &&);
  Statement &operator=(const Statement &) = delete;

 private:
  Statement(sqlite3_stmt &stmt);

  absl::Status BindDouble(int index, double value);
  absl::Status BindDouble(std::string_view name, double value);

  double ColumnDouble(int index);
  absl::StatusOr<double> ColumnDouble(std::string_view name);
  int64_t ColumnInt64(int index);
  absl::StatusOr<int64_t> ColumnInt64(std::string_view name);
  int ColumnInt(int index);
  absl::StatusOr<int> ColumnInt(std::string_view name);
  absl::StatusOr<std::string_view> ColumnBlob(int index);
  absl::StatusOr<std::string_view> ColumnBlob(std::string_view name);

  absl::StatusOr<std::reference_wrapper<absl::flat_hash_map<std::string, int>>>
    GetColumnIndices();
  absl::StatusOr<
    std::reference_wrapper<const absl::flat_hash_map<std::string, int>>>
    GetColumnIndices() const;

  int step_count_ = 0;
  sqlite3_stmt *stmt_ = nullptr;
  mutable std::optional<absl::flat_hash_map<std::string, int>> column_indices_;
};

class WithSavepoint {
 public:
  static absl::StatusOr<WithSavepoint> Create(
      Connection *absl_nonnull db,
      std::source_location loc = std::source_location::current());

  static absl::StatusOr<WithSavepoint> Create(
      Connection *absl_nonnull db, std::string name);

  WithSavepoint() = default;

  // Runs RELEASE on the savepoint, saving the results to the database (unless
  // this is a nested savepoint, then it's deferred until the final RELEASE is
  // run).
  absl::Status Commit() &&;

  // Runs ROLLBACK TO SAVEPOINT @name on the savepoint, bringing the transaction
  // back to the state when the savepoint was created.
  absl::Status Rollback();

  // Runs ROLLBACK TO SAVEPOINT @name on the savepoint, followed by RELEASE,
  // effectively cancelling the savepoint.
  absl::Status Revert() &&;

  // If the Commit method has not been called, the savepoint will be rolled
  // back.
  ~WithSavepoint();

  WithSavepoint(WithSavepoint &&);
  WithSavepoint(const WithSavepoint &) = delete;
  WithSavepoint &operator=(WithSavepoint &&);
  WithSavepoint &operator=(const WithSavepoint &) = delete;

 private:
  explicit WithSavepoint(std::string name);

  absl::Status Savepoint();
  absl::Status Release() &&;

  struct {
    Statement savepoint;
    Statement release;
    Statement rollback;
  } statements_;

  // Active means that SAVEPOINT has been run without a corresponding RELEASE.
  bool active_ = false;
  std::string name_;
};

class Connection {
 public:
  Connection() = default;
  ~Connection();

  static absl::StatusOr<Connection> Open(
      std::string_view filename, int flags = 0,
      std::optional<std::string_view> vfs = std::nullopt);

  ::sqlite3 &operator*();
  ::sqlite3 *Get();

  absl::Status LastError();

  absl::Status Exec(
      std::string_view sql,
      std::optional<
        absl::FunctionRef<
          absl::Status(
            const std::vector<std::string_view> &colnames,
            std::vector<std::string_view> colvals)>>
        callback = std::nullopt,
      unsigned int flags = 0);

  int64_t LastInsertRowID();

  Connection(Connection &&);
  Connection(const Connection &) = delete;
  Connection &operator=(Connection &&);
  Connection &operator=(const Connection &) = delete;

 private:
  Connection(::sqlite3 &db);

  ::sqlite3 *db_ = nullptr;
};

absl::Status Sqlite3ErrorCodeToStatus(int err);
absl::StatusCode Sqlite3ErrorCodeToCanonical(int err);
std::string Sqlite3ErrorCodeToString(int err);

std::ostream &operator<<(std::ostream &os, Statement::StepResult res);
std::ostream &operator<<(std::ostream &os, Statement stmt);

class WithStatementReset {
 public:
  WithStatementReset() = default;
  WithStatementReset(Statement *absl_nullable stmt);

  ~WithStatementReset();

  absl::Status Reset() &&;

 private:
  Statement *stmt_ = nullptr;
};

// Implementation details below

absl::Status Statement::Bind(int index, std::floating_point auto value) {
  return BindDouble(index, value);
}

absl::Status Statement::Bind(
    std::string_view name, std::floating_point auto value) {
  return BindDouble(name, value);
}

template <std::same_as<std::string_view> T>
constexpr inline absl::StatusOr<T> Statement::Column(int index) {
  return ColumnBlob(index);
}

template <std::same_as<std::string_view> T>
constexpr inline absl::StatusOr<T> Statement::Column(std::string_view name) {
  return ColumnBlob(name);
}

template <std::same_as<double> T>
constexpr inline T Statement::Column(int index) {
  return ColumnDouble(index);
}

template <std::same_as<double> T>
constexpr inline absl::StatusOr<T> Statement::Column(std::string_view name) {
  return ColumnDouble(name);
}

template <std::same_as<int> T>
constexpr inline T Statement::Column(int index) {
  return ColumnInt(index);
}

template <std::same_as<int> T>
constexpr inline absl::StatusOr<T> Statement::Column(std::string_view name) {
  return ColumnInt(name);
}

template <std::same_as<int64_t> T>
constexpr inline T Statement::Column(int index) {
  return ColumnInt64(index);
}

template <std::same_as<int64_t> T>
constexpr inline absl::StatusOr<T> Statement::Column(std::string_view name) {
  return ColumnInt64(name);
}

template <typename T>
absl::StatusOr<T> Statement::StepOneCellThenDone(int column_index) {
  ASSIGN_OR_RETURN(StepResult res, Step());
  WithStatementReset reset(this);

  if (res != StepResult::kRow) {
    return absl::InternalError(
        absl::StrCat("Expected StepResult::kRow, got ", res));
  }

  auto ret = Column<T>(column_index);

  RETURN_IF_ERROR(StepThenDone());
  return ret;
}

template <typename T>
absl::StatusOr<T> Statement::StepOneCellThenDone(std::string_view column_name) {
  ASSIGN_OR_RETURN(int index, GetColumnIndex(column_name));
  return StepOneCellThenDone<T>(index);
}

template <typename Sink>
void AbslStringify(Sink &sink, Statement::StepResult res) {
  using StepResult = Statement::StepResult;
  switch (res) {
    case StepResult::kDone:
      absl::Format(&sink, "StepResult::kDone");
      break;
    case StepResult::kRow:
      absl::Format(&sink, "StepResult::kRow");
      break;
    default:
      absl::Format(&sink, "StepResult::kUnknown (%d)", static_cast<int>(res));
  }
}

template <typename Sink>
void AbslStringify(Sink &sink, const Statement &stmt) {
  absl::Format(&sink, "%s", stmt.GetExpandedSql());
}

}  // namespace sqlite3
}  // namespace dcfs

#endif  // DCFS_SQLITE_H_
