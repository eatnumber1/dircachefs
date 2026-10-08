#include "dcfs/backing_capture.h"

#include <fcntl.h>
#include <sched.h>
#include <sys/mount.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

#include "absl/cleanup/cleanup.h"
#include "absl/status/status.h"
#include "absl/status/status_macros.h"
#include "absl/status/statusor.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/str_join.h"
#include "dcfs/escape.h"
#include "dcfs/fd.h"
#include "dcfs/mount_dcfs.h"
#include "dcfs/status.h"
#include "dcfs/syscalls.h"
#include "dcfs/syscalls_backing.h"
#include "dcfs/syscalls_process.h"

namespace dcfs {
namespace {

// Where the native mount is made, in the helper's private namespace: a
// directory in the caller's /tmp, removed once the helper is gone (the mount
// on it never existed outside the helper).
constexpr char kStagingPattern[] = "/tmp/dcfs-capture.XXXXXX";
// mount(8) is run by this path (no PATH lookup of the daemon's: the wrapper
// runs as root); /bin is a symlink to /usr/bin on merged-usr systems.
constexpr char kMountProgram[] = "/bin/mount";
// The helper's answer, one message: 'O' with the descriptor as SCM_RIGHTS;
// 'N', mount(8)'s exit status (a byte) and its text; or 'F' and the text of
// any other failure.
constexpr char kAnswerOk = 'O';
constexpr char kAnswerFailed = 'F';
// A mount(8) failure: the status byte is mount's own.
constexpr char kAnswerNative = 'N';
// The most mount(8) text kept.
constexpr size_t kMaxText = 3000;

// Everything `fd` yields until its end, up to kMaxText bytes (the rest is
// read and dropped).
std::string ReadText(int fd) {
  std::string text;
  char buf[1024];
  while (true) {
    absl::StatusOr<size_t> n = syscalls::read(fd, buf, sizeof(buf));
    if (!n.ok() || *n == 0) break;
    if (text.size() < kMaxText) {
      text.append(buf, std::min(*n, kMaxText - text.size()));
    }
  }
  while (!text.empty() && (text.back() == '\n' || text.back() == ' ')) {
    text.pop_back();
  }
  return text;
}

// Runs mount(8) with `command`, its output (stdout and stderr) collected.
// Returns OK, or a NativeMountError with mount's own status and text.
absl::Status RunNativeMount(const std::vector<std::string> &command,
                            const CaptureRequest &request) {
  ABSL_ASSIGN_OR_RETURN(auto output,
                        syscalls::socketpair(AF_UNIX, SOCK_STREAM, 0));
  auto &[read_end, write_end] = output;
  ABSL_ASSIGN_OR_RETURN(pid_t child, syscalls::fork());
  if (child == 0) {
    // dup2 clears close-on-exec on the new descriptors; the original
    // socket ends close at exec.
    if (!syscalls::dup2(*write_end, STDOUT_FILENO).ok() ||
        !syscalls::dup2(*write_end, STDERR_FILENO).ok()) {
      syscalls::_exit(126);
    }
    std::vector<char *> argv;
    argv.reserve(command.size() + 1);
    for (const std::string &arg : command) {
      argv.push_back(const_cast<char *>(arg.c_str()));
    }
    argv.push_back(nullptr);
    syscalls::execv(kMountProgram, argv.data()).IgnoreError();
    syscalls::_exit(127);
  }
  write_end.Close().IgnoreError();
  const std::string text = ReadText(*read_end);
  int wait_status = 0;
  ABSL_RETURN_IF_ERROR(syscalls::waitpid(child, &wait_status, 0).status())
      << "while waiting for " << kMountProgram;
  if (WIFEXITED(wait_status) && WEXITSTATUS(wait_status) == 0) {
    return absl::OkStatus();
  }
  std::string what;
  int exit_status = 32;
  if (WIFSIGNALED(wait_status)) {
    what = absl::StrCat("was killed by signal ", WTERMSIG(wait_status));
  } else {
    exit_status = WEXITSTATUS(wait_status);
    what = absl::StrCat("failed (exit status ", exit_status, ")");
    if (exit_status == 127) what = absl::StrCat("could not be run");
  }
  return NativeMountError(
      exit_status, absl::StrCat("Mounting ", EscapeBytes(request.source), " ",
                                what, ": ", text));
}

// The helper's work, in its own mount namespace: the native mount on
// `staging`, then the detached clone of it.
absl::StatusOr<FileDescriptor> MountAndClone(const CaptureRequest &request,
                                             const std::string &staging) {
  ABSL_RETURN_IF_ERROR(syscalls::unshare(CLONE_NEWNS));
  // Nothing mounted here may propagate out, and the staging mount must not
  // be shared back with the parent namespace's peers.
  ABSL_RETURN_IF_ERROR(
      syscalls::mount(nullptr, "/", nullptr, MS_REC | MS_PRIVATE, nullptr));
  ABSL_RETURN_IF_ERROR(
      RunNativeMount(NativeMountCommand(request, staging), request));
  return syscalls::open_tree(AT_FDCWD, staging,
                             OPEN_TREE_CLONE | OPEN_TREE_CLOEXEC);
}

// Sends the helper's answer; never returns.
[[noreturn]] void AnswerAndExit(int channel,
                                const absl::StatusOr<FileDescriptor> &result) {
  std::string packet;
  char control[CMSG_SPACE(sizeof(int))];
  std::memset(control, 0, sizeof(control));
  struct msghdr message = {};
  struct iovec iov = {};
  message.msg_iov = &iov;
  message.msg_iovlen = 1;
  if (result.ok()) {
    packet = std::string(1, kAnswerOk);
    message.msg_control = control;
    message.msg_controllen = sizeof(control);
    struct cmsghdr *cmsg = CMSG_FIRSTHDR(&message);
    cmsg->cmsg_level = SOL_SOCKET;
    cmsg->cmsg_type = SCM_RIGHTS;
    cmsg->cmsg_len = CMSG_LEN(sizeof(int));
    const int fd = **result;
    std::memcpy(CMSG_DATA(cmsg), &fd, sizeof(fd));
  } else if (result.status().GetPayload(kMountExitStatusTypeUrl).has_value()) {
    packet = std::string(1, kAnswerNative);
    packet.push_back(static_cast<char>(ExitStatusFor(result.status())));
    packet += result.status().message();
  } else {
    packet = std::string(1, kAnswerFailed);
    packet += result.status().ToString();
  }
  iov.iov_base = packet.data();
  iov.iov_len = packet.size();
  syscalls::sendmsg(channel, message, MSG_NOSIGNAL).status().IgnoreError();
  syscalls::_exit(0);
}

// The helper's answer, read by the wrapper.
absl::StatusOr<FileDescriptor> ReceiveAnswer(int channel) {
  char buf[kMaxText + 64];
  struct iovec iov = {.iov_base = buf, .iov_len = sizeof(buf)};
  char control[CMSG_SPACE(sizeof(int))];
  struct msghdr message = {};
  message.msg_iov = &iov;
  message.msg_iovlen = 1;
  message.msg_control = control;
  message.msg_controllen = sizeof(control);
  ABSL_ASSIGN_OR_RETURN(size_t n, syscalls::recvmsg(channel, message, 0));
  if (n == 0) {
    return InternalErrorBuilder()
           << "The mount helper exited without an answer";
  }
  if (buf[0] == kAnswerOk) {
    struct cmsghdr *cmsg = CMSG_FIRSTHDR(&message);
    if (cmsg == nullptr || cmsg->cmsg_level != SOL_SOCKET ||
        cmsg->cmsg_type != SCM_RIGHTS) {
      return InternalErrorBuilder()
             << "The mount helper answered without a descriptor";
    }
    int fd = -1;
    std::memcpy(&fd, CMSG_DATA(cmsg), sizeof(fd));
    return FileDescriptor(fd);
  }
  if (buf[0] == kAnswerNative && n >= 2) {
    return NativeMountError(static_cast<unsigned char>(buf[1]),
                            std::string(buf + 2, n - 2));
  }
  if (buf[0] == kAnswerFailed) {
    return FailedPreconditionErrorBuilder() << std::string(buf + 1, n - 1);
  }
  return InternalErrorBuilder() << "The mount helper's answer made no sense";
}

}  // namespace

std::vector<std::string> NativeMountCommand(const CaptureRequest &request,
                                            const std::string &staging) {
  std::vector<std::string> command = {"mount", "-n"};
  if (request.sloppy) command.push_back("-s");
  if (request.verbose) command.push_back("-v");
  std::vector<std::string> options;
  if (request.bind) {
    options.push_back("bind");
  } else if (!request.native_type.empty()) {
    command.push_back("-t");
    command.push_back(request.native_type);
  }
  options.insert(options.end(), request.options.begin(),
                 request.options.end());
  if (!options.empty()) {
    command.push_back("-o");
    command.push_back(absl::StrJoin(options, ","));
  }
  command.push_back(request.source);
  command.push_back(staging);
  return command;
}

absl::StatusOr<CapturedTree> CaptureBacking(const CaptureRequest &request) {
  ABSL_ASSIGN_OR_RETURN(std::string staging,
                        syscalls::mkdtemp(kStagingPattern));
  absl::Cleanup remove_staging = [&staging] {
    syscalls::unlinkat(AT_FDCWD, staging, AT_REMOVEDIR).IgnoreError();
  };
  ABSL_ASSIGN_OR_RETURN(auto channel,
                        syscalls::socketpair(AF_UNIX, SOCK_STREAM, 0));
  auto &[helper_end, mine] = channel;
  ABSL_ASSIGN_OR_RETURN(pid_t helper, syscalls::fork());
  if (helper == 0) {
    mine.Close().IgnoreError();
    AnswerAndExit(*helper_end, MountAndClone(request, staging));
  }
  helper_end.Close().IgnoreError();
  absl::StatusOr<FileDescriptor> tree = ReceiveAnswer(*mine);
  int wait_status = 0;
  syscalls::waitpid(helper, &wait_status, 0).status().IgnoreError();
  ABSL_RETURN_IF_ERROR(tree.status());
  // A real directory descriptor on the clone's root: open_by_handle_at's
  // mount descriptor is resolved as a regular file, which rejects O_PATH.
  ABSL_ASSIGN_OR_RETURN(
      FileDescriptor root,
      syscalls::openat(**tree, ".", O_RDONLY | O_DIRECTORY | O_CLOEXEC),
      _ << "while opening the captured filesystem of "
        << EscapeBytes(request.source));
  return CapturedTree{.root = std::move(root), .tree = *std::move(tree)};
}

}  // namespace dcfs
