// SPDX-License-Identifier: MIT
// Tests of the runner that starts child processes, over the programs the
// guest has (busybox's).
#include "tools/process_runner.h"

#include <fcntl.h>
#include <sys/types.h>

#include <string>
#include <vector>

#include "absl/status/status.h"
#include "absl/status/status_matchers.h"
#include "absl/status/statusor.h"
#include "dcfs/fd.h"
#include "dcfs/syscalls.h"
#include "dcfs/syscalls_backing.h"
#include "dcfs/testonly/assert_ok_and_assign.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "tools/test_first.h"

namespace dcfs::test_first {
namespace {

using ::absl_testing::IsOk;
using ::absl_testing::StatusIs;
using ::testing::HasSubstr;

absl::StatusOr<CommandResult> RunSh(const std::string &script,
                                    const std::string &path = "/bin") {
  ProcessRunner runner(path);
  const std::vector<std::string> argv = {"sh", "-c", script};
  return runner.Run(argv);
}

TEST(ProcessRunnerTest, FindsTheProgramOnThePath) {
  ProcessRunner runner("/nonexistent:/bin");
  const std::vector<std::string> argv = {"echo", "hi"};
  ASSERT_OK_AND_ASSIGN(CommandResult result, runner.Run(argv));
  EXPECT_EQ(result.exit_code, 0);
  EXPECT_EQ(result.output, "hi\n");
}

TEST(ProcessRunnerTest, AnAbsolutePathIsNotSearched) {
  ProcessRunner runner("/nonexistent");
  const std::vector<std::string> argv = {"/bin/echo", "direct"};
  ASSERT_OK_AND_ASSIGN(CommandResult result, runner.Run(argv));
  EXPECT_EQ(result.output, "direct\n");
}

TEST(ProcessRunnerTest, ExitCodeAndBothStreams) {
  ASSERT_OK_AND_ASSIGN(CommandResult result,
                       RunSh("echo out; echo err >&2; exit 3"));
  EXPECT_EQ(result.exit_code, 3);
  EXPECT_EQ(result.output, "out\nerr\n");
}

TEST(ProcessRunnerTest, ADeathBySignalIsExitCode128PlusTheSignal) {
  ASSERT_OK_AND_ASSIGN(CommandResult result, RunSh("kill -9 $$"));
  EXPECT_EQ(result.exit_code, 128 + 9);
}

TEST(ProcessRunnerTest, OutputLargerThanAPipeBufferIsAllRead) {
  ASSERT_OK_AND_ASSIGN(CommandResult result,
                       RunSh("head -c 300000 /dev/zero | tr '\\000' x"));
  EXPECT_EQ(result.output.size(), 300000u);
}

TEST(ProcessRunnerTest, AProgramNotOnThePathIsNotFound) {
  ProcessRunner runner("/bin");
  const std::vector<std::string> argv = {"no-such-program-here"};
  EXPECT_THAT(runner.Run(argv), StatusIs(absl::StatusCode::kNotFound,
                                         HasSubstr("no-such-program-here")));
}

TEST(ProcessRunnerTest, AProgramThatCannotBeExecutedExits127) {
  ProcessRunner runner("/bin");
  const std::vector<std::string> argv = {"/etc/passwd"};
  ASSERT_OK_AND_ASSIGN(CommandResult result, runner.Run(argv));
  EXPECT_EQ(result.exit_code, 127);
}

// A directory of /tmp holding what a test makes.
class ResolveTest : public ::testing::Test {
 protected:
  void SetUp() override {
    ASSERT_OK_AND_ASSIGN(dir_, syscalls::mkdtemp("/tmp/resolve.XXXXXX"));
  }
  void TearDown() override { syscalls::chdir("/").IgnoreError(); }

  // Makes `dir_/name` a file with `mode`.
  void MakeFile(const std::string &name, mode_t mode) {
    ASSERT_OK_AND_ASSIGN(FileDescriptor fd,
                         syscalls::openat(AT_FDCWD, dir_ + "/" + name,
                                          O_CREAT | O_WRONLY, mode));
    ASSERT_THAT(syscalls::fchmod(*fd, mode), IsOk());
  }

  std::string dir_;
};

TEST_F(ResolveTest, AnEmptyComponentOfThePathIsTheCurrentDirectory) {
  MakeFile("prog", 0755);
  ASSERT_THAT(syscalls::chdir(dir_), IsOk());
  ASSERT_OK_AND_ASSIGN(std::string found,
                       Resolve("prog", "/nonexistent::/bin"));
  EXPECT_EQ(found, "./prog");
}

TEST_F(ResolveTest, ANameWithASlashIsReturnedAsGiven) {
  ASSERT_OK_AND_ASSIGN(std::string found, Resolve("./x", "/nonexistent"));
  EXPECT_EQ(found, "./x");
}

TEST_F(ResolveTest, AFileThatIsNotExecutableIsSkipped) {
  ASSERT_THAT(syscalls::mkdirat(AT_FDCWD, dir_ + "/a", 0755), IsOk());
  ASSERT_THAT(syscalls::mkdirat(AT_FDCWD, dir_ + "/b", 0755), IsOk());
  MakeFile("a/prog", 0644);
  MakeFile("b/prog", 0755);
  ASSERT_OK_AND_ASSIGN(std::string found,
                       Resolve("prog", dir_ + "/a:" + dir_ + "/b"));
  EXPECT_EQ(found, dir_ + "/b/prog");
}

TEST_F(ResolveTest, ADirectoryOfThatNameIsSkipped) {
  ASSERT_THAT(syscalls::mkdirat(AT_FDCWD, dir_ + "/a", 0755), IsOk());
  ASSERT_THAT(syscalls::mkdirat(AT_FDCWD, dir_ + "/a/prog", 0755), IsOk());
  ASSERT_THAT(syscalls::mkdirat(AT_FDCWD, dir_ + "/b", 0755), IsOk());
  MakeFile("b/prog", 0755);
  ASSERT_OK_AND_ASSIGN(std::string found,
                       Resolve("prog", dir_ + "/a:" + dir_ + "/b"));
  EXPECT_EQ(found, dir_ + "/b/prog");
}

TEST_F(ResolveTest, ADirectoryThatDoesNotExistIsSkipped) {
  ASSERT_OK_AND_ASSIGN(std::string found, Resolve("echo", "/nonexistent:/bin"));
  EXPECT_EQ(found, "/bin/echo");
}

}  // namespace
}  // namespace dcfs::test_first
