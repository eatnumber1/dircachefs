// Step 15.5: the argument parsing, the status combination and the cache
// database checks of fsck.dcfs. The helper as a process (findmnt, the
// backing's fsck, fsck(8)'s statuses through systemd) is the guests'
// (test/qemu/guest/mount_dcfs.sh, mount_dcfs_systemd.sh).
#include "dcfs/fsck.h"

#include <fcntl.h>
#include <sys/file.h>

#include <cstdint>
#include <cstdlib>
#include <string>
#include <vector>

#include "absl/log/check.h"
#include "absl/status/status.h"
#include "absl/status/status_matchers.h"
#include "absl/status/statusor.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/str_join.h"
#include "dcfs/device_id.h"
#include "dcfs/fd.h"
#include "dcfs/migrate.h"
#include "dcfs/sqlite.h"
#include "dcfs/status.h"
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
using ::testing::Field;
using ::testing::HasSubstr;
using ::testing::Not;

using Strings = std::vector<std::string>;

TEST(FsckNameTest, OnlyFsckDcfs) {
  EXPECT_TRUE(IsFsckHelperName("fsck.dcfs"));
  EXPECT_FALSE(IsFsckHelperName("fsck"));
  EXPECT_FALSE(IsFsckHelperName("fsck.ext4"));
  EXPECT_FALSE(IsFsckHelperName("mount.dcfs"));
}

// fsck(8) ors the statuses of its checkers.
TEST(CombineFsckStatusTest, BitsAreOred) {
  EXPECT_EQ(CombineFsckStatus(kFsckOk, kFsckOk), 0);
  EXPECT_EQ(CombineFsckStatus(kFsckOk, kFsckCorrected), 1);
  EXPECT_EQ(CombineFsckStatus(kFsckCorrected, kFsckUncorrected), 5);
  EXPECT_EQ(CombineFsckStatus(kFsckUncorrected | kFsckOperational,
                              kFsckCorrected),
            13);
  EXPECT_EQ(CombineFsckStatus(kFsckCancelled, kFsckUsage), 48);
  EXPECT_EQ(kFsckCorrected, 1);
  EXPECT_EQ(kFsckReboot, 2);
  EXPECT_EQ(kFsckUncorrected, 4);
  EXPECT_EQ(kFsckOperational, 8);
  EXPECT_EQ(kFsckUsage, 16);
  EXPECT_EQ(kFsckCancelled, 32);
}

TEST(ParseFsckArgsTest, DeviceAlone) {
  ASSERT_OK_AND_ASSIGN(FsckArgs args, ParseFsckArgs(Strings{"/dev/vda"}));
  EXPECT_EQ(args.device, "/dev/vda");
  EXPECT_EQ(args.mode, FsckMode::kReport);  // nobody to ask: report
  EXPECT_TRUE(args.backing_flags.empty());
  EXPECT_FALSE(args.options.has_value());
}

TEST(ParseFsckArgsTest, ModeFlagsAndPassThrough) {
  for (const char *repair : {"-a", "-p", "-y"}) {
    ASSERT_OK_AND_ASSIGN(FsckArgs args,
                         ParseFsckArgs(Strings{repair, "-f", "/dev/vda"}));
    EXPECT_EQ(args.mode, FsckMode::kRepair) << repair;
    EXPECT_EQ(args.backing_flags, (Strings{repair, "-f"}));
  }
  ASSERT_OK_AND_ASSIGN(FsckArgs report,
                       ParseFsckArgs(Strings{"-n", "/dev/vda"}));
  EXPECT_EQ(report.mode, FsckMode::kReport);
  EXPECT_EQ(report.backing_flags, (Strings{"-n"}));
  // The last mode flag wins, as in e2fsck... except that -n beats a repair:
  // fsck(8) never changes what it was told not to.
  ASSERT_OK_AND_ASSIGN(FsckArgs both,
                       ParseFsckArgs(Strings{"-y", "-n", "/dev/vda"}));
  EXPECT_EQ(both.mode, FsckMode::kReport);
}

TEST(ParseFsckArgsTest, ProgressDescriptorAndUnknownFlagsGoToTheBacking) {
  ASSERT_OK_AND_ASSIGN(
      FsckArgs args,
      ParseFsckArgs(Strings{"-a", "-C3", "-T", "/dev/vda"}));
  EXPECT_EQ(args.backing_flags, (Strings{"-a", "-C3", "-T"}));
  EXPECT_EQ(args.device, "/dev/vda");
}

TEST(ParseFsckArgsTest, OptionsAndVersion) {
  ASSERT_OK_AND_ASSIGN(
      FsckArgs args,
      ParseFsckArgs(Strings{"-o", "dcfs.fstype=bind,dcfs.cache_db=/c.db",
                            "/srv/x"}));
  EXPECT_EQ(args.options, "dcfs.fstype=bind,dcfs.cache_db=/c.db");
  EXPECT_TRUE(args.backing_flags.empty());  // -o is ours, not the backing's
  ASSERT_OK_AND_ASSIGN(FsckArgs version, ParseFsckArgs(Strings{"-V"}));
  EXPECT_TRUE(version.version);
}

TEST(ParseFsckArgsTest, UsageErrors) {
  EXPECT_THAT(ParseFsckArgs(Strings{}),
              StatusIs(absl::StatusCode::kInvalidArgument,
                       HasSubstr("device")));
  EXPECT_THAT(ParseFsckArgs(Strings{"-a"}),
              StatusIs(absl::StatusCode::kInvalidArgument));
  EXPECT_THAT(ParseFsckArgs(Strings{"/dev/a", "/dev/b"}),
              StatusIs(absl::StatusCode::kInvalidArgument,
                       HasSubstr("one device")));
  EXPECT_THAT(ParseFsckArgs(Strings{"/dev/a", "-o"}),
              StatusIs(absl::StatusCode::kInvalidArgument,
                       HasSubstr("-o")));
}

// --- the cache database ------------------------------------------------------

std::string TestFile(const std::string &name) {
  const char *tmp = std::getenv("TEST_TMPDIR");
  std::string path = absl::StrCat(tmp != nullptr ? tmp : "/tmp", "/", name);
  for (const char *suffix : {"", "-wal", "-shm"}) {
    (void)syscalls::unlinkat(AT_FDCWD, path + suffix, 0);
  }
  return path;
}

bool Exists(const std::string &path) {
  return syscalls::fstatat(AT_FDCWD, path).ok();
}

RootIdentity TestRoot() {
  DeviceId id;
  id.uuid.fill(0xAB);
  return RootIdentity{
      .device_id = id, .fstype = 0xEF53, .backing_ino = 2, .backing_gen = 0};
}

// A cache database as dcfs leaves it, closed (checkpointed): `sql` runs on it
// first (foreign keys off, so that it can write what dcfs never would).
std::string MakeCache(const std::string &name, const std::string &sql = "") {
  std::string path = TestFile(name);
  absl::StatusOr<sqlite3::Connection> db =
      sqlite3::ConnectionFactory{.path = path}.Open();
  CHECK_OK(db.status());
  CHECK_OK(Migrate(*db, TestRoot()));
  if (!sql.empty()) {
    CHECK_OK(db->Exec("PRAGMA foreign_keys=OFF"));
    CHECK_OK(db->ExecScript(sql));
  }
  CHECK_OK(db->Checkpoint());
  CHECK_OK(db->Close());
  return path;
}

std::string Joined(const CacheReport &report) {
  return absl::StrJoin(report.lines, "\n");
}

TEST(CheckCacheDatabaseTest, NoFileIsNothingToCheck) {
  const std::string path = TestFile("fsck_none.db");
  CacheReport report = CheckCacheDatabase(path, FsckMode::kRepair);
  EXPECT_EQ(report.status, kFsckOk);
  EXPECT_THAT(Joined(report), HasSubstr("no cache database"));
  EXPECT_FALSE(Exists(path)) << "checking creates nothing";
}

TEST(CheckCacheDatabaseTest, AHealthyCacheIsLeftAlone) {
  const std::string path = MakeCache("fsck_ok.db");
  for (FsckMode mode : {FsckMode::kReport, FsckMode::kRepair}) {
    CacheReport report = CheckCacheDatabase(path, mode);
    EXPECT_EQ(report.status, kFsckOk) << Joined(report);
    EXPECT_TRUE(Exists(path));
  }
}

TEST(CheckCacheDatabaseTest, NotADatabaseIsReportedThenRebuilt) {
  const std::string path = TestFile("fsck_garbage.db");
  {
    ASSERT_OK_AND_ASSIGN(
        FileDescriptor fd,
        syscalls::openat(AT_FDCWD, path, O_WRONLY | O_CREAT, 0600));
    const std::string junk(8192, 'x');
    ASSERT_THAT(syscalls::write(*fd, junk.data(), junk.size()), IsOk());
  }
  CacheReport report = CheckCacheDatabase(path, FsckMode::kReport);
  EXPECT_EQ(report.status, kFsckUncorrected) << Joined(report);
  EXPECT_THAT(Joined(report), HasSubstr("-y"));
  EXPECT_TRUE(Exists(path));
  report = CheckCacheDatabase(path, FsckMode::kRepair);
  EXPECT_EQ(report.status, kFsckCorrected) << Joined(report);
  EXPECT_THAT(Joined(report), HasSubstr("cold"));
  EXPECT_FALSE(Exists(path));
}

// The pages after the first (the tables') overwritten: integrity_check (or the first read of the
// page) says so.
TEST(CheckCacheDatabaseTest, AFailedIntegrityCheckIsReportedThenRebuilt) {
  const std::string path = MakeCache("fsck_corrupt.db");
  {
    ASSERT_OK_AND_ASSIGN(FileDescriptor fd,
                         syscalls::openat(AT_FDCWD, path, O_RDWR));
    const std::string junk(16384, '\xff');
    ASSERT_THAT(syscalls::pwrite(*fd, junk.data(), junk.size(), 4096),
                IsOk());
  }
  CacheReport report = CheckCacheDatabase(path, FsckMode::kReport);
  EXPECT_EQ(report.status, kFsckUncorrected) << Joined(report);
  EXPECT_TRUE(Exists(path));
  report = CheckCacheDatabase(path, FsckMode::kRepair);
  EXPECT_EQ(report.status, kFsckCorrected) << Joined(report);
  EXPECT_FALSE(Exists(path));
}

TEST(CheckCacheDatabaseTest, ADatabaseADaemonHoldsIsReportedNotWaitedFor) {
  const std::string path = MakeCache("fsck_held.db");
  // The daemon's lock: an exclusive flock on the file (dcfs/main.cc).
  ASSERT_OK_AND_ASSIGN(FileDescriptor holder,
                       syscalls::openat(AT_FDCWD, path, O_RDWR));
  ASSERT_THAT(syscalls::flock(*holder, LOCK_EX | LOCK_NB), IsOk());
  for (FsckMode mode : {FsckMode::kReport, FsckMode::kRepair}) {
    CacheReport report = CheckCacheDatabase(path, mode);
    EXPECT_EQ(report.status, kFsckOperational) << Joined(report);
    EXPECT_THAT(Joined(report), HasSubstr("in use by a running dcfs"));
    EXPECT_TRUE(Exists(path)) << "a held database is never deleted";
  }
}

TEST(CheckCacheDatabaseTest, ADirtyRowForAMissingInodeIsCorrupt) {
  const std::string path =
      MakeCache("fsck_dirty_missing.db", "INSERT INTO dirty (inode) VALUES (9999);");
  CacheReport report = CheckCacheDatabase(path, FsckMode::kReport);
  EXPECT_EQ(report.status, kFsckUncorrected) << Joined(report);
  EXPECT_THAT(Joined(report), HasSubstr("9999"));
  report = CheckCacheDatabase(path, FsckMode::kRepair);
  EXPECT_EQ(report.status, kFsckCorrected) << Joined(report);
  EXPECT_FALSE(Exists(path));
}

// FinishRun marks a clean shutdown only with an empty dirty set.
TEST(CheckCacheDatabaseTest, CleanShutdownWithADirtySetIsInconsistent) {
  const std::string path = MakeCache(
      "fsck_clean_dirty.db",
      "INSERT INTO dirty (inode) VALUES (1);"
      "UPDATE cache_state SET clean_shutdown = 1;");
  CacheReport report = CheckCacheDatabase(path, FsckMode::kReport);
  EXPECT_EQ(report.status, kFsckUncorrected) << Joined(report);
  EXPECT_THAT(Joined(report), HasSubstr("clean shutdown"));
  report = CheckCacheDatabase(path, FsckMode::kRepair);
  EXPECT_EQ(report.status, kFsckCorrected) << Joined(report);
  EXPECT_FALSE(Exists(path));
}

// What a crash leaves: unclean, with rows for inodes that exist. The next
// start recovers it by itself, so it is not an error.
TEST(CheckCacheDatabaseTest, ACrashedRunsDirtySetIsRecoverable) {
  const std::string path = MakeCache(
      "fsck_crashed.db",
      "INSERT INTO dirty (inode) VALUES (1);"
      "UPDATE cache_state SET clean_shutdown = 0;");
  CacheReport report = CheckCacheDatabase(path, FsckMode::kRepair);
  EXPECT_EQ(report.status, kFsckOk) << Joined(report);
  EXPECT_THAT(Joined(report), HasSubstr("recovers"));
  EXPECT_TRUE(Exists(path));
}

TEST(CheckCacheDatabaseTest, ANewerSchemaIsNeverDeleted) {
  const std::string path = MakeCache(
      "fsck_newer.db",
      absl::StrCat("UPDATE cache_state SET schema_version = ",
                   kSchemaVersion + 1, ";"));
  for (FsckMode mode : {FsckMode::kReport, FsckMode::kRepair}) {
    CacheReport report = CheckCacheDatabase(path, mode);
    EXPECT_EQ(report.status, kFsckUncorrected) << Joined(report);
    EXPECT_THAT(Joined(report), HasSubstr("newer"));
    EXPECT_TRUE(Exists(path));
  }
}

TEST(CheckCacheDatabaseTest, AnOlderSchemaIsUpgradedAtTheNextMount) {
  const std::string path = MakeCache(
      "fsck_older.db", "UPDATE cache_state SET schema_version = 5;");
  CacheReport report = CheckCacheDatabase(path, FsckMode::kRepair);
  EXPECT_EQ(report.status, kFsckOk) << Joined(report);
  EXPECT_THAT(Joined(report), HasSubstr("upgrade"));
  EXPECT_TRUE(Exists(path));
}

TEST(CheckCacheDatabaseTest, ARebuildRemovesTheWalAndSharedMemoryToo) {
  const std::string path = TestFile("fsck_companions.db");
  {
    ASSERT_OK_AND_ASSIGN(
        FileDescriptor fd,
        syscalls::openat(AT_FDCWD, path, O_WRONLY | O_CREAT, 0600));
    ASSERT_THAT(syscalls::write(*fd, "not sqlite", 10), IsOk());
    for (const char *suffix : {"-wal", "-shm"}) {
      ASSERT_THAT(
          syscalls::openat(AT_FDCWD, path + suffix, O_WRONLY | O_CREAT, 0600),
          IsOk());
    }
  }
  CacheReport report = CheckCacheDatabase(path, FsckMode::kRepair);
  EXPECT_EQ(report.status, kFsckCorrected) << Joined(report);
  for (const char *suffix : {"", "-wal", "-shm"}) {
    EXPECT_FALSE(Exists(path + suffix)) << suffix;
  }
}

}  // namespace
}  // namespace dcfs
