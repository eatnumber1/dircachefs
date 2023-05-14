#ifndef DFS_STATUS_H_
#define DFS_STATUS_H_

#include <string_view>

#include "absl/status/status.h"

namespace dfs {

absl::Status Prepend(absl::Status st, std::string_view message);

}  // namespace

#endif  // DFS_STATUS_H_
