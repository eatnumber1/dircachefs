#include "dcfs/mount_table.h"

#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "absl/cleanup/cleanup.h"
#include "absl/functional/function_ref.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/str_cat.h"
#include "dcfs/status.h"
#include "libmount.h"

namespace dcfs {
namespace {

std::vector<std::pair<std::string, int>> WithParseErrorCollector(
    libmnt_table &mtab, absl::FunctionRef<void()> callback) {
  thread_local std::vector<std::pair<std::string, int>> files_and_lines;
  absl::Cleanup reset_vec([&]() { files_and_lines.clear(); });

  mnt_table_set_parser_errcb(
      &mtab,
      [](libmnt_table *, const char *filename, int line) -> int {
        files_and_lines.push_back({std::string(filename), line});
        // Return codes are:
        //  - <0 : fatal error (abort parsing)
        //  - 0 : success (parsing continues)
        //  - >0 : recoverable error (the line is ignored, parsing continues).
        return 1;
      });
  absl::Cleanup reset_errcb([&]() { mnt_table_set_parser_errcb(&mtab, nullptr); });

  callback();

  // Move from thread-local storage to local storage before returning, so
  // Cleanup can reset the thread-local vector on return.
  std::vector<std::pair<std::string, int>> ret = std::move(files_and_lines);
  return ret;
}

}  // namespace

using MountTableUniquePtr =
    std::unique_ptr<libmnt_table, decltype(&mnt_unref_table)>;

absl::StatusOr<MountTableUniquePtr> CreateMountTableFromMtab() {
  ASSIGN_OR_RETURN(
    auto mtab, []() -> absl::StatusOr<MountTableUniquePtr> {
      libmnt_table *mtab = mnt_new_table();
      if (mtab == nullptr) return absl::UnknownError("mnt_new_table returned null");
      return MountTableUniquePtr(mtab, mnt_unref_table);
    }());

  absl::Status status;
  std::vector<std::pair<std::string, int>> parse_errors =
    WithParseErrorCollector(*mtab, [&]() {
      status = [&]() {
        errno = 0;
        int rc = mnt_table_parse_mtab(mtab.get(), /*filename=*/nullptr);
        if (rc == 0) return absl::OkStatus();
        if (errno != 0) {
          return absl::ErrnoToStatus(errno, "mnt_table_parse_mtab");
        }
        return absl::UnknownError(
            absl::StrCat("mnt_table_parse_mtab failed with code", rc));
      }();
    });

  if (status.ok() && !parse_errors.empty()) {
    status = absl::UnknownError("mnt_table_parse_mtab encountered syntax errors");
  }
  for (const auto &[file, line] : parse_errors) {
    status = Append(std::move(status), absl::StrFormat("syntax error at %s:%d", file, line));
  }
  RETURN_IF_ERROR(status);

  return mtab;
}

}  // namespace dcfs
