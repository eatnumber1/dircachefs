#include "dcfs/syslog_sink.h"

#include <syslog.h>

#include "absl/base/log_severity.h"
#include "absl/log/log_entry.h"
#include "dcfs/syscalls.h"

namespace dcfs {

SyslogSink::SyslogSink(absl::LogSeverityAtLeast threshold)
    : threshold_(threshold) {
  syscalls::openlog("dcfs", LOG_PID, LOG_DAEMON);
}

bool SyslogSink::Wants(absl::LogSeverityAtLeast threshold,
                       absl::LogSeverity severity) {
  return static_cast<int>(severity) >= static_cast<int>(threshold);
}

void SyslogSink::Send(const absl::LogEntry &entry) {
  if (!Wants(threshold_, entry.log_severity())) return;
  syscalls::syslog(SyslogPriority(entry.log_severity()),
                   entry.text_message());
}

int SyslogPriority(absl::LogSeverity severity) {
  switch (severity) {
    case absl::LogSeverity::kInfo:
      return LOG_INFO;
    case absl::LogSeverity::kWarning:
      return LOG_WARNING;
    case absl::LogSeverity::kError:
      return LOG_ERR;
    case absl::LogSeverity::kFatal:
      return LOG_CRIT;
  }
  return LOG_ERR;
}

}  // namespace dcfs
