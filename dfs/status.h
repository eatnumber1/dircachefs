#ifndef DFS_STATUS_H_
#define DFS_STATUS_H_

#include <string_view>

#include "absl/status/status.h"
#include "absl/log/log.h"

#define RETURN_IF_ERROR(expr) \
  if (auto st = (expr); !st.ok()) return st

#define ASSIGN_OR_RETURN(var, expr) \
  var = ({ \
    auto v = (expr); \
    if (!v.ok()) return std::move(v).status(); \
    *std::move(v); \
  })

namespace dfs {

absl::Status Prepend(absl::Status st, std::string_view message, std::string_view joiner = "; ");
absl::Status Append(absl::Status st, std::string_view message, std::string_view joiner = "; ");
void LogIfError(absl::Status status);

}  // namespace

#endif  // DFS_STATUS_H_
