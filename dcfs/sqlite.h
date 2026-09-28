#ifndef DCFS_SQLITE_H_
#define DCFS_SQLITE_H_

#include <bit>
#include <concepts>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include "absl/container/flat_hash_map.h"
#include "absl/functional/function_ref.h"
#include "absl/status/status.h"
#include "absl/status/status_macros.h"
#include "absl/status/statusor.h"
#include "sqlite3.h"

// The C sqlite3 API declares a type named `sqlite3` (see <sqlite3.h>) at
// global scope, so this namespace cannot also be named `sqlite3` at global
// scope. Nest it under `dcfs` instead.
namespace dcfs {
namespace sqlite3 {

class Connection;
class Statement;
struct ConnectionFactory;

// How durable a Connection::Transaction()'s COMMIT must be.
enum class Durability {
  // The connection's default, synchronous=NORMAL (see ConnectionFactory):
  // in WAL mode a commit reaches the WAL with write(2) but is not fsynced,
  // so a power loss (not a process crash) can roll back a suffix of
  // recently committed transactions. Every surviving state is still a
  // consistent prefix of the committed ones (the WAL's chained frame
  // checksums guarantee that).
  kNormal,
  // synchronous=FULL for this one transaction: in WAL mode the WAL is
  // fsynced as part of the COMMIT (sqlite3.c sqlite3WalFrames: `isCommit &&
  // WAL_SYNC_FLAGS(sync_flags)` -> sqlite3OsSync, with the commit sync flags
  // set only when the pager's fullSync is, i.e. synchronous >= FULL), so
  // once Transaction() returns OK the transaction -- and, WAL frames being
  // appended in order, every transaction committed before it -- survives a
  // power loss. Costs one fdatasync of the WAL (plus a device cache flush).
  kSync,
};

namespace internal {

// Trait used to give Bind()/Column() a single overload that handles
// std::optional<T> generically (bind NULL / report nullopt for an empty
// optional, delegate to the T overload otherwise), instead of writing out
// every T x optional<T> combination by hand.
template <typename T>
struct IsOptional : std::false_type {};
template <typename T>
struct IsOptional<std::optional<T>> : std::true_type {
  using value_type = T;
};

}  // namespace internal

// A single prepared SQL statement. Owns a `sqlite3_stmt*`.
//
// Statements are move-only and are meant to live exactly one place: inside
// the StatementCache owned by a Connection (see Connection::Prepared()). No
// other class should hold a Statement member -- ask the owning Connection
// for one instead.
//
// See https://www.sqlite.org/c3ref/stmt.html
class Statement {
 public:
  Statement() = default;
  ~Statement();

  // Prepares `sql` against `db`. Only a single SQL statement may appear in
  // `sql`; trailing text after it is an error. Most callers should go
  // through Connection::Prepared() instead, which caches and resets
  // statements for reuse rather than preparing a fresh one every time.
  static absl::StatusOr<Statement> Prepare(
      Connection &db, std::string_view sql, unsigned int flags = 0);

  // The return status from Reset indicates whether or not the previous
  // evaluation of this prepared statement completed successfully.
  //
  // Any SQL statement variables that had values bound to them retain their
  // values -- see ClearBindings to also remove those.
  absl::Status Reset();

  // Resets all host parameters to NULL. Does not affect Step()'s position;
  // call Reset() (or Step() to completion) for that.
  absl::Status ClearBindings();

  // Bind() overloads. The leftmost SQL parameter has an index of 1, not 0.
  absl::Status Bind(int index, int64_t value);
  absl::Status Bind(int index, int value);
  // Stored as the int64 bit pattern of `value` (i.e. via std::bit_cast);
  // SQLite has no native unsigned integer type. Column<uint64_t> reverses
  // this, so values round-trip exactly, but the value stored in the
  // database is a signed 64-bit integer whose bit pattern happens to equal
  // `value` -- other tools reading the column directly will see it as
  // (possibly negative) int64.
  absl::Status Bind(int index, uint64_t value);
  absl::Status Bind(int index, double value);
  // Bound as the integer 0 or 1.
  absl::Status Bind(int index, bool value);
  // Text must be UTF-8. The bytes are copied (SQLITE_TRANSIENT), so `value`
  // need not outlive this call.
  absl::Status Bind(int index, std::string_view value);
  // Binds a BLOB. The bytes are copied (SQLITE_TRANSIENT). An empty span
  // binds a zero-length BLOB, not SQL NULL -- use Null()/nullopt for NULL.
  absl::Status Bind(int index, std::span<const uint8_t> value);
  // Binds SQL NULL. Same effect as Null(index).
  absl::Status Bind(int index, std::nullopt_t);
  absl::Status Null(int index);
  // Binds NULL for an empty optional, or delegates to the Bind() overload
  // for T otherwise.
  template <typename T>
    requires internal::IsOptional<T>::value
  absl::Status Bind(int index, const T &value);

  // Binds args... to parameters 1..sizeof...(args), in order. Stops (without
  // binding the rest) at the first failing Bind() and returns its status.
  template <typename... Args>
  absl::Status BindAll(Args &&...args);

  // Column() overloads for reading the current row. The leftmost column of
  // the result set has index 0. Undefined if there is no current row (i.e.
  // Step() has not been called, or last returned false) or if `index` is out
  // of range.
  template <std::same_as<int64_t> T>
  T Column(int index);
  template <std::same_as<int> T>
  T Column(int index);
  // Reverses Bind(index, uint64_t) -- see its comment.
  template <std::same_as<uint64_t> T>
  T Column(int index);
  template <std::same_as<double> T>
  T Column(int index);
  // True iff the column's integer value is nonzero.
  template <std::same_as<bool> T>
  T Column(int index);
  template <std::same_as<std::string> T>
  T Column(int index);
  // Valid until the next call to Step(), Reset(), or a type conversion of
  // this same column (e.g. calling Column<std::string>() on it).
  template <std::same_as<std::string_view> T>
  T Column(int index);
  template <std::same_as<std::vector<uint8_t>> T>
  T Column(int index);
  // nullopt iff the column is SQL NULL (checked via ColumnIsNull(), not by
  // inferring NULL from an empty/zero value -- this is what correctly tells
  // apart NULL from e.g. a zero-length blob). Otherwise delegates to
  // Column<T>().
  template <typename T>
    requires internal::IsOptional<T>::value
  T Column(int index);

  bool ColumnIsNull(int index);

  // Advances to the next row. Returns true if a row is now available (and
  // its columns may be read via Column()), false if the statement has
  // finished (SQLITE_DONE).
  absl::StatusOr<bool> Step();

  // Steps until done, calling fn(*this) once per row in order. Stops and
  // returns the first error encountered, from either Step() or fn(). Resets
  // the statement (but does not clear bindings) before returning, whether or
  // not that happened successfully.
  absl::Status ForEachRow(absl::FunctionRef<absl::Status(Statement &)> fn);

  // For statements that produce no rows (e.g. INSERT/UPDATE/DELETE without a
  // RETURNING clause): steps once, expecting SQLITE_DONE, then resets.
  absl::Status ExecuteOnce();

  // https://www.sqlite.org/c3ref/expanded_sql.html
  std::string_view Sql() const;
  // For logging only (VLOG(2)) -- expands bound parameter values into the
  // SQL text, which is not cheap.
  std::string ExpandedSql() const;

  sqlite3_stmt *Get() const;

  Statement(Statement &&);
  Statement(const Statement &) = delete;
  Statement &operator=(Statement &&);
  Statement &operator=(const Statement &) = delete;

 private:
  explicit Statement(sqlite3_stmt &stmt);

  // Builds the absl::Status for a failing sqlite3_* return code `rc`
  // produced against this statement, pulling the detailed message from the
  // owning connection (via sqlite3_db_handle). Returns OkStatus() for
  // SQLITE_OK.
  absl::Status StatusFromRc(int rc) const;

  sqlite3_stmt *stmt_ = nullptr;
};

// A single SQLite connection. Move-only: exactly one Connection should own a
// given `sqlite3*` handle at a time. Not thread-safe -- today dcfs is
// single-threaded, and later each thread/coroutine-runner is expected to
// have its own Connection (hence SQLITE_OPEN_NOMUTEX and no assumption
// anywhere in this file about the default VFS).
class Connection {
 public:
  Connection() = default;
  ~Connection();

  // Runs `sql` (which must be a single statement) to completion, discarding
  // any rows it produces. For statement-less DDL/pragmas; for anything that
  // takes parameters or returns rows you care about, use Prepared() instead.
  absl::Status Exec(std::string_view sql);

  // Runs `sql` as a script of any number of `;`-separated statements (via
  // sqlite3_exec), discarding any rows they produce. Unlike Exec(), none of
  // the statements can take bound parameters -- this is meant for running
  // schema DDL (e.g. applying schema.sql) in one call, not for anything
  // parameterized.
  absl::Status ExecScript(std::string_view sql);

  // Returns a RESET, unbound (all bindings cleared) prepared statement for
  // `sql`, from this connection's statement cache. Preparing the same SQL
  // text twice returns the *same* underlying Statement -- do not hold the
  // returned pointer across another Prepared() call for the same SQL, since
  // that call will reset and clear the bindings out from under you.
  absl::StatusOr<Statement *> Prepared(std::string_view sql);

  // Runs `body` inside a transaction: BEGIN IMMEDIATE, then COMMIT if body()
  // returns OK, else ROLLBACK and return body's status (annotated with the
  // ROLLBACK's own error too, if that also fails). A Transaction() called
  // from within another Transaction()'s body nests via SAVEPOINT/RELEASE/
  // ROLLBACK TO instead of BEGIN/COMMIT/ROLLBACK. `body` must not itself
  // suspend (e.g. no coroutine suspension points inside it).
  //
  // `durability` (see Durability) applies to the outermost transaction's
  // COMMIT. SQLite refuses to change the safety level inside a transaction
  // ("Safety level may not be changed inside a transaction"), so kSync
  // issues `PRAGMA synchronous=FULL` just before BEGIN and restores
  // `PRAGMA synchronous=NORMAL` (the connection default) after the
  // COMMIT/ROLLBACK, whatever the outcome; neither pragma does I/O. A
  // nested Transaction() asking for kSync inside an outer kNormal one is a
  // FailedPrecondition error (its commit is the outer one's, which would
  // not be synced); inside an outer kSync one it simply nests.
  absl::Status Transaction(absl::FunctionRef<absl::Status()> body,
                           Durability durability = Durability::kNormal);

  int64_t LastInsertRowId() const;
  int64_t Changes() const;
  // True iff a transaction (started by us, or otherwise) is active.
  bool InTransaction() const;

  // Runs a WAL checkpoint that writes every committed frame back into the
  // main database file and then truncates the WAL file to zero bytes
  // (SQLITE_CHECKPOINT_TRUNCATE), so nothing is left in it for the next
  // startup (or another process) to replay. A no-op (returns OkStatus)
  // when the database isn't in WAL mode (e.g. ":memory:"). See
  // https://www.sqlite.org/c3ref/wal_checkpoint_v2.html
  absl::Status Checkpoint();

  ::sqlite3 *Get() const;

  // Finalizes cached statements and closes the underlying database. Safe to
  // call more than once. The destructor calls this and discards any error,
  // since there is nowhere useful to report it -- call Close() explicitly if
  // you need to observe failures (e.g. SQLITE_BUSY from a statement this
  // connection didn't itself cache).
  absl::Status Close();

  Connection(Connection &&);
  Connection(const Connection &) = delete;
  Connection &operator=(Connection &&);
  Connection &operator=(const Connection &) = delete;

 private:
  friend class Statement;
  friend struct ConnectionFactory;

  explicit Connection(::sqlite3 &db);

  // Builds the absl::Status for the most recent failing call on this
  // connection's handle (sqlite3_extended_errcode + sqlite3_errmsg).
  absl::Status LastErrorStatus() const;

  // Rolls back the transaction/savepoint at nesting level `depth` (see
  // Transaction()) and returns `status`, annotated with the rollback's own
  // error too if that also fails. Used by Transaction() both when body()
  // fails and when a successful body()'s COMMIT/RELEASE itself fails --
  // either way, the transaction/savepoint must not be left open.
  absl::Status UnwindFailedTransaction(int depth, absl::Status status);

  // Transaction() without the durability handling.
  absl::Status RunTransaction(absl::FunctionRef<absl::Status()> body);

  ::sqlite3 *db_ = nullptr;
  // Owns every Statement handed out by Prepared(). unique_ptr so that
  // pointers returned by Prepared() stay valid across map rehashes.
  absl::flat_hash_map<std::string, std::unique_ptr<Statement>>
      statement_cache_;
  // Number of Transaction() calls currently nested (0 = no transaction
  // open). Only Transaction() touches this.
  int savepoint_depth_ = 0;
  // Whether the outermost open Transaction() was started with
  // Durability::kSync.
  bool sync_transaction_ = false;
};

// Opens a Connection. Nothing in Connection may assume the default VFS, so
// this is the only way to create one -- construct a ConnectionFactory naming
// the VFS (or leave vfs_name empty for the default) and call Open().
struct ConnectionFactory {
  std::string path;
  int flags = SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE;
  // Empty means the default VFS.
  std::string vfs_name;

  // Opens `path` via sqlite3_open_v2 (always OR'd with SQLITE_OPEN_NOMUTEX
  // and SQLITE_OPEN_EXRESCODE) and applies dcfs's standard pragmas:
  // journal_mode=WAL (best-effort; not applicable to e.g. ":memory:",
  // where it's skipped by simply ignoring its result), synchronous=NORMAL,
  // foreign_keys=ON, busy_timeout=5000, temp_store=MEMORY.
  absl::StatusOr<Connection> Open() const;
};

inline constexpr std::string_view kSqliteTypeUrl =
    "rus.har.mn/dcfs/status/sqlite";

// Maps a raw sqlite3 (extended) result code to the absl::StatusCode dcfs
// uses to represent it.
absl::StatusCode Sqlite3ErrorCodeToCanonical(int err);

// Builds a Status from a raw sqlite3 (extended) result code alone (i.e. with
// no live connection/statement to pull sqlite3_errmsg's more detailed text
// from -- callers with one should prefer building the Status from
// sqlite3_errmsg instead). The extended code is attached as a payload under
// kSqliteTypeUrl; see GetSqliteCodeFromStatus.
absl::Status Sqlite3ErrorCodeToStatus(int err);

// Recovers the sqlite3 extended result code from a Status produced by this
// file (i.e. one with a kSqliteTypeUrl payload).
absl::StatusOr<int> GetSqliteCodeFromStatus(const absl::Status &status);

// Implementation details below.

template <typename T>
  requires internal::IsOptional<T>::value
absl::Status Statement::Bind(int index, const T &value) {
  if (!value.has_value()) return Null(index);
  return Bind(index, *value);
}

template <typename... Args>
absl::Status Statement::BindAll(Args &&...args) {
  absl::Status status;
  // [[maybe_unused]]: with zero arguments the fold below expands to nothing
  // and -Wunused-but-set-variable would otherwise fire under -Werror.
  [[maybe_unused]] int index = 0;
  [[maybe_unused]] auto bind_one = [&](auto &&value) {
    if (!status.ok()) return;
    ++index;
    status = Bind(index, std::forward<decltype(value)>(value));
  };
  (bind_one(std::forward<Args>(args)), ...);
  return status;
}

template <std::same_as<int64_t> T>
T Statement::Column(int index) {
  return sqlite3_column_int64(stmt_, index);
}

template <std::same_as<int> T>
T Statement::Column(int index) {
  return sqlite3_column_int(stmt_, index);
}

template <std::same_as<uint64_t> T>
T Statement::Column(int index) {
  return std::bit_cast<uint64_t>(sqlite3_column_int64(stmt_, index));
}

template <std::same_as<double> T>
T Statement::Column(int index) {
  return sqlite3_column_double(stmt_, index);
}

template <std::same_as<bool> T>
T Statement::Column(int index) {
  return sqlite3_column_int(stmt_, index) != 0;
}

template <std::same_as<std::string> T>
T Statement::Column(int index) {
  const unsigned char *text = sqlite3_column_text(stmt_, index);
  int len = sqlite3_column_bytes(stmt_, index);
  if (text == nullptr) return std::string();
  return std::string(reinterpret_cast<const char *>(text), len);
}

template <std::same_as<std::string_view> T>
T Statement::Column(int index) {
  const unsigned char *text = sqlite3_column_text(stmt_, index);
  int len = sqlite3_column_bytes(stmt_, index);
  if (text == nullptr) return std::string_view();
  return std::string_view(reinterpret_cast<const char *>(text), len);
}

template <std::same_as<std::vector<uint8_t>> T>
T Statement::Column(int index) {
  const void *blob = sqlite3_column_blob(stmt_, index);
  int len = sqlite3_column_bytes(stmt_, index);
  if (blob == nullptr || len == 0) return std::vector<uint8_t>();
  const uint8_t *bytes = static_cast<const uint8_t *>(blob);
  return std::vector<uint8_t>(bytes, bytes + len);
}

template <typename T>
  requires internal::IsOptional<T>::value
T Statement::Column(int index) {
  if (ColumnIsNull(index)) return std::nullopt;
  return Column<typename internal::IsOptional<T>::value_type>(index);
}

}  // namespace sqlite3
}  // namespace dcfs

#endif  // DCFS_SQLITE_H_
