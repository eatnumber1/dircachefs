// SPDX-License-Identifier: MIT
// The helpers of guest.h, in a guest, against a real dcfs mount.
#include "test/qemu/guest/guest.h"

#include <fcntl.h>
#include <sys/types.h>

#include <cerrno>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "absl/base/log_severity.h"
#include "absl/log/scoped_mock_log.h"
#include "absl/status/status.h"
#include "absl/status/status_macros.h"
#include "absl/status/status_matchers.h"
#include "absl/status/statusor.h"
#include "absl/strings/match.h"
#include "absl/strings/str_cat.h"
#include "dcfs/status.h"
#include "dcfs/syscalls.h"
#include "dcfs/syscalls_backing.h"
#include "dcfs/syscalls_process.h"
#include "dcfs/testonly/assert_ok_and_assign.h"
#include "dcfs/testonly/files.h"
#include "dcfs/testonly/process.h"
#include "gmock/gmock.h"
#include "gtest/gtest-spi.h"
#include "gtest/gtest.h"

namespace dcfs {
namespace {

using ::absl_testing::IsOk;
using ::absl_testing::IsOkAndHolds;
using ::absl_testing::StatusIs;
using ::testing::AllOf;
using ::testing::ElementsAre;
using ::testing::HasSubstr;
using ::testing::IsEmpty;
using ::testing::StartsWith;

// A source directory on the guest's disk, a cache database and a mount point
// (/mnt is guest/init's).
absl::StatusOr<MountSpec> NewSpec(std::string_view name) {
  MountSpec spec = {.source = absl::StrCat(TestTmp(), "/", name, "-src"),
                    .mountpoint = absl::StrCat("/mnt/", name),
                    .cache_db = absl::StrCat("/tmp/", name, "-cache/db")};
  RETURN_IF_ERROR(Mkdir(spec.source));
  RETURN_IF_ERROR(Mkdir(spec.mountpoint));
  RETURN_IF_ERROR(Mkdir(absl::StrCat("/tmp/", name, "-cache")));
  return spec;
}

TEST(InChildTest, ReturnsWhatTheFunctionReturned) {
  absl::StatusOr<std::string> got =
      InChild([]() -> absl::StatusOr<std::string> { return "hello"; });
  ASSERT_THAT(got, IsOk());
  EXPECT_EQ(*got, "hello");
}

TEST(InChildTest, WhatTheChildDoesStaysInTheChild) {
  ASSERT_OK_AND_ASSIGN(std::string child_cwd,
                       InChild([]() -> absl::StatusOr<std::string> {
                         RETURN_IF_ERROR(syscalls::chdir("/proc"));
                         return syscalls::getcwd();
                       }));
  EXPECT_EQ(child_cwd, "/proc");
  ASSERT_OK_AND_ASSIGN(std::string parent_cwd, syscalls::getcwd());
  EXPECT_NE(parent_cwd, "/proc");
}

TEST(InChildTest, RunsAsTheGivenUserAndGroups) {
  absl::StatusOr<std::string> got = InChild(
      []() -> absl::StatusOr<std::string> {
        gid_t groups[16];
        ASSIGN_OR_RETURN(int n, syscalls::getgroups(16, groups));
        std::string grp_str;
        for (int i = 0; i < n; ++i) {
          absl::StrAppend(&grp_str, i == 0 ? "" : ",", groups[i]);
        }
        return absl::StrCat(syscalls::getuid(), ":", syscalls::geteuid(), ":",
                            syscalls::getgid(), ":", syscalls::getegid(), ":",
                            grp_str);
      },
      Identity{.uid = 1000, .gid = 2000, .groups = {3000}});
  ASSERT_THAT(got, IsOk());
  EXPECT_EQ(*got, "1000:1000:2000:2000:3000");
}

TEST(InChildTest, PropagatesCallbackError) {
  EXPECT_THAT(
      InChild([]() -> absl::StatusOr<std::string> {
        return absl::PermissionDeniedError("denied");
      }),
      StatusIs(absl::StatusCode::kPermissionDenied, HasSubstr("denied")));
}

TEST(InChildTest, AnErrnoStatusArrivesWithItsMessageAndItsErrno) {
  absl::StatusOr<std::string> got =
      InChild([]() -> absl::StatusOr<std::string> {
        return ErrnoToStatus(ENOENT, "open x");
      });
  EXPECT_THAT(got, StatusIs(absl::StatusCode::kNotFound,
                            "open x: No such file or directory"));
  EXPECT_EQ(StatusToErrno(got.status()), ENOENT);
}

TEST(InChildTest, ACallerThatCannotTakeRootBackCannotChangeUser) {
  absl::StatusOr<std::string> got = InChild(
      []() -> absl::StatusOr<std::string> {
        return InChild([]() -> absl::StatusOr<std::string> { return "x"; },
                       Identity{.uid = 2000, .gid = 2000, .groups = {}});
      },
      Identity{.uid = 1000, .gid = 1000, .groups = {}});
  EXPECT_THAT(got, StatusIs(absl::StatusCode::kPermissionDenied,
                            AllOf(HasSubstr("could not become the user"),
                                  HasSubstr("setresgid(2000"))));
  EXPECT_EQ(StatusToErrno(got.status()), EPERM);
}

TEST(InChildTest, AChildThatExitsWithoutAnAnswerIsAnError) {
  EXPECT_THAT(
      InChild([]() -> absl::StatusOr<std::string> { syscalls::_exit(0); }),
      StatusIs(absl::StatusCode::kInternal,
               HasSubstr("without producing an answer")));
}

TEST(InChildTest, AChildThatDiesBeforeReturningIsAnError) {
  EXPECT_THAT(
      InChild([]() -> absl::StatusOr<std::string> { syscalls::_exit(7); }),
      StatusIs(absl::StatusCode::kInternal,
               HasSubstr("exited with status 7 before")));
}

TEST(WriteFileTest, CreatesAndTruncates) {
  std::string path = absl::StrCat(TestTmp(), "/written");
  ASSERT_THAT(WriteFile(path, "a longer text"), IsOk());
  ASSERT_THAT(WriteFile(path, "short"), IsOk());
  absl::StatusOr<struct stat> st = syscalls::stat(path);
  ASSERT_THAT(st, IsOk());
  EXPECT_EQ(st->st_size, 5);
}

TEST(SnapshotTest, OneLinePerEntryWithTheContentsOfFiles) {
  std::string dir = absl::StrCat(TestTmp(), "/snap");
  ASSERT_THAT(Mkdir(dir), IsOk());
  ASSERT_THAT(syscalls::mkdir(absl::StrCat(dir, "/d"), 0700), IsOk());
  ASSERT_THAT(WriteFile(absl::StrCat(dir, "/d/f"), "abc"), IsOk());
  ASSERT_THAT(syscalls::symlinkat("d/f", AT_FDCWD, absl::StrCat(dir, "/l")),
              IsOk());
  EXPECT_THAT(Snapshot(dir),
              IsOkAndHolds(ElementsAre("./d d 4096 700 2",
                                       StartsWith("./d/f f 3 644 1 "),
                                       StartsWith("./l l"))));
}

class CompareTreesTest : public ::testing::Test {
 protected:
  void SetUp() override {
    std::string test =
        ::testing::UnitTest::GetInstance()->current_test_info()->name();
    a_ = absl::StrCat(TestTmp(), "/", test, "-a");
    b_ = absl::StrCat(TestTmp(), "/", test, "-b");
    ASSERT_THAT(Mkdir(a_), IsOk());
    ASSERT_THAT(Mkdir(b_), IsOk());
    ASSERT_THAT(WriteFile(absl::StrCat(a_, "/f"), "one"), IsOk());
  }
  std::string a_, b_;
};

TEST_F(CompareTreesTest, EqualTreesAreOk) {
  ASSERT_THAT(WriteFile(absl::StrCat(b_, "/f"), "one"), IsOk());
  EXPECT_THAT(CompareTrees(a_, b_), IsOk());
}

TEST_F(CompareTreesTest, ADifferentByteFailsAndNamesTheSide) {
  ASSERT_THAT(WriteFile(absl::StrCat(b_, "/f"), "two"), IsOk());
  EXPECT_THAT(CompareTrees(a_, b_),
              StatusIs(absl::StatusCode::kInternal,
                       AllOf(HasSubstr("< ./f f 3"), HasSubstr("> ./f f 3"))));
}

TEST_F(CompareTreesTest, AMissingFileFails) {
  EXPECT_THAT(CompareTrees(a_, b_),
              StatusIs(absl::StatusCode::kInternal, HasSubstr("< ./f f 3")));
}

TEST(ExpectSameAsBackingTest, EqualAnswersPass) {
  ExpectSameAsBacking("nlink=0", "nlink=0");
}

TEST(ExpectSameAsBackingTest, DifferentAnswersFailAndShowBoth) {
  EXPECT_NONFATAL_FAILURE(ExpectSameAsBacking("nlink=0", "ESTALE"),
                          "backing filesystem: 'nlink=0'; dcfs: 'ESTALE'");
}

TEST(DcfsMountTest, ServesTheBackingAndUnmountsCleanly) {
  ASSERT_OK_AND_ASSIGN(MountSpec spec, NewSpec("serve"));
  ASSERT_THAT(WriteFile(absl::StrCat(spec.source, "/f"), "data"), IsOk());
  absl::StatusOr<std::unique_ptr<DcfsMount>> mount = DcfsMount::Start(spec);
  ASSERT_THAT(mount, IsOk());
  EXPECT_THAT(testonly::ListDirectory(spec.mountpoint),
              IsOkAndHolds(ElementsAre("f")));
  ASSERT_THAT(WriteFile(absl::StrCat(spec.mountpoint, "/g"), "through"),
              IsOk());
  EXPECT_THAT(CompareTrees(spec.mountpoint, spec.source), IsOk());
  EXPECT_THAT((*mount)->Unmount(), IsOk());
  EXPECT_THAT(testonly::ListDirectory(spec.mountpoint),
              IsOkAndHolds(IsEmpty()));
}

TEST(DcfsMountTest, ACrashedDaemonLeavesTheBackingAndItCanBeMountedAgain) {
  ASSERT_OK_AND_ASSIGN(MountSpec spec, NewSpec("crash"));
  absl::StatusOr<std::unique_ptr<DcfsMount>> mount = DcfsMount::Start(spec);
  ASSERT_THAT(mount, IsOk());
  ASSERT_THAT(WriteFile(absl::StrCat(spec.mountpoint, "/f"), "x"), IsOk());
  ASSERT_THAT((*mount)->Crash(), IsOk());
  EXPECT_THAT(testonly::ListDirectory(spec.mountpoint),
              IsOkAndHolds(IsEmpty()));
  absl::StatusOr<std::unique_ptr<DcfsMount>> again = DcfsMount::Start(spec);
  ASSERT_THAT(again, IsOk());
  EXPECT_THAT(testonly::ListDirectory(spec.mountpoint),
              IsOkAndHolds(ElementsAre("f")));
  EXPECT_THAT((*again)->Unmount(), IsOk());
}

TEST(DcfsMountTest, ADaemonThatExitsWithoutMountingIsReported) {
  ASSERT_OK_AND_ASSIGN(MountSpec spec, NewSpec("bad"));
  absl::StrAppend(&spec.source, "-missing");
  EXPECT_THAT(DcfsMount::Start(spec), StatusIs(absl::StatusCode::kInternal,
                                               HasSubstr("without mounting")));
}

TEST(DcfsMountTest, TheErrorCarriesWhatTheDaemonWrote) {
  ASSERT_OK_AND_ASSIGN(MountSpec spec, NewSpec("log"));
  spec.flags = {"no_such_option"};
  EXPECT_THAT(DcfsMount::Start(spec), StatusIs(absl::StatusCode::kInternal,
                                               HasSubstr("no_such_option")));
}

TEST(DcfsMountTest, AMountThatIsGoneCannotBeUnmountedOrCrashedAgain) {
  ASSERT_OK_AND_ASSIGN(MountSpec spec, NewSpec("twice"));
  absl::StatusOr<std::unique_ptr<DcfsMount>> mount = DcfsMount::Start(spec);
  ASSERT_THAT(mount, IsOk());
  ASSERT_THAT((*mount)->Unmount(), IsOk());
  EXPECT_THAT((*mount)->Unmount(),
              StatusIs(absl::StatusCode::kFailedPrecondition,
                       HasSubstr("already unmounted or crashed")));
  EXPECT_THAT((*mount)->Crash(),
              StatusIs(absl::StatusCode::kFailedPrecondition,
                       HasSubstr("already unmounted or crashed")));
}

TEST(DcfsMountTest, ADaemonKilledAtUnmountIsReported) {
  ASSERT_OK_AND_ASSIGN(MountSpec spec, NewSpec("killed"));
  absl::StatusOr<std::unique_ptr<DcfsMount>> mount = DcfsMount::Start(spec);
  ASSERT_THAT(mount, IsOk());
  ASSERT_THAT(testonly::RunCommand({"/bin/busybox", "killall", "-9", "dcfs"}),
              IsOk());
  EXPECT_THAT((*mount)->Unmount(),
              StatusIs(absl::StatusCode::kInternal,
                       HasSubstr("was killed by signal 9 at unmount")));
}

TEST(DcfsMountTest, ALogThatCannotBeReadIsSaidSoInTheError) {
  ASSERT_OK_AND_ASSIGN(MountSpec spec, NewSpec("nolog"));
  absl::StatusOr<std::unique_ptr<DcfsMount>> mount = DcfsMount::Start(spec);
  ASSERT_THAT(mount, IsOk());
  // The only log in /tmp is this mount's: the others are gone with theirs.
  ASSERT_OK_AND_ASSIGN(std::vector<std::string> names,
                       testonly::ListDirectory("/tmp"));
  for (const std::string &name : names) {
    if (absl::StartsWith(name, "dcfs-")) {
      ASSERT_THAT(syscalls::unlink(absl::StrCat("/tmp/", name)), IsOk());
    }
  }
  ASSERT_THAT(testonly::RunCommand({"/bin/busybox", "killall", "-9", "dcfs"}),
              IsOk());
  EXPECT_THAT((*mount)->Unmount(),
              StatusIs(absl::StatusCode::kInternal,
                       AllOf(HasSubstr("was killed by signal 9 at unmount"),
                             HasSubstr("its stderr: unreadable ("))));
}

TEST(DcfsMountTest, ADestroyedMountThatCannotBeCleanedUpSaysSo) {
  ASSERT_OK_AND_ASSIGN(MountSpec spec, NewSpec("leftover"));
  absl::StatusOr<std::unique_ptr<DcfsMount>> mount = DcfsMount::Start(spec);
  ASSERT_THAT(mount, IsOk());
  // The daemon exits by itself; the destructor's lazy umount then finds
  // nothing mounted.
  ASSERT_THAT(testonly::RunCommand({"/bin/umount", spec.mountpoint}), IsOk());
  absl::ScopedMockLog log(absl::MockLogDefault::kIgnoreUnexpected);
  EXPECT_CALL(log, Log(absl::LogSeverity::kError, testing::_,
                       AllOf(HasSubstr("Cleaning up the dcfs mount on"),
                             HasSubstr("/bin/umount -l"))));
  log.StartCapturingLogs();
  mount->reset();
}

}  // namespace
}  // namespace dcfs
