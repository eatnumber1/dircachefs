// SPDX-License-Identifier: MIT
#include "test/qemu/guest/guest.h"

#include <fcntl.h>
#include <signal.h>  // NOLINT(modernize-deprecated-headers): the header of SIGKILL
#include <stdio.h>  // NOLINT(modernize-deprecated-headers): SEEK_SET lives here
#include <stdlib.h>  // NOLINT(modernize-deprecated-headers): WIFEXITED lives here
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
#include "absl/container/fixed_array.h"
#include "absl/crc/crc32c.h"
#include "absl/functional/function_ref.h"
#include "absl/status/status.h"
#include "absl/status/status_macros.h"
#include "absl/status/statusor.h"
#include "absl/strings/match.h"
#include "absl/strings/numbers.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/str_format.h"
#include "absl/strings/str_join.h"
#include "absl/strings/str_split.h"
#include "absl/strings/substitute.h"
#include "dcfs/escape.h"
#include "dcfs/fd.h"
#include "dcfs/ret_check.h"
#include "dcfs/status.h"
#include "dcfs/syscalls.h"
#include "dcfs/syscalls_backing.h"
#include "dcfs/syscalls_process.h"
#include "dcfs/testonly/files.h"
#include "gtest/gtest.h"

namespace dcfs {
namespace {

// How a child ended, for a message.
std::string HowEnded(int wait_status) {
  if (WIFEXITED(wait_status)) {
    return absl::StrCat("exited with status ", WEXITSTATUS(wait_status));
  }
  return absl::StrCat("was killed by signal ", WTERMSIG(wait_status));
}

absl::StatusOr<int> Wait(pid_t pid) {
  int wait_status = 0;
  RETURN_IF_ERROR(syscalls::waitpid(pid, &wait_status, 0).status());
  return wait_status;
}

absl::StatusOr<std::string> ReadAll(int fd) {
  std::string out;
  absl::FixedArray<char> buf(4096);
  for (;;) {
    ASSIGN_OR_RETURN(size_t n, syscalls::read(fd, buf.data(), buf.size()));
    if (n == 0) return out;
    out.append(buf.data(), n);
  }
}

// In a child, where there is nobody to tell: a failed write is a child that
// died, which the parent reports.
void WriteAll(int fd, std::string_view data) {
  while (!data.empty()) {
    absl::StatusOr<size_t> n = syscalls::write(fd, data.data(), data.size());
    if (!n.ok()) syscalls::_exit(112);
    data.remove_prefix(*n);
  }
}

absl::Status SetIdentity(const Identity &as) {
  RETURN_IF_ERROR(syscalls::setresgid(as.gid, as.gid, as.gid));
  RETURN_IF_ERROR(syscalls::setgroups(as.groups));
  return syscalls::setresuid(as.uid, as.uid, as.uid);
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

// Appends one line for every entry under `root`, and the path of each regular
// file to hash, in the same order. A directory is listed when it is reached:
// the work list holds the ones still to list.
absl::Status Walk(const std::string &root, std::vector<std::string> &lines,
                  std::vector<std::string> &to_hash) {
  std::vector<std::string> pending = {""};
  while (!pending.empty()) {
    std::string rel = std::move(pending.back());
    pending.pop_back();
    ASSIGN_OR_RETURN(std::vector<std::string> names,
                     testonly::ListDirectory(
                         rel.empty() ? root : absl::StrCat(root, "/", rel)));
    lines.reserve(lines.size() + names.size());
    to_hash.reserve(to_hash.size() + names.size());
    for (const std::string &name : names) {
      std::string entry = rel.empty() ? name : absl::StrCat(rel, "/", name);
      if (entry == "lost+found") continue;
      std::string path = absl::StrCat(root, "/", entry);
      ASSIGN_OR_RETURN(struct stat st,
                       syscalls::fstatat(AT_FDCWD, path, AT_SYMLINK_NOFOLLOW));
      lines.push_back(absl::Substitute(
          "./$0 $1 $2 $3 $4", EscapeBytes(entry),
          std::string(1, TypeLetter(st.st_mode)), st.st_size,
          absl::StrFormat("%o", st.st_mode & 07777), st.st_nlink));
      to_hash.push_back(S_ISREG(st.st_mode) ? path : "");
      if (S_ISDIR(st.st_mode)) pending.push_back(entry);
    }
  }
  return absl::OkStatus();
}

// OK once `mountpoint` is in the table that `mounts` (an open
// /proc/self/mounts) reads; Aborted if the process `pidfd` is of exits first.
// Nothing is waited for on a timer: the kernel wakes the poll when the table
// changes (it compares with the table as last read) or the process exits.
absl::Status WaitForMount(int mounts, int pidfd,
                          const std::string &mountpoint) {
  std::string needle = absl::StrCat(" ", mountpoint, " ");
  for (;;) {
    RETURN_IF_ERROR(syscalls::lseek(mounts, 0, SEEK_SET).status());
    ASSIGN_OR_RETURN(std::string table, ReadAll(mounts));
    if (absl::StrContains(table, needle)) return absl::OkStatus();
    struct pollfd fds[2] = {{.fd = mounts, .events = POLLPRI | POLLERR},
                            {.fd = pidfd, .events = POLLIN}};
    RETURN_IF_ERROR(syscalls::poll(fds, -1).status());
    if ((fds[1].revents & POLLIN) != 0) {
      return AbortedErrorBuilder() << "The process exited";
    }
  }
}

std::string EncodeChildResult(const absl::StatusOr<std::string> &res) {
  if (!res.ok()) {
    int code = static_cast<int>(res.status().code());
    int err = StatusToErrno(res.status());
    int origin = ProducedByDcfs(res.status()) ? 1 : 0;
    return absl::StrFormat("F%d:%d:%d:%s", code, err, origin,
                           res.status().message());
  }
  return absl::StrCat("R", *res);
}

[[noreturn]] void RunChild(int write_fd,
                           absl::FunctionRef<absl::StatusOr<std::string>()> fn,
                           const std::optional<Identity> &as) {
  if (as.has_value()) {
    absl::Status set = SetIdentity(*as);
    if (!set.ok()) {
      WriteAll(write_fd, absl::StrCat("E", set.ToString()));
      syscalls::_exit(0);
    }
  }
  WriteAll(write_fd, EncodeChildResult(fn()));
  syscalls::_exit(0);
}

absl::Status DecodeChildError(std::string_view payload) {
  std::vector<std::string_view> parts =
      absl::StrSplit(payload, absl::MaxSplits(':', 3));
  if (parts.size() == 4) {
    int code_int = 0;
    int err_int = 0;
    int origin_int = 0;
    if (absl::SimpleAtoi(parts[0], &code_int) &&
        absl::SimpleAtoi(parts[1], &err_int) &&
        absl::SimpleAtoi(parts[2], &origin_int)) {
      absl::Status status =
          err_int != 0
              ? ErrnoToStatus(err_int, parts[3])
              : absl::Status(static_cast<absl::StatusCode>(code_int), parts[3]);
      if (origin_int != 0) {
        status = MarkProducedByDcfs(status);
      }
      return status;
    }
  }
  return absl::UnknownError(payload);
}

absl::StatusOr<std::string> DecodeChildMessage(std::string_view got) {
  if (got.empty()) {
    return InternalErrorBuilder()
           << "The child exited without producing an answer";
  }
  char prefix = got.front();
  std::string_view payload = got.substr(1);
  switch (prefix) {
    case 'R':
      return std::string(payload);
    case 'F':
      return DecodeChildError(payload);
    case 'E':
      return InternalErrorBuilder()
             << "The child could not become the user: " << payload;
    default:
      return InternalErrorBuilder()
             << "Unexpected child message prefix: " << prefix;
  }
}

}  // namespace

absl::StatusOr<std::string> InChild(
    absl::FunctionRef<absl::StatusOr<std::string>()> fn,
    const std::optional<Identity> &as) {
  ASSIGN_OR_RETURN(auto ends, syscalls::socketpair(AF_UNIX, SOCK_STREAM, 0));
  auto &[parent_end, child_end] = ends;
  ASSIGN_OR_RETURN(pid_t pid, syscalls::fork());
  if (pid == 0) {
    RunChild(*child_end, fn, as);
  }
  // Closing the child's end here makes the read end at the child's exit.
  absl::Status close_status = child_end.Close();
  absl::StatusOr<std::string> got = ReadAll(*parent_end);
  ASSIGN_OR_RETURN(int wait_status, Wait(pid));
  RETURN_IF_ERROR(close_status);
  RETURN_IF_ERROR(got.status());
  if (!WIFEXITED(wait_status) || WEXITSTATUS(wait_status) != 0) {
    return InternalErrorBuilder() << "The child " << HowEnded(wait_status)
                                  << " before its function returned";
  }
  return DecodeChildMessage(*got);
}

DcfsMount::DcfsMount(MountSpec spec, pid_t pid, std::string log_path)
    : spec_(std::move(spec)), pid_(pid), log_path_(std::move(log_path)) {}

// NOLINTNEXTLINE(bugprone-exception-escape): dcfs does not use exceptions
DcfsMount::~DcfsMount() {
  if (pid_ > 0) {
    syscalls::kill(pid_, SIGKILL).IgnoreError();
    Wait(pid_).IgnoreError();
    if (!spec_.mountpoint.empty()) {
      syscalls::system({"/bin/umount", "-l", spec_.mountpoint}).IgnoreError();
    }
  }
  if (!log_path_.empty()) {
    syscalls::unlink(log_path_).IgnoreError();
  }
}

absl::StatusOr<std::unique_ptr<DcfsMount>> DcfsMount::Start(MountSpec spec) {
  std::string options =
      absl::StrCat("dcfs.cache_db=", spec.cache_db, ",dcfs.foreground");
  for (const std::string &flag : spec.flags) {
    absl::StrAppend(&options, ",dcfs.", flag);
  }
  std::string log_path = "/tmp/dcfs-XXXXXX";
  ASSIGN_OR_RETURN(FileDescriptor log, syscalls::mkstemp(log_path));
  // The table is read before the daemon starts: a change after this read wakes
  // the poll in WaitForMount, whatever the daemon does in between.
  ASSIGN_OR_RETURN(FileDescriptor mounts,
                   syscalls::open("/proc/self/mounts", O_RDONLY));

  ASSIGN_OR_RETURN(pid_t pid, syscalls::fork());
  if (pid == 0) {
    if (!syscalls::dup2(*log, STDOUT_FILENO).ok() ||
        !syscalls::dup2(*log, STDERR_FILENO).ok()) {
      syscalls::_exit(126);
    }
    char *argv[] = {const_cast<char *>("mount.dcfs"),
                    const_cast<char *>("-o"),
                    options.data(),
                    spec.source.data(),
                    spec.mountpoint.data(),
                    nullptr};
    syscalls::execv(std::string(kDcfsBinary).c_str(), argv).IgnoreError();
    syscalls::_exit(127);
  }
  std::unique_ptr<DcfsMount> daemon(
      new DcfsMount(std::move(spec), pid, log_path));
  ASSIGN_OR_RETURN(FileDescriptor pidfd, syscalls::pidfd_open(pid));

  absl::Status mounted =
      WaitForMount(*mounts, *pidfd, daemon->spec_.mountpoint);
  if (mounted.ok()) return daemon;
  if (!absl::IsAborted(mounted)) return mounted;
  // The daemon is gone: what it said is the reason.
  ASSIGN_OR_RETURN(int wait_status, Wait(pid));
  daemon->pid_ = 0;
  auto log_res = daemon->Log();
  return InternalErrorBuilder()
         << "dcfs " << HowEnded(wait_status) << " without mounting "
         << daemon->spec_.mountpoint << "; its stderr: "
         << (log_res.ok() ? *log_res : log_res.status().ToString());
}

absl::Status DcfsMount::Unmount() {
  RET_CHECK_GT(pid_, 0);
  RETURN_IF_ERROR(syscalls::system({"/bin/umount", spec_.mountpoint}));
  ASSIGN_OR_RETURN(int wait_status, Wait(pid_));
  pid_ = 0;
  if (!WIFEXITED(wait_status) || WEXITSTATUS(wait_status) != 0) {
    auto log_res = Log();
    return InternalErrorBuilder()
           << "dcfs " << HowEnded(wait_status) << " at unmount; its stderr: "
           << (log_res.ok() ? *log_res : log_res.status().ToString());
  }
  return absl::OkStatus();
}

absl::Status DcfsMount::Crash() {
  RET_CHECK_GT(pid_, 0);
  RETURN_IF_ERROR(syscalls::kill(pid_, SIGKILL));
  RETURN_IF_ERROR(Wait(pid_).status());
  pid_ = 0;
  return syscalls::system({"/bin/umount", "-l", spec_.mountpoint});
}

absl::StatusOr<std::string> DcfsMount::Log() const {
  return testonly::ReadFileToString(log_path_);
}

absl::Status WriteFile(std::string_view path, std::string_view contents) {
  ASSIGN_OR_RETURN(FileDescriptor fd,
                   syscalls::open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644));
  while (!contents.empty()) {
    ASSIGN_OR_RETURN(size_t n,
                     syscalls::write(*fd, contents.data(), contents.size()));
    contents.remove_prefix(n);
  }
  return absl::OkStatus();
}

absl::StatusOr<std::vector<std::string>> Snapshot(std::string_view dir) {
  std::string root(dir);
  std::vector<std::string> lines;
  std::vector<std::string> to_hash;
  RETURN_IF_ERROR(Walk(root, lines, to_hash));
  // The attributes were all read first: reading a file moves its atime.
  for (size_t i = 0; i < lines.size(); ++i) {
    if (to_hash[i].empty()) continue;
    ASSIGN_OR_RETURN(FileDescriptor fd,
                     syscalls::open(to_hash[i], O_RDONLY | O_NOATIME));
    ASSIGN_OR_RETURN(std::string contents, ReadAll(*fd));
    absl::StrAppend(&lines[i], " ", absl::ComputeCrc32c(contents));
  }
  absl::c_sort(lines);
  return lines;
}

absl::Status ExpectSameTree(std::string_view a, std::string_view b) {
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

void ExpectSameAsBacking(std::string_view what, const std::string &backing,
                         const std::string &served) {
  EXPECT_EQ(backing, served) << what << ": backing filesystem: '" << backing
                             << "'; dcfs: '" << served << "'";
}

}  // namespace dcfs
