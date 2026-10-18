// SPDX-License-Identifier: MIT
#include "test/qemu/guest/guest.h"

#include <fcntl.h>
#include <signal.h>  // NOLINT(modernize-deprecated-headers): include-cleaner finds SIGKILL here
#include <stdio.h>  // NOLINT(modernize-deprecated-headers): include-cleaner finds SEEK_SET here
#include <stdlib.h>  // NOLINT(modernize-deprecated-headers): include-cleaner finds WIFEXITED here
#include <sys/poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <cstddef>
#include <iterator>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "absl/algorithm/container.h"
#include "absl/base/nullability.h"
#include "absl/cleanup/cleanup.h"
#include "absl/crc/crc32c.h"
#include "absl/functional/function_ref.h"
#include "absl/log/log.h"
#include "absl/status/status.h"
#include "absl/status/status_macros.h"
#include "absl/status/status_matchers.h"
#include "absl/status/statusor.h"
#include "absl/strings/match.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/str_format.h"
#include "absl/strings/str_join.h"
#include "absl/strings/strip.h"
#include "absl/strings/substitute.h"
#include "absl/time/time.h"
#include "absl/types/optional_ref.h"
#include "dcfs/escape.h"
#include "dcfs/fd.h"
#include "dcfs/ret_check.h"
#include "dcfs/status.h"
#include "dcfs/syscalls.h"
#include "dcfs/syscalls_backing.h"
#include "dcfs/syscalls_process.h"
#include "dcfs/testonly/files.h"
#include "dcfs/testonly/process.h"
#include "dcfs/testonly/status_wire.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"

namespace dcfs {
namespace {

using ::absl_testing::IsOk;

absl::StatusOr<int> Wait(pid_t pid) {
  int wait_status = 0;
  RETURN_IF_ERROR(syscalls::waitpid(pid, &wait_status, 0).status());
  return wait_status;
}

absl::Status WriteFully(int fd, std::string_view data) {
  while (!data.empty()) {
    ASSIGN_OR_RETURN(size_t n, syscalls::write(fd, data.data(), data.size()));
    data.remove_prefix(n);
  }
  return absl::OkStatus();
}

char TypeLetter(mode_t mode) {
  switch (mode & S_IFMT) {
    case S_IFREG:
      return 'f';
    case S_IFDIR:
      return 'd';
    case S_IFLNK:
      return 'l';
    case S_IFIFO:
      return 'p';
    case S_IFSOCK:
      return 's';
    case S_IFCHR:
      return 'c';
    default:
      return 'b';
  }
}

enum class MountWait { kMounted, kDaemonExited };

// Whether `mountpoint` is in the table that `mounts` (an open
// /proc/self/mounts) reads, or the process `pidfd` is of exits first. The
// daemon's exit is the event to wait on, not a timer: the kernel wakes the
// poll when the table changes (it compares with the table as last read) or
// the process exits.
absl::StatusOr<MountWait> WaitForMount(int mounts, int pidfd,
                                       std::string_view mountpoint) {
  std::string needle = absl::StrCat(" ", mountpoint, " ");
  for (;;) {
    RETURN_IF_ERROR(syscalls::lseek(mounts, 0, SEEK_SET).status());
    ASSIGN_OR_RETURN(std::string table, testonly::ReadAll(mounts));
    if (absl::StrContains(table, needle)) return MountWait::kMounted;
    struct pollfd fds[2] = {{.fd = mounts, .events = POLLPRI | POLLERR},
                            {.fd = pidfd, .events = POLLIN}};
    RETURN_IF_ERROR(syscalls::poll(fds, absl::InfiniteDuration()).status());
    if ((fds[1].revents & POLLIN) != 0) return MountWait::kDaemonExited;
  }
}

absl::Status SetIdentity(const Identity &as) {
  RETURN_IF_ERROR(syscalls::setresgid(as.gid, as.gid, as.gid));
  RETURN_IF_ERROR(syscalls::setgroups(as.groups));
  return syscalls::setresuid(as.uid, as.uid, as.uid);
}

absl::StatusOr<std::string> RunAs(
    absl::FunctionRef<absl::StatusOr<std::string>()> fn,
    absl::optional_ref<const Identity> as) {
  if (as.has_value()) {
    RETURN_IF_ERROR(SetIdentity(*as)) << "The child could not become the user";
  }
  return fn();
}

// 'R' and the value, or 'F' and the serialized Status.
std::string EncodeChildResult(const absl::StatusOr<std::string> &res) {
  if (res.ok()) return absl::StrCat("R", *res);
  return absl::StrCat("F", testonly::SerializeStatus(res.status()));
}

absl::StatusOr<std::string> DecodeChildMessage(std::string_view got) {
  if (got.empty()) {
    return InternalErrorBuilder()
           << "The child exited without producing an answer";
  }
  std::string_view payload = got.substr(1);
  if (got.front() == 'R') return std::string(payload);
  RET_CHECK_EQ(got.front(), 'F') << "a child's answer starts with R or F";
  ASSIGN_OR_RETURN(absl::Status status, testonly::ParseStatus(payload));
  return status;
}

// In the child, where there is nobody to tell: a failed write is a child that
// died, which the parent reports.
[[noreturn]] void RunChild(int write_fd,
                           absl::FunctionRef<absl::StatusOr<std::string>()> fn,
                           absl::optional_ref<const Identity> as) {
  if (!WriteFully(write_fd, EncodeChildResult(RunAs(fn, as))).ok()) {
    syscalls::_exit(112);
  }
  syscalls::_exit(0);
}

}  // namespace

absl::StatusOr<std::string> InChild(
    absl::FunctionRef<absl::StatusOr<std::string>()> fn,
    absl::optional_ref<const Identity> as) {
  ASSIGN_OR_RETURN(auto ends, syscalls::socketpair(AF_UNIX, SOCK_STREAM, 0));
  auto &[parent_end, child_end] = ends;
  ASSIGN_OR_RETURN(pid_t pid, syscalls::fork());
  if (pid == 0) RunChild(*child_end, fn, as);
  // With the child's end closed here, the parent's read ends at the child's
  // exit.
  RET_CHECK_OK(child_end.Close());
  absl::StatusOr<std::string> got = testonly::ReadAll(*parent_end);
  RETURN_IF_ERROR(got.status());
  ASSIGN_OR_RETURN(int wait_status, Wait(pid));
  if (!WIFEXITED(wait_status) || WEXITSTATUS(wait_status) != 0) {
    return InternalErrorBuilder()
           << "The child " << testonly::DescribeWaitStatus(wait_status)
           << " before its function returned";
  }
  return DecodeChildMessage(*got);
}

DcfsMount::DcfsMount(MountSpec spec, pid_t pid, std::string log_path)
    : spec_(std::move(spec)), pid_(pid), log_path_(std::move(log_path)) {}

// NOLINTNEXTLINE(bugprone-exception-escape): dcfs does not use exceptions
DcfsMount::~DcfsMount() {
  absl::Status status;
  if (pid_.has_value()) {
    status.Update(syscalls::kill(*pid_, SIGKILL));
    status.Update(Wait(*pid_).status());
    status.Update(
        testonly::RunCommand({"/bin/umount", "-l", spec_.mountpoint}));
  }
  status.Update(syscalls::unlink(log_path_));
  if (!status.ok()) {
    LOG(ERROR) << "Cleaning up the dcfs mount on " << spec_.mountpoint << ": "
               << status;
  }
}

absl::StatusOr<std::unique_ptr<DcfsMount> absl_nonnull> DcfsMount::Spawn(
    MountSpec spec) {
  std::vector<std::string> argv = {
      "mount.dcfs", "-o",
      absl::StrCat("dcfs.cache_db=", spec.cache_db, ",dcfs.foreground")};
  for (const std::string &flag : spec.flags) {
    absl::StrAppend(&argv[2], ",dcfs.", flag);
  }
  argv.push_back(spec.source);
  argv.push_back(spec.mountpoint);

  std::string log_path = "/tmp/dcfs-XXXXXX";
  ASSIGN_OR_RETURN(FileDescriptor log, syscalls::mkstemp(log_path));
  absl::Cleanup remove_log = [&log_path] {
    syscalls::unlink(log_path).IgnoreError();
  };
  ASSIGN_OR_RETURN(pid_t pid, syscalls::fork());
  if (pid == 0) {
    if (!syscalls::dup2(*log, STDOUT_FILENO).ok() ||
        !syscalls::dup2(*log, STDERR_FILENO).ok()) {
      syscalls::_exit(126);
    }
    syscalls::execv(kDcfsBinary, argv).IgnoreError();
    syscalls::_exit(127);
  }
  std::move(remove_log).Cancel();
  return std::unique_ptr<DcfsMount>(
      new DcfsMount(std::move(spec), pid, std::move(log_path)));
}

absl::StatusOr<std::unique_ptr<DcfsMount> absl_nonnull> DcfsMount::Start(
    MountSpec spec) {
  // The table is read before the daemon starts: a change after this read wakes
  // the poll in WaitForMount, whatever the daemon does in between.
  ASSIGN_OR_RETURN(FileDescriptor mounts,
                   syscalls::open("/proc/self/mounts", O_RDONLY));
  ASSIGN_OR_RETURN(std::unique_ptr<DcfsMount> daemon, Spawn(std::move(spec)));
  ASSIGN_OR_RETURN(pid_t pid, daemon->Running());
  ASSIGN_OR_RETURN(FileDescriptor pidfd, syscalls::pidfd_open(pid));
  ASSIGN_OR_RETURN(MountWait waited,
                   WaitForMount(*mounts, *pidfd, daemon->spec_.mountpoint));
  if (waited == MountWait::kMounted) {
    // The mount is in the table before the daemon serves it; a stat is
    // answered once it does.
    RETURN_IF_ERROR(syscalls::stat(daemon->spec_.mountpoint).status());
    return daemon;
  }
  ASSIGN_OR_RETURN(int wait_status, Wait(pid));
  daemon->pid_.reset();
  return daemon->DaemonEnded(
      wait_status, absl::StrCat("without mounting ", daemon->spec_.mountpoint));
}

absl::StatusOr<pid_t> DcfsMount::Running() const {
  if (!pid_.has_value()) {
    return FailedPreconditionErrorBuilder()
           << "The dcfs daemon of " << spec_.mountpoint
           << " was already unmounted or crashed";
  }
  return *pid_;
}

absl::Status DcfsMount::DaemonEnded(int wait_status,
                                    std::string_view when) const {
  ASSIGN_OR_RETURN(std::string log, Log());
  return InternalErrorBuilder()
         << "dcfs " << testonly::DescribeWaitStatus(wait_status) << " " << when
         << "; its stderr: " << log;
}

absl::Status DcfsMount::Unmount() {
  ASSIGN_OR_RETURN(pid_t pid, Running());
  RETURN_IF_ERROR(testonly::RunCommand({"/bin/umount", spec_.mountpoint}));
  ASSIGN_OR_RETURN(int wait_status, Wait(pid));
  pid_.reset();
  if (!WIFEXITED(wait_status) || WEXITSTATUS(wait_status) != 0) {
    return DaemonEnded(wait_status, "at unmount");
  }
  return absl::OkStatus();
}

absl::Status DcfsMount::Crash() {
  ASSIGN_OR_RETURN(pid_t pid, Running());
  RETURN_IF_ERROR(syscalls::kill(pid, SIGKILL));
  RETURN_IF_ERROR(Wait(pid).status());
  pid_.reset();
  return testonly::RunCommand({"/bin/umount", "-l", spec_.mountpoint});
}

absl::StatusOr<std::string> DcfsMount::Log() const {
  return testonly::ReadFileToString(log_path_);
}

std::string TestTmp() {
  return std::string(absl::StripSuffix(::testing::TempDir(), "/"));
}

void Mkdir(std::string_view path) {
  ASSERT_THAT(syscalls::mkdir(path, 0755), IsOk());
}

void Touch(std::string_view path, std::string_view contents) {
  EXPECT_THAT(WriteFile(path, contents), IsOk());
}

absl::Status WriteFile(std::string_view path, std::string_view contents) {
  ASSIGN_OR_RETURN(FileDescriptor fd,
                   syscalls::open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644));
  return WriteFully(*fd, contents);
}

absl::StatusOr<std::vector<std::string>> Snapshot(std::string_view dir) {
  ASSIGN_OR_RETURN(std::vector<std::string> paths, testonly::ListTree(dir));
  std::vector<std::string> lines;
  lines.reserve(paths.size());
  for (const std::string &path : paths) {
    ASSIGN_OR_RETURN(struct stat st,
                     syscalls::fstatat(AT_FDCWD, path, AT_SYMLINK_NOFOLLOW));
    std::string line = absl::Substitute(
        "./$0 $1 $2 $3 $4",
        EscapeBytes(absl::StripPrefix(path, absl::StrCat(dir, "/"))),
        TypeLetter(st.st_mode), st.st_size,
        absl::StrFormat("%o", st.st_mode & 07777), st.st_nlink);
    if (S_ISREG(st.st_mode)) {
      ASSIGN_OR_RETURN(std::string contents, testonly::ReadFileToString(path));
      absl::StrAppend(&line, " ", absl::ComputeCrc32c(contents));
    }
    lines.push_back(std::move(line));
  }
  absl::c_sort(lines);
  return lines;
}

absl::Status CompareTrees(std::string_view a, std::string_view b) {
  ASSIGN_OR_RETURN(std::vector<std::string> left, Snapshot(a));
  ASSIGN_OR_RETURN(std::vector<std::string> right, Snapshot(b));
  std::vector<std::string> only_left;
  std::vector<std::string> only_right;
  absl::c_set_difference(left, right, std::back_inserter(only_left));
  absl::c_set_difference(right, left, std::back_inserter(only_right));
  if (only_left.empty() && only_right.empty()) return absl::OkStatus();
  return InternalErrorBuilder()
         << "The trees differ ('<' only in " << a << ", '>' only in " << b
         << "):\n< " << absl::StrJoin(only_left, "\n< ") << "\n> "
         << absl::StrJoin(only_right, "\n> ");
}

void ExpectSameAsBacking(std::string_view backing, std::string_view served) {
  EXPECT_EQ(backing, served)
      << "backing filesystem: '" << backing << "'; dcfs: '" << served << "'";
}

}  // namespace dcfs
