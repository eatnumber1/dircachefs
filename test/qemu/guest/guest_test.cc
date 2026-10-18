// SPDX-License-Identifier: MIT
// The helpers of guest.h, in a guest, against a real dcfs mount.
#include "test/qemu/guest/guest.h"

#include <fcntl.h>
#include <sys/types.h>

#include <memory>
#include <string>
#include <vector>

#include "absl/status/status.h"
#include "absl/status/status_macros.h"
#include "absl/status/status_matchers.h"
#include "absl/status/statusor.h"
#include "absl/strings/match.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/strip.h"
#include "dcfs/syscalls.h"
#include "dcfs/syscalls_backing.h"
#include "dcfs/syscalls_process.h"
#include "dcfs/testonly/assert_ok_and_assign.h"
#include "dcfs/testonly/files.h"
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

// The guest's disk (TEST_TMPDIR), without the trailing slash.
std::string Tmp() {
  return std::string(absl::StripSuffix(::testing::TempDir(), "/"));
}

void Mkdir(const std::string &path) {
  ASSERT_THAT(syscalls::mkdir(path, 0755), IsOk());
}

// A source directory on the guest's disk, a cache database and a mount point
// (/mnt is guest/init's).
MountSpec NewSpec(const std::string &name) {
  MountSpec spec = {.source = Tmp() + "/" + name + "-src",
                    .mountpoint = "/mnt/" + name,
                    .cache_db = "/tmp/" + name + "-cache/db"};
  Mkdir(spec.source);
  Mkdir(spec.mountpoint);
  Mkdir("/tmp/" + name + "-cache");
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

TEST(InChildTest, AChildThatDiesBeforeReturningIsAnError) {
  EXPECT_THAT(
      InChild([]() -> absl::StatusOr<std::string> { syscalls::_exit(7); }),
      StatusIs(absl::StatusCode::kInternal,
               HasSubstr("exited with status 7 before")));
}

TEST(WriteFileTest, CreatesAndTruncates) {
  std::string path = Tmp() + "/written";
  ASSERT_THAT(WriteFile(path, "a longer text"), IsOk());
  ASSERT_THAT(WriteFile(path, "short"), IsOk());
  absl::StatusOr<struct stat> st = syscalls::stat(path);
  ASSERT_THAT(st, IsOk());
  EXPECT_EQ(st->st_size, 5);
}

TEST(ListNamesTest, NamesAreSortedBytesWithoutDots) {
  std::string dir = Tmp() + "/names";
  Mkdir(dir);
  ASSERT_THAT(WriteFile(dir + "/b", ""), IsOk());
  ASSERT_THAT(WriteFile(dir + "/a\nc", ""), IsOk());
  ASSERT_THAT(WriteFile(dir + "/\xff", ""), IsOk());
  EXPECT_THAT(testonly::ListDirectory(dir),
              IsOkAndHolds(ElementsAre("a\nc", "b", "\xff")));
}

TEST(ListNamesTest, AMissingDirectoryIsAnError) {
  EXPECT_THAT(testonly::ListDirectory(Tmp() + "/no-such"),
              StatusIs(absl::StatusCode::kNotFound));
}

TEST(SnapshotTest, OneLinePerEntryWithTheContentsOfFiles) {
  std::string dir = Tmp() + "/snap";
  Mkdir(dir);
  ASSERT_THAT(syscalls::mkdir(dir + "/d", 0700), IsOk());
  ASSERT_THAT(WriteFile(dir + "/d/f", "abc"), IsOk());
  ASSERT_THAT(syscalls::symlinkat("d/f", AT_FDCWD, dir + "/l"), IsOk());
  absl::StatusOr<std::vector<std::string>> lines = Snapshot(dir);
  ASSERT_THAT(lines, IsOk());
  ASSERT_EQ(lines->size(), 3u);
  EXPECT_EQ((*lines)[0], "./d d 4096 700 2");
  EXPECT_TRUE(absl::StartsWith((*lines)[1], "./d/f f 3 644 1 "));
  EXPECT_EQ((*lines)[2].substr(0, 5), "./l l");
}

class ExpectSameTreeTest : public ::testing::Test {
 protected:
  void SetUp() override {
    std::string test =
        ::testing::UnitTest::GetInstance()->current_test_info()->name();
    a_ = Tmp() + "/" + test + "-a";
    b_ = Tmp() + "/" + test + "-b";
    Mkdir(a_);
    Mkdir(b_);
    ASSERT_THAT(WriteFile(a_ + "/f", "one"), IsOk());
  }
  std::string a_, b_;
};

TEST_F(ExpectSameTreeTest, EqualTreesAreOk) {
  ASSERT_THAT(WriteFile(b_ + "/f", "one"), IsOk());
  EXPECT_THAT(ExpectSameTree(a_, b_), IsOk());
}

TEST_F(ExpectSameTreeTest, ADifferentByteFailsAndNamesTheSide) {
  ASSERT_THAT(WriteFile(b_ + "/f", "two"), IsOk());
  EXPECT_THAT(ExpectSameTree(a_, b_),
              StatusIs(absl::StatusCode::kInternal,
                       AllOf(HasSubstr("< ./f f 3"), HasSubstr("> ./f f 3"))));
}

TEST_F(ExpectSameTreeTest, AMissingFileFails) {
  EXPECT_THAT(ExpectSameTree(a_, b_),
              StatusIs(absl::StatusCode::kInternal, HasSubstr("< ./f f 3")));
}

TEST(ExpectSameAsBackingTest, EqualAnswersPass) {
  ExpectSameAsBacking("removed-cwd", "nlink=0", "nlink=0");
}

TEST(ExpectSameAsBackingTest, DifferentAnswersFailAndNameTheCheck) {
  EXPECT_NONFATAL_FAILURE(
      ExpectSameAsBacking("removed-cwd", "nlink=0", "ESTALE"),
      "removed-cwd: backing filesystem: 'nlink=0'; dcfs: 'ESTALE'");
}

TEST(DcfsMountTest, ServesTheBackingAndUnmountsCleanly) {
  MountSpec spec = NewSpec("serve");
  ASSERT_THAT(WriteFile(spec.source + "/f", "data"), IsOk());
  absl::StatusOr<std::unique_ptr<DcfsMount>> mount = DcfsMount::Start(spec);
  ASSERT_THAT(mount, IsOk());
  EXPECT_THAT(testonly::ListDirectory(spec.mountpoint),
              IsOkAndHolds(ElementsAre("f")));
  ASSERT_THAT(WriteFile(spec.mountpoint + "/g", "through"), IsOk());
  EXPECT_THAT(ExpectSameTree(spec.mountpoint, spec.source), IsOk());
  EXPECT_THAT((*mount)->Unmount(), IsOk());
  EXPECT_THAT(testonly::ListDirectory(spec.mountpoint),
              IsOkAndHolds(IsEmpty()));
}

TEST(DcfsMountTest, ACrashedDaemonLeavesTheBackingAndItCanBeMountedAgain) {
  MountSpec spec = NewSpec("crash");
  absl::StatusOr<std::unique_ptr<DcfsMount>> mount = DcfsMount::Start(spec);
  ASSERT_THAT(mount, IsOk());
  ASSERT_THAT(WriteFile(spec.mountpoint + "/f", "x"), IsOk());
  ASSERT_THAT((*mount)->Crash(), IsOk());
  EXPECT_THAT(testonly::ListDirectory(spec.mountpoint),
              IsOkAndHolds(IsEmpty()));
  absl::StatusOr<std::unique_ptr<DcfsMount>> again = DcfsMount::Start(spec);
  ASSERT_THAT(again, IsOk());
  EXPECT_THAT(testonly::ListDirectory(spec.mountpoint),
              IsOkAndHolds(ElementsAre("f")));
  EXPECT_THAT((*again)->Unmount(), IsOk());
}

TEST(DcfsMountTest, ADaemonThatExitsWithoutMountingIsReportedWithItsLog) {
  MountSpec spec = NewSpec("bad");
  spec.source += "-missing";
  EXPECT_THAT(DcfsMount::Start(spec), StatusIs(absl::StatusCode::kInternal,
                                               HasSubstr("without mounting")));
}

TEST(DcfsMountTest, TheErrorCarriesWhatTheDaemonWrote) {
  MountSpec spec = NewSpec("log");
  spec.flags = {"no_such_option"};
  EXPECT_THAT(DcfsMount::Start(spec), StatusIs(absl::StatusCode::kInternal,
                                               HasSubstr("no_such_option")));
}

TEST(DcfsMountTest, CrashOnUnstartedMountFails) {
  DcfsMount mount;
  EXPECT_THAT(mount.Crash(), StatusIs(absl::StatusCode::kInternal));
}

TEST(DcfsMountTest, UnmountOnUnstartedMountFails) {
  DcfsMount mount;
  EXPECT_THAT(mount.Unmount(), StatusIs(absl::StatusCode::kInternal));
}

}  // namespace
}  // namespace dcfs
