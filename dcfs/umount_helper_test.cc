// Step 15.6b: the lock the daemon holds for umount.fuse.dcfs to wait on. Waiting
// for a real daemon, and what the helper does around a real unmount, are
// test/qemu/guest/mount_dcfs.sh and the systemd guest: they take a mount.
#include "dcfs/umount_helper.h"

#include <fcntl.h>
#include <sys/file.h>
#include <sys/wait.h>

#include <cstdlib>
#include <string>

#include "absl/log/log_entry.h"
#include "absl/log/log_sink.h"
#include "absl/log/log_sink_registry.h"
#include "absl/status/status.h"
#include "absl/status/status_matchers.h"
#include "dcfs/fd.h"
#include "dcfs/status.h"
#include "dcfs/syscalls.h"
#include "dcfs/syscalls_backing.h"
#include "dcfs/syscalls_process.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"

namespace dcfs {
namespace {

using ::absl_testing::IsOk;
using ::absl_testing::StatusIs;
using ::testing::HasSubstr;

std::string LockDir(const std::string &name) {
  const char *tmp = std::getenv("TEST_TMPDIR");
  return std::string(tmp != nullptr ? tmp : "/tmp") + "/" + name;
}

// A mountinfo line for a dcfs mount at /m on device 0:`minor`.
std::string Info(int minor) {
  return "40 26 0:" + std::to_string(minor) +
         " / /m rw - fuse.dcfs /dev/vdb rw\n";
}

TEST(CanonicalMountpointTest, ResolvesWhatIsThere) {
  EXPECT_THAT(CanonicalMountpoint("/proc/self/../self"),
              absl_testing::IsOkAndHolds(testing::StartsWith("/proc/")));
  EXPECT_THAT(CanonicalMountpoint("/no/such/dir"),
              StatusIs(absl::StatusCode::kNotFound));
}

TEST(HoldDaemonLockTest, TakesTheLockOfItsDeviceInTheDirectory) {
  const std::string dir = LockDir("hold1");
  absl::StatusOr<DaemonLock> lock = HoldDaemonLock(Info(91), "/m", dir);
  ASSERT_THAT(lock, IsOk());
  EXPECT_EQ(lock->path, dir + "/0_91.lock");
  EXPECT_THAT(syscalls::fstatat(AT_FDCWD, lock->path), IsOk());
}

TEST(HoldDaemonLockTest, AMountThatIsNotThereIsAnInternalError) {
  EXPECT_THAT(HoldDaemonLock("26 1 8:1 / / rw - ext4 /dev/vda rw\n", "/m",
                             LockDir("hold2")),
              StatusIs(absl::StatusCode::kInternal, HasSubstr("/m")));
}

TEST(HoldDaemonLockTest, ADirectoryThatCannotBeMadeOrUsedIsAnError) {
  // No parent for the one level it makes.
  EXPECT_THAT(HoldDaemonLock(Info(93), "/m", LockDir("no/such/parent/dir")),
              StatusIs(absl::StatusCode::kNotFound,
                       HasSubstr("creating")));
  // A file where the directory should be: the lock file cannot be opened in it.
  const std::string file = LockDir("hold_is_a_file");
  absl::StatusOr<FileDescriptor> made =
      syscalls::openat(AT_FDCWD, file, O_RDWR | O_CREAT, 0600);
  ASSERT_THAT(made, IsOk());
  EXPECT_THAT(HoldDaemonLock(Info(93), "/m", file), testing::Not(IsOk()));
}

TEST(RemoveDaemonLockFileTest, ARemovalThatFailsOnlyWarns) {
  // Gone already: nothing to say. A directory cannot be unlinked: a warning,
  // and the daemon goes on shutting down.
  RemoveDaemonLockFile({.path = LockDir("never_there.lock")});
  RemoveDaemonLockFile({.path = "/proc"});
}

// A child that holds the lock of device 0:`minor` in `dir`, says so on `to_parent`,
// waits for a byte on `from_parent`, then (`unlink_it`) removes the file as a
// daemon does, and exits.
pid_t ForkHolder(const std::string &dir, int minor, bool unlink_it,
                 int to_parent, int from_parent) {
  absl::StatusOr<pid_t> child = syscalls::fork();
  if (!child.ok()) return -1;
  if (*child != 0) return *child;
  const std::string path = DaemonLockPath("0:" + std::to_string(minor), dir);
  absl::StatusOr<FileDescriptor> fd =
      syscalls::openat(AT_FDCWD, path, O_RDWR | O_CREAT, 0600);
  char byte = 'x';
  if (!fd.ok() || !syscalls::flock(**fd, LOCK_EX).ok()) {
    (void)syscalls::write(to_parent, &byte, 1);
    syscalls::_exit(1);
  }
  byte = 'h';
  (void)syscalls::write(to_parent, &byte, 1);
  (void)syscalls::read(from_parent, &byte, 1);
  if (unlink_it) (void)syscalls::unlinkat(AT_FDCWD, path, 0);
  syscalls::_exit(0);
}

// The daemon of a mount whose device an earlier daemon still holds waits for
// it (it is exiting: a live mount holds its device number): the answer is a
// lock, not a refusal.
// Says on `fd` ('w') when the daemon logs that it is waiting for the holder:
// the line is logged after the non-blocking attempt found the lock held and
// just before the blocking one, so it is the event for "has reached the wait".
class WaitingSink : public absl::LogSink {
 public:
  explicit WaitingSink(int fd) : fd_(fd) {}
  void Send(const absl::LogEntry &entry) override {
    if (entry.text_message().find("waiting for it to exit") !=
        std::string_view::npos) {
      (void)syscalls::write(fd_, "w", 1);
    }
  }

 private:
  int fd_;
};

void ExpectWaitsForTheHolder(int minor, bool holder_unlinks) {
  const std::string dir = LockDir("wait" + std::to_string(minor));
  (void)syscalls::mkdirat(AT_FDCWD, dir, 0700);
  auto holder_pipe = syscalls::socketpair(AF_UNIX, SOCK_STREAM, 0);
  auto waiter_pipe = syscalls::socketpair(AF_UNIX, SOCK_STREAM, 0);
  ASSERT_THAT(holder_pipe, IsOk());
  ASSERT_THAT(waiter_pipe, IsOk());
  auto &[holder_parent, holder_child] = *holder_pipe;
  auto &[waiter_parent, waiter_child] = *waiter_pipe;
  // The holder holds the lock before the waiter starts.
  const pid_t holder = ForkHolder(dir, minor, holder_unlinks, *holder_child,
                                  *holder_child);
  ASSERT_GT(holder, 0);
  char byte = 0;
  ASSERT_THAT(syscalls::read(*holder_parent, &byte, 1), IsOk());
  ASSERT_EQ(byte, 'h');
  absl::StatusOr<pid_t> waiter = syscalls::fork();
  ASSERT_THAT(waiter, IsOk());
  if (*waiter == 0) {
    WaitingSink sink(*waiter_child);
    absl::AddLogSink(&sink);
    absl::StatusOr<DaemonLock> lock = HoldDaemonLock(Info(minor), "/m", dir);
    // The file it holds is the one at the path, whatever the holder did.
    char answer = lock.ok() && syscalls::fstatat(AT_FDCWD, lock->path).ok()
                      ? 'k'
                      : 'e';
    (void)syscalls::write(*waiter_child, &answer, 1);
    syscalls::_exit(0);
  }
  // The holder lets go only once the waiter has said it is waiting (it
  // would otherwise pass if the holder exited first, without the wait), and
  // exiting is what lets the waiter have the lock.
  ASSERT_THAT(syscalls::read(*waiter_parent, &byte, 1), IsOk());
  ASSERT_EQ(byte, 'w');
  byte = 'r';
  ASSERT_THAT(syscalls::write(*holder_parent, &byte, 1), IsOk());
  int status = 0;
  ASSERT_THAT(syscalls::waitpid(holder, &status, 0), IsOk());
  ASSERT_TRUE(WIFEXITED(status) && WEXITSTATUS(status) == 0);
  ASSERT_THAT(syscalls::read(*waiter_parent, &byte, 1), IsOk());
  EXPECT_EQ(byte, 'k');
  ASSERT_THAT(syscalls::waitpid(*waiter, &status, 0), IsOk());
}

TEST(HoldDaemonLockTest, WaitsForADaemonThatIsStillShuttingDown) {
  ExpectWaitsForTheHolder(94, /*holder_unlinks=*/false);
}

// The holder removes the file before it exits: the waiter's lock, taken on the
// removed file's inode, is not at the path, so it takes the lock again on the
// file there.
TEST(HoldDaemonLockTest, WaitsAndThenLocksTheFileThatIsAtThePath) {
  ExpectWaitsForTheHolder(95, /*holder_unlinks=*/true);
}

}  // namespace
}  // namespace dcfs
