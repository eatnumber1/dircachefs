// SPDX-License-Identifier: MIT
#include "dcfs/proc_file.h"

#include <fcntl.h>

#include <cerrno>
#include <cstddef>
#include <string>

#include "absl/status/status_matchers.h"
#include "absl/status/statusor.h"
#include "absl/strings/str_cat.h"
#include "dcfs/fd.h"
#include "dcfs/status.h"
#include "dcfs/syscalls.h"
#include "dcfs/syscalls_backing.h"
#include "dcfs/testonly/assert_ok_and_assign.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"

namespace dcfs {
namespace {

using ::absl_testing::IsOkAndHolds;

TEST(ProcFileTest, AFileThatCannotBeOpenedIsItsOpensError) {
  absl::StatusOr<std::string> read = ReadProcFile("/proc/self/no-such-file");
  EXPECT_EQ(StatusToErrno(read.status()), ENOENT) << read.status();
}

// A file longer than the read buffer (64 KiB) comes back whole, in order.
// Nothing in procfs is that long and fixed, so a file of the test's own
// stands in: ReadProcFile reads any file to its end.
TEST(ProcFileTest, AFileLongerThanTheBufferIsReadWhole) {
  std::string contents;
  for (int i = 0; contents.size() < 200000; ++i) {
    absl::StrAppend(&contents, i, "\n");
  }
  std::string path = absl::StrCat(::testing::TempDir(), "long");
  ASSERT_OK_AND_ASSIGN(FileDescriptor fd,
                       syscalls::open(path, O_WRONLY | O_CREAT, 0600));
  ASSERT_OK_AND_ASSIGN(size_t written,
                       syscalls::write(*fd, contents.data(), contents.size()));
  ASSERT_EQ(written, contents.size());
  EXPECT_THAT(ReadProcFile(path), IsOkAndHolds(contents));
}

}  // namespace
}  // namespace dcfs
