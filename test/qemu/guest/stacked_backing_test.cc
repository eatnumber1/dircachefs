// SPDX-License-Identifier: MIT
// dcfs serves a backing the kernel refuses FUSE passthrough for. The kernel
// refuses per backing file: FUSE_DEV_IOC_BACKING_OPEN fails with ELOOP when
// the backing filesystem's s_stack_depth is at least the connection's
// max_stack_depth (fuse_backing_open, fs/fuse/backing.c:111-114 (Linux
// v7.3-rc7)), which dcfs sets to 2 (FUSE_BACKING_STACKED_OVER). An overlayfs
// over the test disk has depth 1 and gets passthrough; an overlayfs over that
// overlayfs has depth 2 and does not, so dcfs's own read, write and fallocate
// serve it. Each scenario runs on a directory of the backing itself and on one
// served by dcfs, and the two answers must be the same (the backing is the
// reference); the dcfs log says whether the kernel granted passthrough.
#include <fcntl.h>
#include <linux/falloc.h>

#include <memory>
#include <string>
#include <string_view>

#include "absl/status/status_macros.h"
#include "absl/status/statusor.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/str_format.h"
#include "dcfs/fd.h"
#include "dcfs/status.h"
#include "dcfs/syscalls.h"
#include "dcfs/syscalls_backing.h"
#include "dcfs/testonly/assert_ok_and_assign.h"
#include "dcfs/testonly/files.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "test/qemu/guest/guest.h"

namespace dcfs {
namespace {

using ::testing::HasSubstr;
using ::testing::Not;

// What libfuse logs for the ioctl that failed with ELOOP (strerror(ELOOP)).
constexpr std::string_view kRefusal =
    "passthrough_open: Too many levels of symbolic links";

// Mounts an overlayfs of `lower` (a directory) over a fresh upper layer under
// `root`, named `name`, and returns its merged directory. nfs_export=on lets
// dcfs open files by handle.
absl::StatusOr<std::string> Overlay(std::string_view root,
                                    std::string_view name,
                                    std::string_view lower) {
  const std::string upper = absl::StrCat(root, "/", name, "_upper");
  const std::string work = absl::StrCat(root, "/", name, "_work");
  std::string merged = absl::StrCat(root, "/", name);
  RETURN_IF_ERROR(Mkdir(upper));
  RETURN_IF_ERROR(Mkdir(work));
  RETURN_IF_ERROR(Mkdir(merged));
  const std::string options =
      absl::StrCat("lowerdir=", lower, ",upperdir=", upper, ",workdir=", work,
                   ",index=on,nfs_export=on");
  RETURN_IF_ERROR(
      syscalls::mount("overlay", merged, "overlay", 0, options.c_str()));
  return merged;
}

// The same operations on a file under `dir`, and what each one answered: a
// write, a fallocate that extends the file, a punched hole, a truncate and
// reads of the result.
absl::StatusOr<std::string> Exercise(const std::string &dir) {
  const std::string path = absl::StrCat(dir, "/file");
  std::string out;
  ASSIGN_OR_RETURN(FileDescriptor fd,
                   syscalls::open(path, O_RDWR | O_CREAT | O_TRUNC, 0644));
  absl::StrAppend(&out, "write=",
                  ErrnoToErrorName(StatusToErrno(
                      syscalls::pwrite(*fd, "hello world", 11, 0).status())));
  absl::StrAppend(
      &out, " fallocate=",
      ErrnoToErrorName(StatusToErrno(syscalls::fallocate(*fd, 0, 0, 8192))));
  absl::StrAppend(&out, " punch=",
                  ErrnoToErrorName(StatusToErrno(syscalls::fallocate(
                      *fd, FALLOC_FL_PUNCH_HOLE | FALLOC_FL_KEEP_SIZE, 1, 3))));
  absl::StrAppend(&out, " append=",
                  ErrnoToErrorName(StatusToErrno(
                      syscalls::pwrite(*fd, "tail", 4, 8190).status())));
  ASSIGN_OR_RETURN(struct stat st, syscalls::fstat(*fd));
  absl::StrAppendFormat(&out, " size=%d", st.st_size);
  RETURN_IF_ERROR(syscalls::ftruncate(*fd, 9000));
  ASSIGN_OR_RETURN(st, syscalls::fstat(*fd));
  absl::StrAppendFormat(&out, " truncated=%d", st.st_size);
  RETURN_IF_ERROR(syscalls::fsync(*fd));
  fd = FileDescriptor();
  ASSIGN_OR_RETURN(std::string contents, testonly::ReadFileToString(path));
  absl::StrAppendFormat(
      &out, " read=%d,%s", contents.size(),
      absl::StrCat(
          contents.substr(0, 11), "|", contents.substr(8190, 4), "|",
          contents.size() > 8194 ? contents.substr(8194, 1) + "..." : ""));
  return out;
}

class StackedBackingTest : public ::testing::Test {
 protected:
  // Layers on the test disk: depth 1 is an overlayfs over a directory of it,
  // depth 2 an overlayfs over that overlayfs.
  static absl::StatusOr<std::string> Stack(int depth) {
    const std::string root = absl::StrCat(TestTmp(), "/stack", depth);
    RETURN_IF_ERROR(Mkdir(root));
    RETURN_IF_ERROR(Mkdir(absl::StrCat(root, "/base")));
    ASSIGN_OR_RETURN(std::string top,
                     Overlay(root, "one", absl::StrCat(root, "/base")));
    if (depth == 2) {
      ASSIGN_OR_RETURN(top, Overlay(root, "two", top));
    }
    return top;
  }

  // Runs Exercise() on the backing's own directory and on dcfs's, and returns
  // the daemon's log.
  static absl::StatusOr<std::string> Differential(int depth) {
    ASSIGN_OR_RETURN(std::string top, Stack(depth));
    RETURN_IF_ERROR(Mkdir(absl::StrCat(top, "/native")));
    RETURN_IF_ERROR(Mkdir(absl::StrCat(top, "/served")));
    const std::string mnt = absl::StrCat(TestTmp(), "/mnt", depth);
    RETURN_IF_ERROR(Mkdir(mnt));
    ASSIGN_OR_RETURN(
        std::unique_ptr<DcfsMount> mount,
        DcfsMount::Start(
            {.source = top,
             .mountpoint = mnt,
             .cache_db = absl::StrCat(TestTmp(), "/cache", depth, ".db")}));
    ASSIGN_OR_RETURN(std::string native,
                     Exercise(absl::StrCat(top, "/native")));
    ASSIGN_OR_RETURN(std::string served,
                     Exercise(absl::StrCat(mnt, "/served")));
    ExpectSameAsBacking(native, served);
    EXPECT_THAT(native, HasSubstr("write=OK")) << "the reference must work";
    ASSIGN_OR_RETURN(std::string log, mount->Log());
    RETURN_IF_ERROR(mount->Unmount());
    return log;
  }
};

TEST_F(StackedBackingTest, AnOverlayOverAnOverlayIsServedByDcfsItself) {
  ASSERT_OK_AND_ASSIGN(std::string log, Differential(2));
  EXPECT_THAT(log, HasSubstr(kRefusal)) << "ELOOP is what refused passthrough";
}

TEST_F(StackedBackingTest, AnOverlayOverTheDiskGetsPassthrough) {
  ASSERT_OK_AND_ASSIGN(std::string log, Differential(1));
  EXPECT_THAT(log, Not(HasSubstr("passthrough_open")));
}

}  // namespace
}  // namespace dcfs
