#include "dfs/status.h"

#include "absl/cord/cord.h"
#include "absl/strings/str_cat.h"

namespace dfs {

absl::Status Prepend(absl::Status st, std::string_view message) {
  absl::Status new_status(st.code(), absl::StrCat(message, "; ", st.message()));
  st.ForEachPayload(
      [&new_status](std::string_view type_url, const absl::Cord &payload) {
          new_status.SetPayload(type_url, payload);
      });
  return new_status;
}

}  // namespace
