// Known-good input of status_message.query: a Status is put into a message
// whole (it implements AbslStringify), and absl::string_view::size and other
// classes' message() are not the Status's; and
// StatusToString(const absl::Status&) and SerializeStatus(const
// absl::Status&) are the exempt helpers: the first for domain answers, the
// second serializes the whole Status.
#include <string>
#include <string_view>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/str_cat.h"

absl::Status Fetch();
absl::StatusOr<int> Count();

struct Greeting {
  std::string message() const { return "hello"; }
};

std::string_view StatusToString(const absl::Status &status) {
  return status.message();
}

std::string SerializeStatus(const absl::Status &status) {
  return std::string(status.message());
}

std::string Describe(const Greeting &greeting) {
  absl::Status status = Fetch();
  absl::StatusOr<int> count = Count();
  return absl::StrCat("fetch: ", status, "; count: ", count.status(), "; ",
                      greeting.message());
}
