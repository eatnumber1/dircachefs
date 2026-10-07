#ifndef DCFS_TESTONLY_COST_COUNTER_H_
#define DCFS_TESTONLY_COST_COUNTER_H_

// The cost counters of the testonly observer (dcfs/protocol_events.h; step
// 26.4b): SQLite statement steps and transactions (from the cache
// database's Connection), FUSE requests by opcode (the invariant checks'
// request frames) and backing calls (their hooks). They replace the
// counting of sqlite.cc's VLOG(2) lines: no formatting, no log, no
// verbosity to raise.
//
// The forged-request harness reads them directly (counts()); the testonly
// checking daemon writes them, after every request, to $DCFS_COUNTERS_FILE
// when that is set (dcfs/testonly/main_invariant_checker.cc), for the guest
// scripts: guest/syscall_traces.sh's statement budgets, guest/
// request_counts.sh's request budgets.
//
// The invariant checker's own statements (InvariantChecker::kSqlMarker) are
// left out: what is counted is what the daemon itself costs.
//
// Not thread-safe: dcfs serves one request at a time.

#include <cstdint>
#include <map>
#include <string>
#include <string_view>

#include "dcfs/context.h"
#include "dcfs/protocol_events.h"

namespace dcfs::testonly {

class CostCounter final : public ProtocolEvents {
 public:
  struct Counts {
    int64_t steps = 0;                 // SQLite statement steps
    int64_t transactions = 0;          // outermost transactions begun
    int64_t durable_transactions = 0;  // ... of them committed with a WAL fsync
    int64_t backing_calls = 0;         // ProtocolEvents::BackingCall hooks
    std::map<std::string, int64_t> requests;  // FUSE requests, by opcode
  };

  // `fd` (not owned; -1 for none): where the counts are written after every
  // request and at Startup's end, as "<name> <value>" lines from offset 0 (the
  // counts only grow, so a write never leaves a longer earlier one's tail
  // behind).
  explicit CostCounter(int fd = -1) : fd_(fd) {}

  void SqliteStep(std::string_view sql) override;
  void SqliteTransaction(bool durable) override;
  void BackingCall(Context &ctx, std::string_view what,
                   absl::SourceLocation site) override;
  void CheckRequestBegin(Context &ctx, const DirCacheFS &fs,
                         const events::Request &request) override;
  void CheckRequestEnd(Context &ctx, const DirCacheFS &fs,
                       const events::Request &request) override;
  // Startup's end: the counts are written then too, so that the first
  // request's are not mixed with the start's.
  void CheckRunStarted(Context &ctx) override;

  const Counts &counts() const { return counts_; }
  void Reset() { counts_ = Counts(); }
  // The counts as the lines written to `fd`.
  std::string Text() const;

 private:
  void Write() const;

  int fd_;
  Counts counts_;
};

}  // namespace dcfs::testonly

#endif  // DCFS_TESTONLY_COST_COUNTER_H_
