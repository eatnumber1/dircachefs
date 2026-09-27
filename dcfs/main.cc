#include <cstdlib>
#include <fcntl.h>
#include <iostream>
#include <string>
#include <utility>
#include <vector>

#include "absl/cleanup/cleanup.h"
#include "absl/flags/flag.h"
#include "absl/flags/parse.h"
#include "absl/flags/usage.h"
#include "absl/log/initialize.h"
#include "absl/log/log.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/str_cat.h"
#include "dcfs/attributes.h"
#include "dcfs/dir_cache_fs.h"
#include "dcfs/fd.h"
#include "dcfs/status.h"
#include "dcfs/syscalls.h"
#include "fuse_lowlevel.h"

// This is a mechanical, minimal skeleton (see the step 0.5 plan): no
// database, no metadata cache, and no `-o` mount-option passthrough yet. It
// only needs to mount, answer stat()/ls on an empty root, and unmount
// cleanly.
ABSL_FLAG(
    std::string, source, "",
    "Directory this filesystem caches. Required.");
ABSL_FLAG(
    bool, foreground, true,
    "Stay in the foreground instead of daemonizing.");

namespace dcfs {
namespace {

absl::StatusOr<int> Main(int argc, char *argv[]) {
  absl::SetProgramUsageMessage("--source=<dir> [flags] mountpoint");
  std::vector<char *> args = absl::ParseCommandLine(argc, argv);
  absl::InitializeLog();

  std::string source = absl::GetFlag(FLAGS_source);
  if (source.empty()) {
    return absl::InvalidArgumentError("--source is required");
  }

  // args[0] is the program name; exactly one positional argument (the
  // mountpoint) should remain after flag parsing.
  if (args.size() != 2) {
    return absl::InvalidArgumentError(
        absl::StrCat(
          "expected exactly one mountpoint argument, got ", args.size() - 1));
  }
  const char *mountpoint = args[1];

  ASSIGN_OR_RETURN(
      FileDescriptor source_fd,
      syscalls::openat(AT_FDCWD, source, O_PATH | O_DIRECTORY));

  DirCacheFS fs(std::move(source_fd));

  // fuse_session_new() requires a non-empty argv (it wants a program name);
  // we don't support any FUSE `-o` style options yet, so pass only argv[0].
  struct fuse_args fuse_args = FUSE_ARGS_INIT(1, args.data());
  absl::Cleanup cleanup_fuse_args = [&fuse_args]() {
    fuse_opt_free_args(&fuse_args);
  };

  struct fuse_lowlevel_ops ops = MakeDirCacheFsOps();
  struct fuse_session *session =
      fuse_session_new(&fuse_args, &ops, sizeof(ops), &fs);
  if (session == nullptr) {
    return absl::InternalError("fuse_session_new failed");
  }
  absl::Cleanup cleanup_session = [session]() {
    fuse_session_destroy(session);
  };

  if (fuse_set_signal_handlers(session) != 0) {
    return absl::InternalError("fuse_set_signal_handlers failed");
  }
  absl::Cleanup cleanup_signal_handlers = [session]() {
    fuse_remove_signal_handlers(session);
  };

  if (fuse_session_mount(session, mountpoint) != 0) {
    return absl::InternalError(
        absl::StrCat("fuse_session_mount(", mountpoint, ") failed"));
  }
  absl::Cleanup cleanup_mount = [session]() {
    fuse_session_unmount(session);
  };

  // Non-zero (the default) keeps this process in the foreground; zero would
  // fork to the background.
  fuse_daemonize(absl::GetFlag(FLAGS_foreground) ? 1 : 0);

  int rc = fuse_session_loop(session);
  if (rc != 0) {
    return absl::InternalError(absl::StrCat("fuse_session_loop: ", rc));
  }
  return EXIT_SUCCESS;
}

}  // namespace
}  // namespace dcfs

int main(int argc, char *argv[]) {
  absl::StatusOr<int> ret = dcfs::Main(argc, argv);
  if (!ret.ok()) {
    std::cerr << ret.status() << std::endl;
    return EXIT_FAILURE;
  }
  return *ret;
}
