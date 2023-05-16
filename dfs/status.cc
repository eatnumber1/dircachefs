#include "dfs/status.h"

#include "absl/strings/cord.h"
#include "absl/strings/str_cat.h"

namespace dfs {
namespace {

void CopyPayload(const absl::Status &from, absl::Status to) {
  from.ForEachPayload(
      [&to](std::string_view type_url, const absl::Cord &payload) {
          to.SetPayload(type_url, payload);
      });
}

}  // namespace

absl::Status Prepend(absl::Status st, std::string_view message, std::string_view joiner) {
  absl::Status new_status(st.code(), absl::StrCat(message, joiner, st.message()));
  CopyPayload(st, new_status);
  return new_status;
}

absl::Status Append(absl::Status st, std::string_view message, std::string_view joiner) {
  absl::Status new_status(st.code(), absl::StrCat(st.message(), joiner, message));
  CopyPayload(st, new_status);
  return new_status;
}

}  // namespace
