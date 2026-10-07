#include "dcfs/fd.h"

#include <fcntl.h>
#include <unistd.h>

#include "absl/log/check.h"
#include "absl/status/status_matchers.h"
#include "dcfs/status.h"
#include "dcfs/syscalls.h"
#include "gtest/gtest.h"

namespace dcfs {
namespace {

using ::absl_testing::IsOk;

TEST(FileDescriptorTest, DefaultConstructedInvalid) {
  FileDescriptor fd;
  EXPECT_FALSE(fd.valid());
}

// An O_PATH descriptor on TEST_TMPDIR, as a plain int the test owns (the
// tests below hand it to a FileDescriptor and take it back).
int OpenTmpdir() {
  const char *tmpdir = std::getenv("TEST_TMPDIR");
  CHECK(tmpdir != nullptr);
  absl::StatusOr<FileDescriptor> fd =
      syscalls::openat(AT_FDCWD, tmpdir, O_PATH | O_DIRECTORY);
  CHECK_OK(fd);
  return std::move(*fd).Release();
}

TEST(FileDescriptorTest, ConstructedWithFdIsValid) {
  FileDescriptor fd(STDOUT_FILENO);
  EXPECT_TRUE(fd.valid());
  EXPECT_EQ(*fd, STDOUT_FILENO);
  std::move(fd).Release();  // Prevent close in destructor
}

TEST(FileDescriptorTest, MoveConstructorTransfersOwnership) {
  const int tmp_fd = OpenTmpdir();
  ASSERT_GE(tmp_fd, 0);

  FileDescriptor fd1(tmp_fd);
  EXPECT_TRUE(fd1.valid());
  EXPECT_EQ(*fd1, tmp_fd);

  FileDescriptor fd2(std::move(fd1));
  EXPECT_FALSE(fd1.valid());
  EXPECT_TRUE(fd2.valid());
  EXPECT_EQ(*fd2, tmp_fd);

  std::move(fd2).Release();  // Prevent close in destructor
  EXPECT_THAT(syscalls::close(FileDescriptor(tmp_fd)), IsOk());
}

TEST(FileDescriptorTest, MoveAssignmentTransfersOwnership) {
  const int tmp_fd = OpenTmpdir();
  ASSERT_GE(tmp_fd, 0);

  FileDescriptor fd1(tmp_fd);
  EXPECT_TRUE(fd1.valid());
  FileDescriptor fd2;
  EXPECT_FALSE(fd2.valid());

  fd2 = std::move(fd1);
  EXPECT_FALSE(fd1.valid());
  EXPECT_TRUE(fd2.valid());
  EXPECT_EQ(*fd2, tmp_fd);

  std::move(fd2).Release();  // Prevent close in destructor
  EXPECT_THAT(syscalls::close(FileDescriptor(tmp_fd)), IsOk());
}

TEST(FileDescriptorTest, ReleaseStopsDestructorFromClosing) {
  const int tmp_fd = OpenTmpdir();
  ASSERT_GE(tmp_fd, 0);

  FileDescriptor fd(tmp_fd);
  int released_fd = std::move(fd).Release();
  EXPECT_EQ(released_fd, tmp_fd);

  // Verify the fd is still valid (destructor didn't close it)
  EXPECT_THAT(syscalls::fcntl(tmp_fd, F_GETFD), IsOk());

  // Manual cleanup
  EXPECT_THAT(syscalls::close(FileDescriptor(tmp_fd)), IsOk());
}

TEST(FileDescriptorTest, CloseIsIdempotent) {
  const int tmp_fd = OpenTmpdir();
  ASSERT_GE(tmp_fd, 0);

  FileDescriptor fd(tmp_fd);
  // First Close() should succeed
  EXPECT_THAT(fd.Close(), IsOk());
  EXPECT_FALSE(fd.valid());
  // Second Close() should also succeed (idempotent)
  EXPECT_THAT(fd.Close(), IsOk());
  EXPECT_FALSE(fd.valid());
}

TEST(FileDescriptorTest, DestructorClosesValidFd) {
  const int tmp_fd = OpenTmpdir();
  ASSERT_GE(tmp_fd, 0);

  {
    FileDescriptor fd(tmp_fd);
    EXPECT_TRUE(fd.valid());
  }
  // After destructor, the fd should be closed
  auto flags = syscalls::fcntl(tmp_fd, F_GETFD);
  ASSERT_FALSE(flags.ok());
  EXPECT_EQ(StatusToErrno(flags.status()), EBADF);
}

TEST(FileDescriptorTest, DefaultDestructorDoesNotClosePreviouslyReleasedFd) {
  const int tmp_fd = OpenTmpdir();
  ASSERT_GE(tmp_fd, 0);

  int released_fd;
  {
    FileDescriptor fd(tmp_fd);
    released_fd = std::move(fd).Release();
  }
  // Destructor ran but didn't close because fd was released

  // Verify the fd is still valid
  EXPECT_THAT(syscalls::fcntl(released_fd, F_GETFD), IsOk());

  // Manual cleanup
  EXPECT_THAT(syscalls::close(FileDescriptor(released_fd)), IsOk());
}

}  // namespace
}  // namespace dcfs
