#ifndef DFS_STATUS_H_
#define DFS_STATUS_H_

#include <string_view>

#include "absl/status/status.h"
#include "absl/status/statusor.h"

#define RETURN_IF_ERROR(expr) \
  if (auto st = (expr); !st.ok()) return st

#define ASSIGN_OR_RETURN(var, expr) \
  var = ({ \
    auto v = (expr); \
    if (!v.ok()) return std::move(v).status(); \
    *std::move(v); \
  })

#define LOG_IF_ERROR(level, expr) \
  if (absl::Status st = (expr); !st.ok()) LOG(level) << st

namespace dfs {

absl::Status Prepend(absl::Status st, std::string_view message, std::string_view joiner = "; ");
absl::Status Append(absl::Status st, std::string_view message, std::string_view joiner = "; ");

constexpr inline std::string_view kErrnoTypeUrl = "rus.har.mn/dfs/status/errno";
absl::Status ErrnoToStatus(int error_number, absl::string_view message);
absl::StatusOr<int> GetErrnoFromStatus(const absl::Status &status);

absl::StatusOr<int> ErrorNameToErrno(std::string_view error_name);
std::string ErrnoToErrorName(int error_number);

}  // namespace

#endif  // DFS_STATUS_H_
