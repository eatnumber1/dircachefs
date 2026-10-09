#ifndef DCFS_SYSLOG_SINK_H_
#define DCFS_SYSLOG_SINK_H_

// Sends every message dcfs logs to syslog(3) (phase 15: the one log path of a
// daemonized dcfs, as for gocryptfs or ntfs-3g; under systemd the journal
// attributes each message to dcfs's mount unit). Register it with
// absl::AddLogSink after absl::InitializeLog.

#include "absl/base/log_severity.h"
#include "absl/log/log_entry.h"
#include "absl/log/log_sink.h"

namespace dcfs {

class SyslogSink final : public absl::LogSink {
 public:
  // Opens syslog under the identity "dcfs". Only messages at or above
  // `threshold` are sent: the daemon's one logging knob, the configured
  // stderrthreshold (WARNING by default), governs syslog.
  explicit SyslogSink(absl::LogSeverityAtLeast threshold);
  void Send(const absl::LogEntry &entry) override;

  // Whether a message of `severity` is sent at `threshold`.
  static bool Wants(absl::LogSeverityAtLeast threshold,
                    absl::LogSeverity severity);

 private:
  absl::LogSeverityAtLeast threshold_;
};

// The syslog(3) priority for an Abseil severity.
int SyslogPriority(absl::LogSeverity severity);

}  // namespace dcfs

#endif  // DCFS_SYSLOG_SINK_H_
