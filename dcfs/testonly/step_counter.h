#ifndef DCFS_TESTONLY_STEP_COUNTER_H_
#define DCFS_TESTONLY_STEP_COUNTER_H_

#include <cstdint>
#include <string_view>

#include "absl/log/globals.h"
#include "absl/log/initialize.h"
#include "absl/log/log_entry.h"
#include "absl/log/log_sink.h"
#include "absl/log/log_sink_registry.h"
#include "absl/strings/match.h"

namespace dcfs::testonly {

// Counts the SQLite statement steps (sqlite3::Statement::Step's VLOG(2)
// lines, the same ones guest/syscall_traces.sh budgets) made while it
// lives, in this process. A step that returns a row and the step that
// returns DONE each count: a primary-key hit is two, a miss one.
class SqliteStepCounter : public absl::LogSink {
 public:
  // Raises the verbosity so the step lines exist, and keeps them off stderr
  // (the guest's serial console takes about 30 ms a line) while counting.
  // Before absl::InitializeLog() every message goes to stderr whatever the
  // threshold, so this initializes logging once (a test binary that already
  // did must not, hence the check of a previous call by the caller's main:
  // none of ours does).
  SqliteStepCounter() {
    static const bool initialized = (absl::InitializeLog(), true);
    (void)initialized;
    saved_vlog_ = absl::SetGlobalVLogLevel(2);
    saved_stderr_ = absl::StderrThreshold();
    absl::SetStderrThreshold(absl::LogSeverityAtLeast::kInfinity);
    absl::AddLogSink(this);
  }
  ~SqliteStepCounter() override {
    absl::RemoveLogSink(this);
    absl::SetStderrThreshold(saved_stderr_);
    absl::SetGlobalVLogLevel(saved_vlog_);
  }

  void Send(const absl::LogEntry &entry) override {
    if (absl::StartsWith(entry.text_message(), "sqlite3_step:")) ++steps_;
  }

  int64_t steps() const { return steps_; }
  void Reset() { steps_ = 0; }

 private:
  int saved_vlog_ = 0;
  absl::LogSeverityAtLeast saved_stderr_ = absl::LogSeverityAtLeast::kInfo;
  int64_t steps_ = 0;
};

}  // namespace dcfs::testonly

#endif  // DCFS_TESTONLY_STEP_COUNTER_H_
