// SPDX-License-Identifier: MIT
#include "dcfs/testonly/files.h"

#include <fcntl.h>
#include <sys/socket.h>

#include <string>
#include <string_view>

#include "absl/status/status.h"
#include "absl/status/status_matchers.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/strip.h"
#include "dcfs/syscalls.h"
#include "dcfs/syscalls_backing.h"
#include "dcfs/testonly/assert_ok_and_assign.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"

namespace dcfs {
namespace {

using ::absl_testing::IsOk;
using ::absl_testing::IsOkAndHolds;
using ::absl_testing::StatusIs;
using ::testing::ElementsAre;

// A directory of its own on the guest's disk (TEST_TMPDIR).
std::string NewDirectory(std::string_view name) {
  std::string dir = absl::StrCat(
      absl::StripSuffix(::testing::TempDir(), "/"), "/", name);
  EXPECT_THAT(syscalls::mkdir(dir, 0755), IsOk());
  return dir;
}

void Touch(const std::string &path) {
  ASSERT_THAT(syscalls::open(path, O_WRONLY | O_CREAT, 0644), IsOk());
}

TEST(ListDirectoryTest, NamesAreSortedBytesWithoutDots) {
  std::string dir = NewDirectory("names");
  Touch(dir + "/b");
  Touch(dir + "/a\nc");
  Touch(dir + "/\xff");
  EXPECT_THAT(testonly::ListDirectory(dir),
              IsOkAndHolds(ElementsAre("a\nc", "b", "\xff")));
}

TEST(ListDirectoryTest, AMissingDirectoryIsNotFound) {
  EXPECT_THAT(testonly::ListDirectory(absl::StrCat(::testing::TempDir(), "/x")),
              StatusIs(absl::StatusCode::kNotFound));
}

TEST(ReadAllTest, ReadsEverythingUpToTheEnd) {
  std::string contents(10000, 'x');
  ASSERT_OK_AND_ASSIGN(auto pair,
                       syscalls::socketpair(AF_UNIX, SOCK_STREAM, 0));
  ASSERT_THAT(syscalls::send(*pair.first, contents.data(), 5000, 0), IsOk());
  ASSERT_THAT(syscalls::send(*pair.first, contents.data(), 5000, 0), IsOk());
  ASSERT_THAT(pair.first.Close(), IsOk());
  EXPECT_THAT(testonly::ReadAll(*pair.second), IsOkAndHolds(contents));
}

}  // namespace
}  // namespace dcfs
