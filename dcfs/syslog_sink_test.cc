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

// The one knob (dcfs.stderrthreshold) decides what reaches syslog.
TEST(SyslogThresholdTest, OnlyMessagesAtOrAboveTheThresholdAreSent) {
  EXPECT_FALSE(SyslogSink::Wants(absl::LogSeverityAtLeast::kWarning,
                                 absl::LogSeverity::kInfo));
  EXPECT_TRUE(SyslogSink::Wants(absl::LogSeverityAtLeast::kWarning,
                                absl::LogSeverity::kWarning));
  EXPECT_TRUE(SyslogSink::Wants(absl::LogSeverityAtLeast::kWarning,
                                absl::LogSeverity::kError));
  EXPECT_TRUE(SyslogSink::Wants(absl::LogSeverityAtLeast::kInfo,
                                absl::LogSeverity::kInfo));
  EXPECT_FALSE(SyslogSink::Wants(absl::LogSeverityAtLeast::kError,
                                 absl::LogSeverity::kWarning));
  EXPECT_FALSE(SyslogSink::Wants(absl::LogSeverityAtLeast::kInfinity,
                                 absl::LogSeverity::kFatal));
}

}  // namespace
}  // namespace dcfs
