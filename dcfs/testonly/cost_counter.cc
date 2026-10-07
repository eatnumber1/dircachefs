#include "dcfs/testonly/cost_counter.h"

#include <cstdint>
#include <string>
#include <string_view>

#include "absl/strings/str_cat.h"
#include "dcfs/syscalls_backing.h"
#include "dcfs/testonly/invariant_checker.h"

namespace dcfs::testonly {

void CostCounter::SqliteStep(std::string_view sql) {
  if (!sql.starts_with(InvariantChecker::kSqlMarker)) ++counts_.steps;
}

void CostCounter::SqliteTransaction(bool durable) {
  ++counts_.transactions;
  if (durable) ++counts_.durable_transactions;
}

void CostCounter::BackingCall(Context &ctx, std::string_view what,
                              absl::SourceLocation site) {
  ++counts_.backing_calls;
}

void CostCounter::CheckRequestBegin(Context &ctx, const DirCacheFS &fs,
                                    const events::Request &request) {
  ++counts_.requests[std::string(OpName(request.op))];
}

void CostCounter::CheckRequestEnd(Context &ctx, const DirCacheFS &fs,
                                  const events::Request &request) {
  Write();
}

void CostCounter::CheckRunStarted(Context &ctx) { Write(); }

void CostCounter::Write() const {
  if (fd_ < 0) return;
  const std::string text = Text();
  // Best effort: a reader sees the last complete write.
  (void)syscalls::pwrite(fd_, text.data(), text.size(), 0);
}

std::string CostCounter::Text() const {
  std::string text = absl::StrCat(
      "steps ", counts_.steps, "\ntransactions ", counts_.transactions,
      "\ndurable_transactions ", counts_.durable_transactions,
      "\nbacking_calls ", counts_.backing_calls, "\n");
  for (const auto &[op, n] : counts_.requests) {
    absl::StrAppend(&text, "request.", op, " ", n, "\n");
  }
  return text;
}

}  // namespace dcfs::testonly
