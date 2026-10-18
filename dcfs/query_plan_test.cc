// SPDX-License-Identifier: MIT
#include <cstddef>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "absl/container/flat_hash_map.h"
#include "absl/container/flat_hash_set.h"
#include "absl/status/status.h"
#include "absl/status/status_macros.h"
#include "absl/status/status_matchers.h"
#include "absl/status/statusor.h"
#include "absl/strings/ascii.h"
#include "absl/strings/match.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/str_join.h"
#include "absl/strings/str_split.h"
#include "dcfs/device_id.h"
#include "dcfs/migrate.h"
#include "dcfs/sql_statements.h"
#include "dcfs/sqlite.h"
#include "dcfs/status.h"
#include "dcfs/testonly/assert_ok_and_assign.h"
#include "dcfs/testonly/files.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"

namespace dcfs {
namespace {

using ::absl_testing::IsOk;

std::string NormalizeSql(std::string_view sql) {
  std::string result;
  result.reserve(sql.size());
  bool in_space = false;
  for (char c : sql) {
    if (absl::ascii_isspace(c)) {
      if (!in_space && !result.empty()) {
        result.push_back(' ');
        in_space = true;
      }
    } else {
      result.push_back(c);
      in_space = false;
    }
  }
  if (!result.empty() && result.back() == ' ') {
    result.pop_back();
  }
  return result;
}

absl::StatusOr<std::string> LoadAllowlistContent() {
  constexpr std::string_view kPaths[] = {
      "/test/data/query_plan_allowlist.txt",
      "dcfs/query_plan_allowlist.txt",
      "query_plan_allowlist.txt",
  };
  for (std::string_view path : kPaths) {
    absl::StatusOr<std::string> content = testonly::ReadFileToString(path);
    if (content.ok()) return *content;
  }
  return NotFoundErrorBuilder() << "Could not find query_plan_allowlist.txt";
}

absl::Status AddEntry(std::string &current_sql, std::string_view raw_reason,
                      absl::flat_hash_map<std::string, std::string> &out) {
  std::string_view reason = absl::StripAsciiWhitespace(raw_reason);
  if (reason.empty()) {
    return InvalidArgumentErrorBuilder()
           << "Allowlist entry has empty reason for SQL: " << current_sql;
  }
  std::string norm = NormalizeSql(current_sql);
  if (norm.empty()) {
    return InvalidArgumentErrorBuilder()
           << "Allowlist entry has reason with no preceding SQL";
  }
  out[std::move(norm)] = std::string(reason);
  current_sql.clear();
  return absl::OkStatus();
}

absl::StatusOr<absl::flat_hash_map<std::string, std::string>> LoadAllowlist() {
  ASSIGN_OR_RETURN(std::string content, LoadAllowlistContent());
  absl::flat_hash_map<std::string, std::string> allowlist;
  std::string current_sql;

  for (std::string_view line : absl::StrSplit(content, '\n')) {
    line = absl::StripAsciiWhitespace(line);
    if (line.empty() || absl::StartsWith(line, "#")) continue;

    if (absl::StartsWith(line, "|")) {
      RETURN_IF_ERROR(AddEntry(current_sql, line.substr(1), allowlist));
      continue;
    }
    size_t pipe = line.find('|');
    if (pipe != std::string_view::npos) {
      if (!current_sql.empty()) current_sql.push_back(' ');
      current_sql.append(line.substr(0, pipe));
      RETURN_IF_ERROR(AddEntry(current_sql, line.substr(pipe + 1), allowlist));
      continue;
    }
    if (!current_sql.empty()) current_sql.push_back(' ');
    current_sql.append(line);
  }
  if (!current_sql.empty()) {
    return InvalidArgumentErrorBuilder()
           << "Allowlist ends with uncommitted SQL: " << current_sql;
  }
  return allowlist;
}

struct PlanResult {
  bool has_scan = false;
  std::vector<std::string> details;
};

absl::StatusOr<PlanResult> ExplainQueryPlan(sqlite3::Connection &db,
                                            std::string_view sql) {
  std::string explain = absl::StrCat("EXPLAIN QUERY PLAN ", sql);
  ASSIGN_OR_RETURN(sqlite3::Statement stmt,
                   sqlite3::Statement::Prepare(db, explain));
  PlanResult plan;
  RETURN_IF_ERROR(stmt.ForEachRow([&](sqlite3::Statement &row) {
    std::string detail = row.Column<std::string>(3);
    if (absl::StrContains(detail, "SCAN ") &&
        !absl::StrContains(detail, "SCAN CONSTANT ROW")) {
      plan.has_scan = true;
    }
    plan.details.push_back(std::move(detail));
    return absl::OkStatus();
  }));
  return plan;
}

class QueryPlanTest : public ::testing::Test {
 protected:
  void SetUp() override {
    ASSERT_OK_AND_ASSIGN(db_,
                         sqlite3::ConnectionFactory{.path = ":memory:"}.Open());
    DeviceId source;
    source.uuid.fill(0xAB);
    ASSERT_THAT(Migrate(db_, RootIdentity{.device_id = source,
                                          .fstype = 0xEF53,
                                          .backing_ino = 2,
                                          .backing_gen = 0}),
                IsOk());
  }

  sqlite3::Connection db_;
};

void CheckStatement(
    sqlite3::Connection &db, std::string_view sql,
    const absl::flat_hash_map<std::string, std::string> &allowlist,
    absl::flat_hash_set<std::string> &used_allowlist,
    std::vector<std::string> &failures) {
  std::string norm_sql = NormalizeSql(sql);
  absl::StatusOr<PlanResult> plan = ExplainQueryPlan(db, sql);
  if (!plan.ok()) {
    failures.push_back(
        absl::StrCat("Failed to prepare EXPLAIN QUERY PLAN for:\n  ", norm_sql,
                     "\n  Error: ", plan.status().ToString()));
    return;
  }

  auto allow_it = allowlist.find(norm_sql);
  if (plan->has_scan && allow_it == allowlist.end()) {
    failures.push_back(absl::StrCat(
        "Unindexed full-table SCAN in statement:\n  ", norm_sql,
        "\n  Query plan:\n    ", absl::StrJoin(plan->details, "\n    "),
        "\n  To allow, add to dcfs/query_plan_allowlist.txt with a reason."));
  } else if (plan->has_scan) {
    used_allowlist.insert(norm_sql);
  } else if (allow_it != allowlist.end()) {
    failures.push_back(absl::StrCat(
        "Statement is in allowlist, but produces no full-table SCAN:\n  ",
        norm_sql, "\n  Query plan:\n    ",
        absl::StrJoin(plan->details, "\n    "),
        "\n  Remove unnecessary entry from dcfs/query_plan_allowlist.txt."));
  }
}

TEST_F(QueryPlanTest, EveryStatementIsIndexedUnlessAllowlisted) {
  ASSERT_OK_AND_ASSIGN(auto allowlist, LoadAllowlist());
  absl::flat_hash_set<std::string> used_allowlist;
  std::vector<std::string> failures;

  for (std::string_view sql : AllSqlStatements()) {
    CheckStatement(db_, sql, allowlist, used_allowlist, failures);
  }

  for (const auto &[allowed_sql, reason] : allowlist) {
    if (!used_allowlist.contains(allowed_sql)) {
      failures.push_back(absl::StrCat(
          "Allowlist entry does not match any scanning statement in dcfs:\n  ",
          allowed_sql, "\n  Reason: ", reason));
    }
  }

  EXPECT_TRUE(failures.empty()) << "Query plan check failed with "
                                << failures.size() << " violation(s):\n\n"
                                << absl::StrJoin(failures, "\n\n");
}

TEST_F(QueryPlanTest, DetectsFullTableScanWhenIndexDropped) {
  ASSERT_OK_AND_ASSIGN(PlanResult before,
                       ExplainQueryPlan(db_, kSqlPresentDentryByInode));
  EXPECT_FALSE(before.has_scan);

  ASSERT_THAT(db_.Exec("DROP INDEX dentries_inode"), IsOk());

  ASSERT_OK_AND_ASSIGN(PlanResult after,
                       ExplainQueryPlan(db_, kSqlPresentDentryByInode));
  EXPECT_TRUE(after.has_scan);
  EXPECT_THAT(after.details,
              ::testing::Contains(::testing::HasSubstr("SCAN dentries")));
}

}  // namespace
}  // namespace dcfs
