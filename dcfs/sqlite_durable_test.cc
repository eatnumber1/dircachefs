// SPDX-License-Identifier: MIT
//
// Connection::MakeDurable's fsync of the WAL, seen through a link-time fake
// of fsync (-Wl,--wrap=fsync, in a target of its own): SQLite's unix VFS
// syncs a file with fsync (it defines fdatasync to fsync unless built with
// HAVE_FDATASYNC). The fake records every fsync's descriptor and forwards
// to the real one unless a test armed a failure.

#include <fcntl.h>
#include <sys/stat.h>

#include <cerrno>
#include <string>
#include <vector>

#include "absl/status/status.h"
#include "absl/status/status_matchers.h"
#include "absl/status/statusor.h"
#include "absl/strings/str_cat.h"
#include "dcfs/sqlite.h"
#include "dcfs/status.h"
#include "dcfs/syscalls_backing.h"
#include "dcfs/testonly/assert_ok_and_assign.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "sqlite3.h"

namespace dcfs::sqlite3 {
namespace {

// The descriptors fsync was called on, in order.
std::vector<int> &Fsyncs() {
  static std::vector<int> fsyncs;
  return fsyncs;
}

// The errno every fsync fails with, or 0 for the real fsync.
int &FsyncFailure() {
  static int failure = 0;
  return failure;
}

}  // namespace
}  // namespace dcfs::sqlite3

extern "C" {

int __real_fsync(int fd);

int __wrap_fsync(int fd) {
  dcfs::sqlite3::Fsyncs().push_back(fd);
  if (dcfs::sqlite3::FsyncFailure() != 0) {
    errno = dcfs::sqlite3::FsyncFailure();
    return -1;
  }
  return __real_fsync(fd);
}

}  // extern "C"

namespace dcfs::sqlite3 {
namespace {

using ::absl_testing::IsOk;
using ::absl_testing::IsOkAndHolds;
using ::testing::ElementsAre;
using ::testing::HasSubstr;
using ::testing::IsEmpty;

class MakeDurableTest : public ::testing::Test {
 protected:
  void SetUp() override {
    // gtest's TempDir is TEST_TMPDIR, with a slash.
    path_ = absl::StrCat(
        ::testing::TempDir(),
        ::testing::UnitTest::GetInstance()->current_test_info()->name(),
        ".sqlite");
    ASSERT_OK_AND_ASSIGN(conn_, ConnectionFactory{.path = path_}.Open());
    // A NORMAL commit: written to the WAL, not fsynced.
    ASSERT_THAT(conn_->Exec("CREATE TABLE t (id INTEGER PRIMARY KEY)"), IsOk());
    Fsyncs().clear();
  }

  void TearDown() override { FsyncFailure() = 0; }

  // The WAL's inode: what each recorded descriptor is compared against.
  ino_t WalInode() const {
    absl::StatusOr<struct stat> st =
        syscalls::fstatat(AT_FDCWD, absl::StrCat(path_, "-wal"));
    EXPECT_THAT(st, IsOk());
    return st.ok() ? st->st_ino : 0;
  }

  // The inodes of the descriptors fsync was called on.
  static std::vector<ino_t> FsyncedInodes() {
    std::vector<ino_t> inodes;
    for (int fd : Fsyncs()) {
      absl::StatusOr<struct stat> st = syscalls::fstat(fd);
      EXPECT_THAT(st, IsOk()) << fd;
      inodes.push_back(st.ok() ? st->st_ino : 0);
    }
    return inodes;
  }

  std::string path_;
  absl::StatusOr<Connection> conn_ = absl::UnknownError("not opened");
};

TEST_F(MakeDurableTest, FsyncsTheWalOnce) {
  ASSERT_THAT(conn_->MakeDurable(), IsOk());

  EXPECT_THAT(FsyncedInodes(), ElementsAre(WalInode()));
  EXPECT_THAT(conn_->CommittedSinceDurable(), IsOkAndHolds(false));
}

// A NORMAL commit fsyncs nothing: the durable point is the only fsync.
TEST_F(MakeDurableTest, NormalCommitDoesNotFsync) {
  ASSERT_THAT(conn_->Exec("INSERT INTO t VALUES (1)"), IsOk());

  EXPECT_THAT(Fsyncs(), IsEmpty());
}

// A failed fsync is reported, and the commits stay owed to the next durable
// point.
TEST_F(MakeDurableTest, FailedFsyncLeavesTheCommitsOwed) {
  FsyncFailure() = EIO;

  absl::Status status = conn_->MakeDurable();

  EXPECT_THAT(GetSqliteCodeFromStatus(status),
              IsOkAndHolds(SQLITE_IOERR_FSYNC));
  EXPECT_EQ(StatusToErrno(status), EIO);
  EXPECT_THAT(status.message(), HasSubstr("WAL"));
  EXPECT_THAT(conn_->CommittedSinceDurable(), IsOkAndHolds(true));
  FsyncFailure() = 0;
  ASSERT_THAT(conn_->MakeDurable(), IsOk());
  EXPECT_THAT(conn_->CommittedSinceDurable(), IsOkAndHolds(false));
}

}  // namespace
}  // namespace dcfs::sqlite3
