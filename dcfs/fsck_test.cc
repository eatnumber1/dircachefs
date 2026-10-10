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
#include "dcfs/testonly/files.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"

extern "C" {
pid_t __real_fork();
pid_t __real_waitpid(pid_t pid, int *status, int options);
int __real_dup2(int oldfd, int newfd);
int __real_flock(int fd, int operation);
int __real_unlinkat(int dirfd, const char *path, int flags);
}

namespace {

// The error each call answers with once it is armed (0: the real call), and
// how many calls it fails.
struct Fault {
  int error = 0;
  int times = 0;
};
Fault g_fork;
Fault g_waitpid;
Fault g_dup2;
Fault g_flock;
Fault g_unlinkat;

bool Fails(Fault &fault) {
  if (fault.error == 0 || fault.times == 0) return false;
  --fault.times;
  errno = fault.error;
  return true;
}

}  // namespace

extern "C" {
pid_t __wrap_fork() { return Fails(g_fork) ? -1 : __real_fork(); }
pid_t __wrap_waitpid(pid_t pid, int *status, int options) {
  return Fails(g_waitpid) ? -1 : __real_waitpid(pid, status, options);
}
int __wrap_dup2(int oldfd, int newfd) {
  return Fails(g_dup2) ? -1 : __real_dup2(oldfd, newfd);
}
int __wrap_flock(int fd, int operation) {
  return Fails(g_flock) ? -1 : __real_flock(fd, operation);
}
int __wrap_unlinkat(int dirfd, const char *path, int flags) {
  return Fails(g_unlinkat) ? -1 : __real_unlinkat(dirfd, path, flags);
}
}


namespace dcfs {
namespace {

using ::absl_testing::IsOk;
using ::absl_testing::IsOkAndHolds;
using ::absl_testing::StatusIs;
using ::testing::Field;
using ::testing::HasSubstr;
using ::testing::Not;
using ::testing::SizeIs;

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

// Reports `path` with -n, then rebuilds it with -y: what the user sees of a
// database that is corrupt in the way `sql` made it.
void ExpectCorruptThenRebuilt(const std::string &path,
                              const std::string &finding) {
  CacheReport report = CheckCacheDatabase(path, FsckMode::kReport);
  EXPECT_EQ(report.status, kFsckUncorrected) << Joined(report);
  EXPECT_THAT(Joined(report), HasSubstr(finding));
  EXPECT_TRUE(Exists(path));
  report = CheckCacheDatabase(path, FsckMode::kRepair);
  EXPECT_EQ(report.status, kFsckCorrected) << Joined(report);
  EXPECT_FALSE(Exists(path));
}

// An index dropped from the schema without its pages (what a half-applied
// migration or a tool that edits sqlite_master leaves): integrity_check lists
// the pages nothing uses, and does not fail (garbage in the pages is the case
// above).
TEST(CheckCacheDatabaseTest, PagesNothingUsesAreCorrupt) {
  ExpectCorruptThenRebuilt(
      MakeCache("fsck_orphans.db",
                "PRAGMA writable_schema=ON;"
                "DELETE FROM sqlite_master WHERE name = 'dentries_inode';"),
      "integrity_check found: ");
}

TEST(CheckCacheDatabaseTest, NoCacheStateRowMeansNoReadableSchemaVersion) {
  ExpectCorruptThenRebuilt(
      MakeCache("fsck_no_state.db", "DELETE FROM cache_state;"),
      "no readable schema version");
}

TEST(CheckCacheDatabaseTest, ALostDirtyTableMeansAnUnreadableDirtySet) {
  ExpectCorruptThenRebuilt(MakeCache("fsck_no_dirty.db", "DROP TABLE dirty;"),
                           "dirty set unreadable");
}

TEST(CheckCacheDatabaseTest, ALostCleanFlagIsUnreadable) {
  ExpectCorruptThenRebuilt(
      MakeCache("fsck_no_flag.db",
                "ALTER TABLE cache_state DROP COLUMN clean_shutdown;"),
      "clean-shutdown flag is unreadable");
}

// Someone holds the database in a way flock does not show (a process that
// is not a dcfs daemon, with the exclusive lock of WAL mode): reported, not
// waited for and not touched.
TEST(CheckCacheDatabaseTest, ADatabaseSQLiteHasLockedIsReportedNotTouched) {
  const std::string path = MakeCache("fsck_sqlite_locked.db");
  ASSERT_OK_AND_ASSIGN(sqlite3::Connection holder,
                       sqlite3::ConnectionFactory{.path = path}.Open());
  ASSERT_THAT(holder.Exec("PRAGMA locking_mode=EXCLUSIVE"), IsOk());
  ASSERT_THAT(holder.Exec("UPDATE cache_state SET boot_id = 'held'"), IsOk());
  for (FsckMode mode : {FsckMode::kReport, FsckMode::kRepair}) {
    CacheReport report = CheckCacheDatabase(path, mode);
    EXPECT_EQ(report.status, kFsckOperational) << Joined(report);
    EXPECT_THAT(Joined(report), HasSubstr("is locked"));
    EXPECT_TRUE(Exists(path)) << "a locked database is never deleted";
  }
}

// O_NOFOLLOW: a link where the cache database should be is not followed (the
// file it names could be anyone's).
TEST(CheckCacheDatabaseTest, ALinkIsNotFollowed) {
  const std::string target = MakeCache("fsck_link_target.db");
  const std::string link = TestFile("fsck_link.db");
  ASSERT_THAT(syscalls::symlinkat(target, AT_FDCWD, link), IsOk());
  for (FsckMode mode : {FsckMode::kReport, FsckMode::kRepair}) {
    CacheReport report = CheckCacheDatabase(link, mode);
    EXPECT_EQ(report.status, kFsckOperational) << Joined(report);
    EXPECT_THAT(Joined(report), HasSubstr("cannot open"));
    EXPECT_TRUE(Exists(target));
  }
}

// --- fsck.dcfs as a process: the programs it runs ------------------------------
//
// FsckMain runs findmnt, blkid and the backing's fsck by the paths fsck(8)
// would find them at. The guest has none of them, so each test puts a script
// there (and takes it away).

// Programs a test puts at the paths fsck.dcfs runs them from (findmnt, blkid and
// the backing's fsck, which it finds by fixed paths), and takes away again
// when the object goes. The test guest has none of them.
class StandInPrograms {
 public:
  StandInPrograms() = default;
  StandInPrograms(const StandInPrograms &) = delete;
  StandInPrograms &operator=(const StandInPrograms &) = delete;
  ~StandInPrograms() {
    for (const std::string &path : installed_) {
      syscalls::unlinkat(AT_FDCWD, path, 0).IgnoreError();
    }
  }

  // A file at `path` (its directories made) with `text` in it, mode `mode`.
  void Install(const std::string &path, std::string_view text,
               mode_t mode = 0755) {
    const std::string dir = path.substr(0, path.rfind('/'));
    if (absl::Status made = syscalls::mkdirat(AT_FDCWD, dir, 0755);
        !made.ok()) {
      CHECK(StatusToErrno(made) == EEXIST) << made;
    }
    installed_.push_back(path);
    absl::StatusOr<FileDescriptor> fd = syscalls::openat(
        AT_FDCWD, path, O_WRONLY | O_CREAT | O_TRUNC, mode);
    CHECK_OK(fd.status());
    CHECK_OK(syscalls::fchmod(**fd, mode));
    CHECK_OK(syscalls::write(**fd, text.data(), text.size()).status());
  }

  // A shell script at `path` that writes its arguments to `log`, then runs
  // `body`.
  void InstallScript(const std::string &path, const std::string &log,
                     std::string_view body) {
    Install(path, "#!/bin/sh\necho \"$*\" >" + log + "\n" + std::string(body) +
                      "\n");
  }

 private:
  std::vector<std::string> installed_;
};


class FsckMainTest : public ::testing::Test {
 protected:
  static std::string Contents(const std::string &path) {
    absl::StatusOr<std::string> text = testonly::ReadFileToString(path);
    CHECK_OK(text.status());
    return *text;
  }

  static int Run(const std::vector<std::string> &words) {
    return FsckMain(words);
  }

  StandInPrograms programs_;
};

// The fsck(8) of a line with a passno gets only the device: the options are
// the fstab line's, as findmnt prints them (the first line only).
TEST_F(FsckMainTest, TheFstabLineSuppliesTheOptions) {
  const std::string db = TestFile("fsck_main_fstab.db");
  const std::string findmnt_log = TestFile("findmnt.args");
  const std::string fsck_log = TestFile("fsck.fakefs.args");
  programs_.InstallScript("/bin/findmnt", findmnt_log,
                   absl::StrCat("echo dcfs.fstype=fakefs,dcfs.cache_db=", db,
                                "\necho dcfs.fstype=otherfs"));
  programs_.InstallScript("/sbin/fsck.fakefs", fsck_log, "exit 0");
  EXPECT_EQ(Run({"-n", "/dev/fstabdev"}), kFsckOk);
  EXPECT_EQ(Contents(findmnt_log),
            "--fstab --noheadings --raw --output OPTIONS --source "
            "/dev/fstabdev\n");
  EXPECT_EQ(Contents(fsck_log), "-n /dev/fstabdev\n");
}

TEST_F(FsckMainTest, NoFstabLineIsOperational) {
  programs_.InstallScript("/bin/findmnt", TestFile("findmnt.args"), "exit 1");
  EXPECT_EQ(Run({"-n", "/dev/nowhere"}), kFsckOperational);
  // findmnt exits 0 and prints nothing for a source with an empty line.
  programs_.InstallScript("/bin/findmnt", TestFile("findmnt.args"), "exit 0");
  EXPECT_EQ(Run({"-n", "/dev/nowhere"}), kFsckOperational);
}

TEST_F(FsckMainTest, OptionsThatDoNotSplitAreOperational) {
  EXPECT_EQ(Run({"-o", "dcfs.nosuchoption=1", "/dev/fake"}),
            kFsckOperational);
}

TEST_F(FsckMainTest, ATypeNotSetIsFoundWithBlkid) {
  const std::string db = TestFile("fsck_main_blkid.db");
  const std::string blkid_log = TestFile("blkid.args");
  programs_.InstallScript("/sbin/blkid", blkid_log, "echo fakefs");
  programs_.InstallScript("/sbin/fsck.fakefs", TestFile("fsck.fakefs.args"),
                   "exit 4");
  EXPECT_EQ(Run({"-n", "-o", absl::StrCat("dcfs.cache_db=", db),
                 "/dev/fake"}),
            kFsckUncorrected);
  EXPECT_EQ(Contents(blkid_log), "-o value -s TYPE /dev/fake\n");
}

TEST_F(FsckMainTest, BlkidFindingNothingIsOperational) {
  const std::string options =
      absl::StrCat("dcfs.cache_db=", TestFile("fsck_main_none.db"));
  programs_.InstallScript("/sbin/blkid", TestFile("blkid.args"), "exit 2");
  EXPECT_EQ(Run({"-o", options, "/dev/fake"}), kFsckOperational);
  programs_.InstallScript("/sbin/blkid", TestFile("blkid.args"), "exit 0");
  EXPECT_EQ(Run({"-o", options, "/dev/fake"}), kFsckOperational);
}

TEST_F(FsckMainTest, NoBlkidMeansTheTypeMustBeSet) {
  ASSERT_FALSE(Exists("/sbin/blkid")) << "the guest has a blkid";
  EXPECT_EQ(Run({"-o", "dcfs.cache_db=/nowhere.db", "/dev/fake"}),
            kFsckOperational);
}

TEST_F(FsckMainTest, ATypeThatIsAPathIsNotRun) {
  EXPECT_EQ(Run({"-o", "dcfs.fstype=../tmp/x,dcfs.cache_db=/nowhere.db",
                 "/dev/fake"}),
            kFsckOperational);
}

TEST_F(FsckMainTest, ACheckerThatCannotBeRunIsOperational) {
  programs_.Install("/sbin/fsck.fakefs", "#!/bin/sh\nexit 0\n", 0644);
  EXPECT_EQ(Run({"-o", "dcfs.fstype=fakefs,dcfs.cache_db=/nowhere.db",
                 "/dev/fake"}),
            kFsckOperational);
}

TEST_F(FsckMainTest, ACheckerStoppedByCtrlCIsCancelled) {
  programs_.Install("/sbin/fsck.fakefs", "#!/bin/sh\nkill -INT $$\n");
  EXPECT_EQ(Run({"-o", "dcfs.fstype=fakefs,dcfs.cache_db=/nowhere.db",
                 "/dev/fake"}),
            kFsckCancelled);
}

TEST_F(FsckMainTest, ACheckerKilledBySomethingElseIsOperational) {
  programs_.Install("/sbin/fsck.fakefs", "#!/bin/sh\nkill -KILL $$\n");
  EXPECT_EQ(Run({"-o", "dcfs.fstype=fakefs,dcfs.cache_db=/nowhere.db",
                 "/dev/fake"}),
            kFsckOperational);
}

// --- fork, waitpid, dup2, flock and unlink answering with an error ----------------
//
// What no real run makes on demand: a Linux that has run out of processes
// (RLIMIT_NPROC), a lock manager that refuses (flock on NFS: ENOLCK), a cache
// directory the user cannot write. Link-time wraps of the five calls
// (fsck_test's linkopts in BUILD.bazel) forward to the real call unless a test
// armed a fault for it. The child of RunProgram is a forked copy of this
// process, so the armed fault is what it sees.

constexpr char kFakeOptions[] = "dcfs.fstype=fakefs,dcfs.cache_db=/nowhere.db";

class FsckFaultTest : public ::testing::Test {
 protected:
  void TearDown() override {
    g_fork = g_waitpid = g_dup2 = g_flock = g_unlinkat = Fault();
  }

  static void Create(const std::string &path, const std::string &text) {
    absl::StatusOr<FileDescriptor> fd =
        syscalls::openat(AT_FDCWD, path, O_WRONLY | O_CREAT, 0600);
    CHECK_OK(fd.status());
    CHECK_OK(syscalls::write(**fd, text.data(), text.size()).status());
  }

  static int Check(const std::vector<std::string> &words) {
    return FsckMain(words);
  }

  StandInPrograms programs_;
};

// flock refusing with something other than "held": the user is told what.
TEST_F(FsckFaultTest, ALockThatCannotBeTakenIsReported) {
  const std::string path = TestFile("fsck_fault_lock.db");
  Create(path, "not looked at");
  g_flock = {.error = ENOLCK, .times = 1};
  const CacheReport report = CheckCacheDatabase(path, FsckMode::kRepair);
  EXPECT_EQ(report.status, kFsckOperational);
  ASSERT_THAT(report.lines, SizeIs(1));
  EXPECT_THAT(report.lines[0], HasSubstr("cannot lock the cache database"));
  EXPECT_THAT(syscalls::fstatat(AT_FDCWD, path), absl_testing::IsOk())
      << "a database that could not be locked is never deleted";
}

// The corrupt cache that cannot be deleted stays, and the user is told.
TEST_F(FsckFaultTest, ACorruptCacheThatCannotBeDeletedIsReported) {
  const std::string path = TestFile("fsck_fault_unlink.db");
  Create(path, std::string(8192, 'x'));
  g_unlinkat = {.error = EBUSY, .times = 1};
  const CacheReport report = CheckCacheDatabase(path, FsckMode::kRepair);
  EXPECT_EQ(report.status, kFsckOperational);
  ASSERT_THAT(report.lines, SizeIs(1));
  EXPECT_THAT(report.lines[0], HasSubstr("cannot delete the corrupt cache"));
  EXPECT_THAT(syscalls::fstatat(AT_FDCWD, path), absl_testing::IsOk());
}

TEST_F(FsckFaultTest, NoProcessForTheBackingsCheckerIsOperational) {
  programs_.Install("/sbin/fsck.fakefs", "#!/bin/sh\nexit 0\n");
  g_fork = {.error = EAGAIN, .times = 1};
  EXPECT_EQ(Check({"-o", kFakeOptions, "/dev/fake"}), kFsckOperational);
}

TEST_F(FsckFaultTest, NoProcessForFindmntIsOperational) {
  g_fork = {.error = EAGAIN, .times = 1};
  EXPECT_EQ(Check({"/dev/fake"}), kFsckOperational);
}

// findmnt's standard output could not be made its own: it does not run, and
// the status says so.
TEST_F(FsckFaultTest, AChildWhoseOutputCannotBeRedirectedDoesNotRun) {
  g_dup2 = {.error = EBADF, .times = 1};
  EXPECT_EQ(Check({"/dev/fake"}), kFsckOperational);
}

TEST_F(FsckFaultTest, AWaitThatFailsIsOperational) {
  programs_.Install("/sbin/fsck.fakefs", "#!/bin/sh\nexit 0\n");
  g_waitpid = {.error = ECHILD, .times = 1};
  EXPECT_EQ(Check({"-o", kFakeOptions, "/dev/fake"}), kFsckOperational);
}

// A signal that interrupts the wait is not the checker's failure.
TEST_F(FsckFaultTest, AWaitThatASignalInterruptedIsRetried) {
  programs_.Install("/sbin/fsck.fakefs", "#!/bin/sh\nexit 4\n");
  g_waitpid = {.error = EINTR, .times = 2};
  EXPECT_EQ(Check({"-o", kFakeOptions, "/dev/fake"}), kFsckUncorrected);
  EXPECT_EQ(g_waitpid.times, 0);
}

}  // namespace
}  // namespace dcfs
