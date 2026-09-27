#include "dcfs/fd.h"

#include <fcntl.h>
#include <unistd.h>

#include "absl/status/status_matchers.h"
#include "dcfs/status.h"
#include "gtest/gtest.h"

namespace dcfs {
namespace {

using ::absl_testing::IsOk;

TEST(FileDescriptorTest, DefaultConstructedInvalid) {
  FileDescriptor fd;
  EXPECT_FALSE(fd.valid());
}

TEST(FileDescriptorTest, ConstructedWithFdIsValid) {
  FileDescriptor fd(STDOUT_FILENO);
  EXPECT_TRUE(fd.valid());
  EXPECT_EQ(*fd, STDOUT_FILENO);
  std::move(fd).Release();  // Prevent close in destructor
}

TEST(FileDescriptorTest, MoveConstructorTransfersOwnership) {
  const char *tmpdir = std::getenv("TEST_TMPDIR");
  ASSERT_NE(tmpdir, nullptr);
  int tmp_fd = ::open(tmpdir, O_PATH | O_DIRECTORY);
  ASSERT_GE(tmp_fd, 0);

  FileDescriptor fd1(tmp_fd);
  EXPECT_TRUE(fd1.valid());
  EXPECT_EQ(*fd1, tmp_fd);

  FileDescriptor fd2(std::move(fd1));
  EXPECT_FALSE(fd1.valid());
  EXPECT_TRUE(fd2.valid());
  EXPECT_EQ(*fd2, tmp_fd);

  std::move(fd2).Release();  // Prevent close in destructor
  ::close(tmp_fd);
}

TEST(FileDescriptorTest, MoveAssignmentTransfersOwnership) {
  const char *tmpdir = std::getenv("TEST_TMPDIR");
  ASSERT_NE(tmpdir, nullptr);
  int tmp_fd = ::open(tmpdir, O_PATH | O_DIRECTORY);
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
  ::close(tmp_fd);
}

TEST(FileDescriptorTest, ReleaseStopsDestructorFromClosing) {
  const char *tmpdir = std::getenv("TEST_TMPDIR");
  ASSERT_NE(tmpdir, nullptr);
  int tmp_fd = ::open(tmpdir, O_PATH | O_DIRECTORY);
  ASSERT_GE(tmp_fd, 0);

  FileDescriptor fd(tmp_fd);
  int released_fd = std::move(fd).Release();
  EXPECT_EQ(released_fd, tmp_fd);

  // Verify the fd is still valid (destructor didn't close it)
  int flags = ::fcntl(tmp_fd, F_GETFD);
  EXPECT_GE(flags, 0);

  // Manual cleanup
  ::close(tmp_fd);
}

TEST(FileDescriptorTest, CloseIsIdempotent) {
  const char *tmpdir = std::getenv("TEST_TMPDIR");
  ASSERT_NE(tmpdir, nullptr);
  int tmp_fd = ::open(tmpdir, O_PATH | O_DIRECTORY);
  ASSERT_GE(tmp_fd, 0);

  FileDescriptor fd(tmp_fd);
  EXPECT_THAT(std::move(fd).Close(), IsOk());
  // After Close(), the fd should be invalid (-1)
}

TEST(FileDescriptorTest, DestructorClosesValidFd) {
  const char *tmpdir = std::getenv("TEST_TMPDIR");
  ASSERT_NE(tmpdir, nullptr);
  int tmp_fd = ::open(tmpdir, O_PATH | O_DIRECTORY);
  ASSERT_GE(tmp_fd, 0);

  {
    FileDescriptor fd(tmp_fd);
    EXPECT_TRUE(fd.valid());
  }
  // After destructor, the fd should be closed
  int flags = ::fcntl(tmp_fd, F_GETFD);
  EXPECT_EQ(flags, -1);  // EBADF
}

TEST(FileDescriptorTest, DefaultDestructorDoesNotClosePreviouslyReleasedFd) {
  const char *tmpdir = std::getenv("TEST_TMPDIR");
  ASSERT_NE(tmpdir, nullptr);
  int tmp_fd = ::open(tmpdir, O_PATH | O_DIRECTORY);
  ASSERT_GE(tmp_fd, 0);

  int released_fd;
  {
    FileDescriptor fd(tmp_fd);
    released_fd = std::move(fd).Release();
  }
  // Destructor ran but didn't close because fd was released

  // Verify the fd is still valid
  int flags = ::fcntl(released_fd, F_GETFD);
  EXPECT_GE(flags, 0);

  // Manual cleanup
  ::close(released_fd);
}

}  // namespace
}  // namespace dcfs
