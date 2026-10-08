#include "dcfs/syslog_sink.h"

#include <syslog.h>

#include "absl/base/log_severity.h"
#include "gtest/gtest.h"

namespace dcfs {
namespace {

TEST(SyslogPriorityTest, EachSeverityHasItsOwnPriority) {
  EXPECT_EQ(SyslogPriority(absl::LogSeverity::kInfo), LOG_INFO);
  EXPECT_EQ(SyslogPriority(absl::LogSeverity::kWarning), LOG_WARNING);
  EXPECT_EQ(SyslogPriority(absl::LogSeverity::kError), LOG_ERR);
  EXPECT_EQ(SyslogPriority(absl::LogSeverity::kFatal), LOG_CRIT);
}

}  // namespace
}  // namespace dcfs
