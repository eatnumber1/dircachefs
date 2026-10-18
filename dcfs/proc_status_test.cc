// SPDX-License-Identifier: MIT
#include "dcfs/proc_status.h"

#include <linux/capability.h>
#include <sys/types.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "absl/status/status_matchers.h"
#include "absl/strings/str_cat.h"
#include "dcfs/syscalls.h"
#include "dcfs/testonly/assert_ok_and_assign.h"
#include "dcfs/testonly/thread_without_cap_fsetid.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"

namespace dcfs {
namespace {

using ::absl_testing::IsOk;
using ::absl_testing::IsOkAndHolds;
using ::testing::ElementsAre;
using ::testing::IsEmpty;
using ::testing::Not;
using ::testing::UnorderedElementsAreArray;

constexpr uint64_t kFsetid = uint64_t{1} << CAP_FSETID;

// The lines of a status file that matter, as the kernel prints them, among
// lines that do not.
constexpr char kStatus[] =
    "Name:\tsh\n"
    "Uid:\t1000\t1001\t1002\t1003\n"
    "Gid:\t2000\t2001\t2002\t2003\n"
    "FDSize:\t64\n"
    "Groups:\t10 2000 \n"
    "CapInh:\t0000000000000000\n"
    "CapPrm:\t000001ffffffffff\n"
    "CapEff:\t0000000000000010\n";

TEST(ProcStatusTest, ParsesTheFilesystemUidGroupsAndEffectiveCapabilities) {
  ASSERT_OK_AND_ASSIGN(ProcCredentials parsed, ParseProcStatus(kStatus));
  EXPECT_EQ(parsed.fsuid, 1003u);
  EXPECT_THAT(parsed.groups, ElementsAre(10u, 2000u));
  EXPECT_EQ(parsed.cap_effective, kFsetid);
}

TEST(ProcStatusTest, AnEmptyGroupsLineIsNoGroups) {
  ASSERT_OK_AND_ASSIGN(ProcCredentials parsed,
                       ParseProcStatus("Uid:\t0\t0\t0\t0\nGroups:\t\n"
                                       "CapEff:\t000001ffffffffff\n"));
  EXPECT_THAT(parsed.groups, IsEmpty());
  EXPECT_EQ(parsed.cap_effective, uint64_t{0x000001ffffffffff});
}

// Whatever the kernel would never write is refused, each line on its own.
TEST(ProcStatusTest, WhatIsNotAStatusFileIsRefused) {
  const std::string uid = "Uid:\t0\t0\t0\t0\n";
  const std::string groups = "Groups:\t0\n";
  const std::string cap = "CapEff:\t0\n";
  for (const std::string &status : {
           absl::StrCat(groups, cap),                     // no Uid
           absl::StrCat(uid, cap),                        // no Groups
           absl::StrCat(uid, groups),                     // no CapEff
           absl::StrCat("Uid:\t0\t0\t0\n", groups, cap),  // three uids
           absl::StrCat(uid, "Groups:\twheel\n", cap),    // not a gid
           absl::StrCat(uid, groups, "CapEff:\tzz\n"),    // not hex
       }) {
    EXPECT_THAT(ParseProcStatus(status), Not(IsOk())) << status;
  }
}

// The test runs as the guest's root, in the init user namespace.
TEST(ProcStatusTest, ReadsThisProcess) {
  std::vector<gid_t> groups(64);
  ASSERT_OK_AND_ASSIGN(
      int n,
      syscalls::getgroups(static_cast<int>(groups.size()), groups.data()));
  groups.resize(static_cast<size_t>(n));
  ASSERT_OK_AND_ASSIGN(ProcCredentials read,
                       ReadProcCredentials(syscalls::getpid()));
  EXPECT_EQ(read.fsuid, 0u);
  EXPECT_THAT(read.groups, UnorderedElementsAreArray(groups));
  EXPECT_NE(read.cap_effective & kFsetid, 0u);
  EXPECT_THAT(SharesMyUserNamespace(syscalls::getpid()), IsOkAndHolds(true));
}

// A filesystem uid other than 0 drops the filesystem capabilities, CAP_FSETID
// among them, from the effective set (cap_task_fix_setuid), and /proc shows
// both, for another thread of this process.
TEST(ProcStatusTest, ReadsAThreadThatDroppedCapFsetid) {
  testonly::ThreadWithoutCapFsetid thread;
  ASSERT_OK_AND_ASSIGN(ProcCredentials read, ReadProcCredentials(thread.tid()));
  EXPECT_EQ(read.fsuid, 1000u);
  EXPECT_EQ(read.cap_effective & kFsetid, 0u);
  EXPECT_THAT(SharesMyUserNamespace(thread.tid()), IsOkAndHolds(true));
}

// pid 0, what the kernel sends for a caller outside the reader's pid
// namespace, has no /proc entry.
TEST(ProcStatusTest, PidZeroIsUnreadable) {
  EXPECT_THAT(ReadProcCredentials(0), Not(IsOk()));
  EXPECT_THAT(SharesMyUserNamespace(0), Not(IsOk()));
}

}  // namespace
}  // namespace dcfs
