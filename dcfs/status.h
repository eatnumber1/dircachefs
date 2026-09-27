#ifndef DCFS_STATUS_H_
#define DCFS_STATUS_H_

#include <string_view>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/log/log.h"

#define RETURN_IF_ERROR(expr) \
  ({ if (auto _st = (expr); !_st.ok()) return _st; })

#define ASSIGN_OR_RETURN(var, expr) \
  var = ({ \
    auto v = (expr); \
    if (!v.ok()) return std::move(v).status(); \
    *std::move(v); \
  })

#define LOG_IF_ERROR(level, expr) \
  ({ if (absl::Status _st = (expr); !_st.ok()) LOG(level) << _st; })

namespace dcfs {

absl::Status Prepend(absl::Status st, std::string_view message, std::string_view joiner = "; ");
absl::Status Append(absl::Status st, std::string_view message, std::string_view joiner = "; ");

constexpr inline std::string_view kErrnoTypeUrl = "rus.har.mn/dcfs/status/errno";
absl::Status ErrnoToStatus(int error_number, absl::string_view message);
absl::StatusOr<int> GetErrnoFromStatus(const absl::Status &status);

absl::StatusOr<int> ErrorNameToErrno(std::string_view error_name);
std::string ErrnoToErrorName(int error_number);

}  // namespace dcfs

#endif  // DCFS_STATUS_H_
