#ifndef DCFS_TESTONLY_STEP_COUNTER_H_
#define DCFS_TESTONLY_STEP_COUNTER_H_

#include <cstdint>
#include <string_view>

#include "absl/log/globals.h"
#include "absl/log/log_entry.h"
#include "absl/log/log_sink.h"
#include "absl/log/log_sink_registry.h"
#include "absl/strings/match.h"

namespace dcfs::testonly {

// Counts the SQLite statement steps (sqlite3::Statement::Step's VLOG(2)
// lines, the same ones guest/syscall_traces.sh budgets) made while it lives,
// in this process. A step that returns a row and the step that returns DONE
// each count: a lookup that stops at its first row (ReadOne) is one, a miss
// one, a query read to its end one more than its rows.
//
// Raises the verbosity of sqlite.cc alone (not the global level) and keeps
// the lines off stderr while counting. Precondition: absl::InitializeLog()
// has run in this process (before it every message goes to stderr whatever
// the threshold, about 30 ms a line on the guest's serial console): the
// test binary calls it from a namespace-scope initializer, so the whole run
// is initialized the same way.
class SqliteStepCounter : public absl::LogSink {
 public:
  SqliteStepCounter()
      : saved_vlog_(absl::SetVLogLevel("sqlite", 2)),
        saved_stderr_(absl::StderrThreshold()) {
    absl::SetStderrThreshold(absl::LogSeverityAtLeast::kInfinity);
    absl::AddLogSink(this);
  }
  ~SqliteStepCounter() override {
    absl::RemoveLogSink(this);
    absl::SetStderrThreshold(saved_stderr_);
    absl::SetVLogLevel("sqlite", saved_vlog_);
  }

  void Send(const absl::LogEntry &entry) override {
    if (absl::StartsWith(entry.text_message(), "sqlite3_step:")) ++steps_;
  }

  int64_t steps() const { return steps_; }
  void Reset() { steps_ = 0; }

 private:
  int saved_vlog_;
  absl::LogSeverityAtLeast saved_stderr_;
  int64_t steps_ = 0;
};

}  // namespace dcfs::testonly

#endif  // DCFS_TESTONLY_STEP_COUNTER_H_
