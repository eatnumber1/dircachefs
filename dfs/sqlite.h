#ifndef DFS_SQLITE_H_
#define DFS_SQLITE_H_

#include <string>
#include <string_view>
#include <optional>
#include <ostream>

#include "absl/functional/function_ref.h"
#include "absl/status/statusor.h"
#include "absl/status/status.h"
#include "sqlite/sqlite3.h"

namespace dfs {

class Sqlite3 {
 public:
  Sqlite3() = default;
  ~Sqlite3();

  static absl::StatusOr<Sqlite3> Open(
      const char *filename, int flags = 0, const char *vfs = nullptr);

  sqlite3 &operator*();
  sqlite3 *Get();

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

  Sqlite3(Sqlite3 &&);
  Sqlite3(const Sqlite3 &) = delete;
  Sqlite3 &operator=(Sqlite3 &&);
  Sqlite3 &operator=(const Sqlite3 &) = delete;

 private:
  Sqlite3(sqlite3 &db);

  sqlite3 *db_ = nullptr;
};

absl::Status Sqlite3ErrorCodeToStatus(int err);
absl::StatusCode Sqlite3ErrorCodeToCanonical(int err);
std::string Sqlite3ErrorCodeToString(int err);

class Sqlite3Stmt {
 public:
  Sqlite3Stmt() = default;
  ~Sqlite3Stmt();

  static absl::StatusOr<Sqlite3Stmt> Prepare(Sqlite3 &db, std::string_view sql, unsigned int flags = 0);

  absl::Status Finalize() &&;

  enum class StepResult {
    kOk,   // SQLITE_OK
    kDone, // SQLITE_DONE
    kRow,  // SQLITE_ROW
  };
  absl::StatusOr<StepResult> Step();

  sqlite3_stmt &operator*();
  sqlite3_stmt *Get();

  Sqlite3Stmt(Sqlite3Stmt &&);
  Sqlite3Stmt(const Sqlite3Stmt &) = delete;
  Sqlite3Stmt &operator=(Sqlite3Stmt &&);
  Sqlite3Stmt &operator=(const Sqlite3Stmt &) = delete;

 private:
  Sqlite3Stmt(sqlite3_stmt &stmt);

  sqlite3_stmt *stmt_ = nullptr;
};

std::ostream &operator<<(std::ostream &os, Sqlite3Stmt::StepResult res);

}  // namespace dfs

#endif  // DFS_SQLITE_H_
