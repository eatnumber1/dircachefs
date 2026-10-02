#include "dcfs/sqlite.h"

#include <sys/stat.h>

#include <cstdint>
#include <cstdlib>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "absl/status/status.h"
#include "absl/status/status_macros.h"
#include "absl/status/status_matchers.h"
#include "absl/status/statusor.h"
#include "absl/strings/str_cat.h"
#include "dcfs/ret_check.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "sqlite3.h"

// This version of Abseil's status_matchers.h doesn't provide
// ASSERT_OK_AND_ASSIGN, so define the usual helper locally (scoped to this
// file only).
#define DCFS_TEST_CONCAT_INNER(x, y) x##y
#define DCFS_TEST_CONCAT(x, y) DCFS_TEST_CONCAT_INNER(x, y)
#define ASSERT_OK_AND_ASSIGN(lhs, rexpr)                        \
  ASSERT_OK_AND_ASSIGN_IMPL(                                    \
      DCFS_TEST_CONCAT(_status_or_value_, __LINE__), lhs, rexpr)
#define ASSERT_OK_AND_ASSIGN_IMPL(statusor, lhs, rexpr) \
  auto statusor = (rexpr);                              \
  ASSERT_THAT(statusor, ::absl_testing::IsOk());        \
  lhs = std::move(statusor).value()

namespace dcfs {
namespace sqlite3 {
namespace {

using ::absl_testing::IsOk;
using ::absl_testing::IsOkAndHolds;
using ::absl_testing::StatusIs;
using ::testing::ElementsAre;
using ::testing::IsEmpty;

std::string TestTmpFile(std::string_view name) {
  const char *tmpdir = std::getenv("TEST_TMPDIR");
  if (tmpdir == nullptr) {
    ADD_FAILURE() << "TEST_TMPDIR must be set when running under bazel test";
    return std::string(name);
  }
  return absl::StrCat(tmpdir, "/", name);
}

absl::StatusOr<Connection> OpenMemory() {
  return ConnectionFactory{.path = ":memory:"}.Open();
}

TEST(ConnectionTest, OpensInMemoryDatabase) {
  ASSERT_THAT(OpenMemory(), IsOk());
}

TEST(ConnectionTest, OpensFileBackedDatabase) {
  std::string path = TestTmpFile("file_backed.sqlite");
  ASSERT_THAT(ConnectionFactory{.path = path}.Open(), IsOk());
}

TEST(ConnectionTest, FileBackedReportsWalAndForeignKeys) {
  std::string path = TestTmpFile("wal_check.sqlite");
  ASSERT_OK_AND_ASSIGN(Connection conn, ConnectionFactory{.path = path}.Open());

  ASSERT_OK_AND_ASSIGN(Statement * mode_stmt, conn.Prepared("PRAGMA journal_mode"));
  ASSERT_THAT(mode_stmt->Step(), IsOkAndHolds(true));
  EXPECT_EQ(mode_stmt->Column<std::string>(0), "wal");
  ASSERT_THAT(mode_stmt->Step(), IsOkAndHolds(false));

  ASSERT_OK_AND_ASSIGN(Statement * fk_stmt, conn.Prepared("PRAGMA foreign_keys"));
  ASSERT_THAT(fk_stmt->Step(), IsOkAndHolds(true));
  EXPECT_EQ(fk_stmt->Column<int>(0), 1);
  ASSERT_THAT(fk_stmt->Step(), IsOkAndHolds(false));
}

// Audit crash F9: a file-backed database that cannot run in WAL mode must
// not open. SQLite answers `PRAGMA journal_mode=WAL` with the mode it
// actually kept, not an error, when the VFS cannot provide WAL's shared
// memory -- here SQLite's own "unix-none" VFS (no locking, version-1 I/O
// methods without xShmMap) -- and in rollback-journal mode
// synchronous=NORMAL is not durable at commit.
TEST(ConnectionTest, FileBackedWithoutWalFailsToOpen) {
  std::string path = TestTmpFile("no_wal.sqlite");
  absl::StatusOr<Connection> conn =
      ConnectionFactory{.path = path, .vfs_name = "unix-none"}.Open();
  ASSERT_FALSE(conn.ok()) << "opened in a journal mode other than WAL";
  EXPECT_EQ(conn.status().code(), absl::StatusCode::kFailedPrecondition)
      << conn.status();
  EXPECT_THAT(conn.status().message(), testing::HasSubstr("journal_mode"));
}

// An in-memory database has no WAL ("memory") and still opens: the unit
// tests use them.
TEST(ConnectionTest, InMemoryDatabaseOpensWithoutWal) {
  ASSERT_OK_AND_ASSIGN(Connection conn, OpenMemory());
  ASSERT_OK_AND_ASSIGN(Statement * mode_stmt,
                       conn.Prepared("PRAGMA journal_mode"));
  ASSERT_THAT(mode_stmt->Step(), IsOkAndHolds(true));
  EXPECT_EQ(mode_stmt->Column<std::string>(0), "memory");
}

// Regression test for main.cc's shutdown path, which relies on Checkpoint()
// to leave nothing in the WAL for the next startup to replay.
TEST(ConnectionTest, CheckpointTruncatesWal) {
  std::string path = TestTmpFile("checkpoint.sqlite");
  ASSERT_OK_AND_ASSIGN(Connection conn, ConnectionFactory{.path = path}.Open());
  ASSERT_THAT(conn.Exec("CREATE TABLE t (id INTEGER PRIMARY KEY)"), IsOk());
  ASSERT_THAT(conn.Exec("INSERT INTO t (id) VALUES (1)"), IsOk());

  std::string wal_path = absl::StrCat(path, "-wal");
  struct stat before_stat;
  ASSERT_EQ(::stat(wal_path.c_str(), &before_stat), 0);
  EXPECT_GT(before_stat.st_size, 0)
      << "expected a nonempty -wal file before checkpointing";

  ASSERT_THAT(conn.Checkpoint(), IsOk());

  struct stat after_stat;
  ASSERT_EQ(::stat(wal_path.c_str(), &after_stat), 0);
  EXPECT_EQ(after_stat.st_size, 0);

  // The data committed before the checkpoint must still be there.
  ASSERT_OK_AND_ASSIGN(Statement * select, conn.Prepared("SELECT id FROM t"));
  ASSERT_THAT(select->Step(), IsOkAndHolds(true));
  EXPECT_EQ(select->Column<int64_t>(0), 1);
}

TEST(ConnectionTest, CheckpointOnInMemoryDatabaseIsANoOp) {
  ASSERT_OK_AND_ASSIGN(Connection conn, OpenMemory());
  ASSERT_THAT(conn.Exec("CREATE TABLE t (id INTEGER PRIMARY KEY)"), IsOk());
  EXPECT_THAT(conn.Checkpoint(), IsOk());
}

// Fixture with a single table with one column per Bind/Column overload this
// wrapper supports.
class StatementTest : public ::testing::Test {
 protected:
  void SetUp() override {
    ASSERT_OK_AND_ASSIGN(conn_, OpenMemory());
    ASSERT_THAT(
        conn_.Exec(
          "CREATE TABLE t ("
          "  id INTEGER PRIMARY KEY,"
          "  i64 INTEGER,"
          "  i32 INTEGER,"
          "  u64 INTEGER,"
          "  dbl REAL,"
          "  boolean INTEGER,"
          "  text TEXT,"
          "  blob BLOB,"
          "  opt_text TEXT,"
          "  opt_blob BLOB"
          ")"),
        IsOk());
  }

  Connection conn_;
};

TEST_F(StatementTest, BindAndColumnRoundTripEveryOverload) {
  ASSERT_OK_AND_ASSIGN(
      Statement * insert,
      conn_.Prepared(
        "INSERT INTO t "
        "(id, i64, i32, u64, dbl, boolean, text, blob, opt_text, opt_blob) "
        "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?)"));

  constexpr uint64_t kHighBit = uint64_t{1} << 63;
  const std::vector<uint8_t> kBlobBytes = {0xDE, 0xAD, 0xBE, 0xEF};

  ASSERT_THAT(insert->Bind(1, int64_t{42}), IsOk());
  ASSERT_THAT(insert->Bind(2, int64_t{-7}), IsOk());
  ASSERT_THAT(insert->Bind(3, int{123}), IsOk());
  ASSERT_THAT(insert->Bind(4, kHighBit), IsOk());
  ASSERT_THAT(insert->Bind(5, 3.5), IsOk());
  ASSERT_THAT(insert->Bind(6, true), IsOk());
  ASSERT_THAT(insert->Bind(7, std::string_view("hello")), IsOk());
  ASSERT_THAT(
      insert->Bind(8, std::span<const uint8_t>(kBlobBytes)), IsOk());
  ASSERT_THAT(
      insert->Bind(9, std::optional<std::string_view>("present")), IsOk());
  ASSERT_THAT(
      insert->Bind(10, std::optional<std::span<const uint8_t>>(std::nullopt)),
      IsOk());
  ASSERT_THAT(insert->ExecuteOnce(), IsOk());

  EXPECT_EQ(conn_.LastInsertRowId(), 42);
  EXPECT_EQ(conn_.Changes(), 1);

  ASSERT_OK_AND_ASSIGN(
      Statement * select,
      conn_.Prepared(
        "SELECT id, i64, i32, u64, dbl, boolean, text, blob, opt_text, "
        "opt_blob FROM t WHERE id = ?"));
  ASSERT_THAT(select->Bind(1, int64_t{42}), IsOk());
  ASSERT_THAT(select->Step(), IsOkAndHolds(true));

  EXPECT_EQ(select->Column<int64_t>(0), 42);
  EXPECT_EQ(select->Column<int64_t>(1), -7);
  EXPECT_EQ(select->Column<int>(2), 123);
  EXPECT_EQ(select->Column<uint64_t>(3), kHighBit);
  EXPECT_EQ(select->Column<double>(4), 3.5);
  EXPECT_EQ(select->Column<bool>(5), true);
  EXPECT_EQ(select->Column<std::string>(6), "hello");
  EXPECT_EQ(select->Column<std::string_view>(6), "hello");
  EXPECT_THAT(select->Column<std::vector<uint8_t>>(7), ElementsAre(0xDE, 0xAD, 0xBE, 0xEF));

  EXPECT_FALSE(select->ColumnIsNull(6));
  EXPECT_TRUE(select->ColumnIsNull(9));

  auto opt_text = select->Column<std::optional<std::string>>(8);
  ASSERT_TRUE(opt_text.has_value());
  EXPECT_EQ(*opt_text, "present");

  auto opt_blob = select->Column<std::optional<std::vector<uint8_t>>>(9);
  EXPECT_FALSE(opt_blob.has_value());

  ASSERT_THAT(select->Step(), IsOkAndHolds(false));
}

TEST_F(StatementTest, ZeroLengthBlobIsNotNull) {
  ASSERT_OK_AND_ASSIGN(
      Statement * insert,
      conn_.Prepared("INSERT INTO t (id, blob, opt_blob) VALUES (1, ?, ?)"));
  ASSERT_THAT(insert->Bind(1, std::span<const uint8_t>()), IsOk());
  ASSERT_THAT(insert->Bind(2, std::nullopt), IsOk());
  ASSERT_THAT(insert->ExecuteOnce(), IsOk());

  ASSERT_OK_AND_ASSIGN(
      Statement * select,
      conn_.Prepared("SELECT blob, opt_blob FROM t WHERE id = 1"));
  ASSERT_THAT(select->Step(), IsOkAndHolds(true));

  EXPECT_FALSE(select->ColumnIsNull(0));
  EXPECT_THAT(select->Column<std::vector<uint8_t>>(0), IsEmpty());

  auto opt_blob = select->Column<std::optional<std::vector<uint8_t>>>(1);
  EXPECT_FALSE(opt_blob.has_value());
}

TEST_F(StatementTest, PreparedReturnsSameCachedStatementResetAndUnbound) {
  ASSERT_OK_AND_ASSIGN(
      Statement * first, conn_.Prepared("SELECT ? AS x"));
  sqlite3_stmt *raw_first = first->Get();
  ASSERT_THAT(first->Bind(1, int64_t{99}), IsOk());
  ASSERT_THAT(first->Step(), IsOkAndHolds(true));
  EXPECT_EQ(first->Column<int64_t>(0), 99);

  // Re-requesting the same SQL text returns the same underlying statement,
  // reset and with bindings cleared.
  ASSERT_OK_AND_ASSIGN(
      Statement * second, conn_.Prepared("SELECT ? AS x"));
  EXPECT_EQ(second->Get(), raw_first);

  // Unbound parameter reads back as NULL now.
  ASSERT_THAT(second->Step(), IsOkAndHolds(true));
  EXPECT_TRUE(second->ColumnIsNull(0));
  ASSERT_THAT(second->Step(), IsOkAndHolds(false));
}

TEST_F(StatementTest, ForEachRowVisitsAllRowsInOrderAndStopsOnError) {
  ASSERT_THAT(conn_.Exec("INSERT INTO t (id) VALUES (1), (2), (3)"), IsOk());

  ASSERT_OK_AND_ASSIGN(
      Statement * select, conn_.Prepared("SELECT id FROM t ORDER BY id"));
  std::vector<int64_t> seen;
  absl::Status st = select->ForEachRow([&](Statement &s) -> absl::Status {
    seen.push_back(s.Column<int64_t>(0));
    return absl::OkStatus();
  });
  ASSERT_THAT(st, IsOk());
  EXPECT_THAT(seen, ElementsAre(1, 2, 3));

  ASSERT_OK_AND_ASSIGN(
      Statement * select2, conn_.Prepared("SELECT id FROM t ORDER BY id"));
  std::vector<int64_t> seen2;
  absl::Status stop_status = select2->ForEachRow([&](Statement &s) -> absl::Status {
    seen2.push_back(s.Column<int64_t>(0));
    if (seen2.size() == 2) return absl::InternalError("stop here");
    return absl::OkStatus();
  });
  EXPECT_THAT(stop_status, StatusIs(absl::StatusCode::kInternal));
  EXPECT_THAT(seen2, ElementsAre(1, 2));
}

TEST_F(StatementTest, UniqueViolationMapsToAlreadyExists) {
  ASSERT_THAT(conn_.Exec("INSERT INTO t (id) VALUES (1)"), IsOk());
  ASSERT_OK_AND_ASSIGN(
      Statement * insert, conn_.Prepared("INSERT INTO t (id) VALUES (1)"));
  absl::StatusOr<bool> result = insert->Step();
  ASSERT_FALSE(result.ok());
  EXPECT_EQ(result.status().code(), absl::StatusCode::kAlreadyExists);

  ASSERT_OK_AND_ASSIGN(int code, GetSqliteCodeFromStatus(result.status()));
  EXPECT_EQ(code, SQLITE_CONSTRAINT_PRIMARYKEY);
}

TEST(ExecScriptTest, RunsMultipleStatementsAndSkipsCommentSemicolons) {
  ASSERT_OK_AND_ASSIGN(Connection conn, OpenMemory());

  ASSERT_THAT(
      conn.ExecScript(
        "-- comment; with semicolon\n"
        "CREATE TABLE a(x); INSERT INTO a VALUES(1);"),
      IsOk());

  ASSERT_OK_AND_ASSIGN(Statement * select, conn.Prepared("SELECT x FROM a"));
  ASSERT_THAT(select->Step(), IsOkAndHolds(true));
  EXPECT_EQ(select->Column<int64_t>(0), 1);
  ASSERT_THAT(select->Step(), IsOkAndHolds(false));
}

TEST(ExecScriptTest, SyntaxErrorReturnsNonOk) {
  ASSERT_OK_AND_ASSIGN(Connection conn, OpenMemory());

  EXPECT_FALSE(conn.ExecScript("CREATE TABLE ;").ok());
}

class TransactionTest : public ::testing::Test {
 protected:
  void SetUp() override {
    ASSERT_OK_AND_ASSIGN(conn_, OpenMemory());
    ASSERT_THAT(conn_.Exec("CREATE TABLE t (id INTEGER PRIMARY KEY)"), IsOk());
  }

  absl::StatusOr<int64_t> RowCount() {
    ABSL_ASSIGN_OR_RETURN(Statement * stmt, conn_.Prepared("SELECT COUNT(*) FROM t"));
    ABSL_ASSIGN_OR_RETURN(bool has_row, stmt->Step());
    RET_CHECK(has_row);
    int64_t count = stmt->Column<int64_t>(0);
    ABSL_RETURN_IF_ERROR(stmt->Reset());
    return count;
  }

  bool RowExists(int64_t id) {
    absl::StatusOr<Statement *> stmt =
        conn_.Prepared("SELECT 1 FROM t WHERE id = ?");
    if (!stmt.ok()) return false;
    if (!(*stmt)->Bind(1, id).ok()) return false;
    absl::StatusOr<bool> has_row = (*stmt)->Step();
    bool exists = has_row.ok() && *has_row;
    (*stmt)->Reset().IgnoreError();
    return exists;
  }

  Connection conn_;
};

TEST_F(TransactionTest, CommitsOnOk) {
  EXPECT_FALSE(conn_.InTransaction());
  absl::Status st = conn_.Transaction([&]() -> absl::Status {
    EXPECT_TRUE(conn_.InTransaction());
    return conn_.Exec("INSERT INTO t (id) VALUES (1)");
  });
  ASSERT_THAT(st, IsOk());
  EXPECT_FALSE(conn_.InTransaction());
  EXPECT_TRUE(RowExists(1));
  EXPECT_THAT(RowCount(), IsOkAndHolds(1));
}

TEST_F(TransactionTest, RollsBackOnErrorAndReturnsIt) {
  absl::Status st = conn_.Transaction([&]() -> absl::Status {
    ABSL_RETURN_IF_ERROR(conn_.Exec("INSERT INTO t (id) VALUES (1)"));
    return absl::InternalError("body failed");
  });
  EXPECT_THAT(st, StatusIs(absl::StatusCode::kInternal));
  EXPECT_FALSE(conn_.InTransaction());
  EXPECT_THAT(RowCount(), IsOkAndHolds(0));
}

TEST_F(TransactionTest, NestedInnerFailsOuterCommits) {
  absl::Status st = conn_.Transaction([&]() -> absl::Status {
    ABSL_RETURN_IF_ERROR(conn_.Exec("INSERT INTO t (id) VALUES (1)"));

    absl::Status inner = conn_.Transaction([&]() -> absl::Status {
      ABSL_RETURN_IF_ERROR(conn_.Exec("INSERT INTO t (id) VALUES (2)"));
      return absl::InternalError("inner failed");
    });
    EXPECT_THAT(inner, StatusIs(absl::StatusCode::kInternal));

    return absl::OkStatus();
  });
  ASSERT_THAT(st, IsOk());
  EXPECT_TRUE(RowExists(1));
  EXPECT_FALSE(RowExists(2));
}

TEST_F(TransactionTest, NestedInnerSucceedsOuterFails) {
  absl::Status st = conn_.Transaction([&]() -> absl::Status {
    absl::Status inner = conn_.Transaction([&]() -> absl::Status {
      return conn_.Exec("INSERT INTO t (id) VALUES (2)");
    });
    EXPECT_THAT(inner, IsOk());
    EXPECT_TRUE(RowExists(2));

    return absl::InternalError("outer failed");
  });
  EXPECT_THAT(st, StatusIs(absl::StatusCode::kInternal));
  EXPECT_FALSE(RowExists(2));
  EXPECT_THAT(RowCount(), IsOkAndHolds(0));
}

absl::StatusOr<int> Synchronous(Connection &conn) {
  ABSL_ASSIGN_OR_RETURN(Statement * stmt, conn.Prepared("PRAGMA synchronous"));
  ABSL_ASSIGN_OR_RETURN(bool has_row, stmt->Step());
  RET_CHECK(has_row);
  int level = stmt->Column<int>(0);
  ABSL_RETURN_IF_ERROR(stmt->Reset());
  return level;
}

// PRAGMA synchronous reports 1 for NORMAL and 2 for FULL. A file-backed
// database, so journal_mode really is WAL (the combination kSync exists
// for).
TEST(DurabilityTest, SyncTransactionRunsWithSynchronousFull) {
  std::string path = TestTmpFile("durability.sqlite");
  ASSERT_OK_AND_ASSIGN(Connection conn, ConnectionFactory{.path = path}.Open());
  ASSERT_THAT(conn.Exec("CREATE TABLE t (id INTEGER PRIMARY KEY)"), IsOk());
  EXPECT_THAT(Synchronous(conn), IsOkAndHolds(1));

  absl::StatusOr<int> inside;
  absl::StatusOr<int> nested;
  ASSERT_THAT(conn.Transaction(
                  [&]() -> absl::Status {
                    inside = Synchronous(conn);
                    ABSL_RETURN_IF_ERROR(
                        conn.Exec("INSERT INTO t (id) VALUES (1)"));
                    // A nested kSync inside a kSync transaction just nests.
                    return conn.Transaction(
                        [&]() -> absl::Status {
                          nested = Synchronous(conn);
                          return absl::OkStatus();
                        },
                        Durability::kSync);
                  },
                  Durability::kSync),
              IsOk());
  EXPECT_THAT(inside, IsOkAndHolds(2));
  EXPECT_THAT(nested, IsOkAndHolds(2));
  EXPECT_THAT(Synchronous(conn), IsOkAndHolds(1));

  // A plain transaction runs at the default, NORMAL.
  absl::StatusOr<int> plain;
  ASSERT_THAT(conn.Transaction([&]() -> absl::Status {
    plain = Synchronous(conn);
    return absl::OkStatus();
  }),
              IsOk());
  EXPECT_THAT(plain, IsOkAndHolds(1));

  // NORMAL is restored after a failed kSync transaction too.
  EXPECT_THAT(conn.Transaction(
                  [&]() -> absl::Status {
                    return absl::InternalError("body failed");
                  },
                  Durability::kSync),
              StatusIs(absl::StatusCode::kInternal));
  EXPECT_FALSE(conn.InTransaction());
  EXPECT_THAT(Synchronous(conn), IsOkAndHolds(1));
}

TEST(DurabilityTest, SyncInsideNormalTransactionIsRejected) {
  ASSERT_OK_AND_ASSIGN(Connection conn, OpenMemory());
  absl::Status inner;
  ASSERT_THAT(conn.Transaction([&]() -> absl::Status {
    inner = conn.Transaction([] { return absl::OkStatus(); },
                             Durability::kSync);
    return absl::OkStatus();
  }),
              IsOk());
  EXPECT_THAT(inner, StatusIs(absl::StatusCode::kFailedPrecondition));
  EXPECT_FALSE(conn.InTransaction());
}

// Regression test for a failing COMMIT (as opposed to a failing body())
// leaving the transaction open. Two connections to the same file: B holds a
// read cursor open (a SHARED lock, in rollback-journal mode) while A runs a
// Transaction() that writes a row; A's BEGIN IMMEDIATE succeeds (RESERVED is
// compatible with B's SHARED), but its COMMIT needs an EXCLUSIVE lock, which
// B's SHARED lock blocks -- with busy_timeout=0 on A, that COMMIT fails with
// SQLITE_BUSY immediately. WAL mode's readers don't block a writer's commit,
// so this needs the older rollback-journal mode instead.
TEST(TransactionUnwindTest, FailedCommitUnwindsAndConnectionStaysUsable) {
  std::string path = TestTmpFile("commit_busy.sqlite");

  ASSERT_OK_AND_ASSIGN(Connection a, ConnectionFactory{.path = path}.Open());
  ASSERT_THAT(a.Exec("PRAGMA journal_mode=DELETE"), IsOk());
  ASSERT_THAT(a.Exec("CREATE TABLE t (id INTEGER PRIMARY KEY)"), IsOk());
  ASSERT_THAT(a.Exec("INSERT INTO t (id) VALUES (1)"), IsOk());
  // Fail fast instead of waiting out the (5s default) busy timeout.
  ASSERT_THAT(a.Exec("PRAGMA busy_timeout=0"), IsOk());

  ASSERT_OK_AND_ASSIGN(Connection b, ConnectionFactory{.path = path}.Open());
  ASSERT_THAT(b.Exec("PRAGMA journal_mode=DELETE"), IsOk());

  // Leave a read cursor open on B, holding a SHARED lock on the file.
  ASSERT_OK_AND_ASSIGN(Statement * b_select, b.Prepared("SELECT id FROM t"));
  ASSERT_THAT(b_select->Step(), IsOkAndHolds(true));

  absl::Status txn_status = a.Transaction([&]() -> absl::Status {
    return a.Exec("INSERT INTO t (id) VALUES (2)");
  });
  EXPECT_THAT(txn_status, StatusIs(absl::StatusCode::kUnavailable));
  EXPECT_FALSE(a.InTransaction());

  // The failed COMMIT must have rolled back id=2 along with everything else.
  ASSERT_OK_AND_ASSIGN(
      Statement * count_stmt, a.Prepared("SELECT COUNT(*) FROM t"));
  ASSERT_THAT(count_stmt->Step(), IsOkAndHolds(true));
  EXPECT_EQ(count_stmt->Column<int64_t>(0), 1);
  ASSERT_THAT(count_stmt->Reset(), IsOk());

  // Once B releases its lock, A must be fully usable again.
  ASSERT_THAT(b_select->Reset(), IsOk());
  absl::Status second_txn = a.Transaction([&]() -> absl::Status {
    return a.Exec("INSERT INTO t (id) VALUES (3)");
  });
  EXPECT_THAT(second_txn, IsOk());
  EXPECT_FALSE(a.InTransaction());
}

}  // namespace
}  // namespace sqlite3
}  // namespace dcfs
