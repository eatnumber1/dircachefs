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
#include "absl/status/status_macros.h"
#include "absl/status/statusor.h"
#include "absl/strings/str_cat.h"
#include "absl/time/time.h"
#include "dcfs/backing.h"
#include "dcfs/context.h"
#include "dcfs/dir_cache_fs.h"
#include "dcfs/fd.h"
#include "dcfs/fuse_ops.h"
#include "dcfs/migrate.h"
#include "dcfs/mount_fds.h"
#include "dcfs/sqlite.h"
#include "dcfs/syscalls.h"
#include "fuse_lowlevel.h"

// Step 3.5 finishes the CLI/lifecycle (signal handling nuances, richer `-o`
// passthrough, ...); this is just enough startup wiring to open the cache
// database, bind it to the source filesystem, and mount read-only ops.
ABSL_FLAG(
    std::string, source, "",
    "Directory this filesystem caches. Required.");
ABSL_FLAG(
    std::string, cache_db, "",
    "Path to the sqlite cache database (created if it doesn't exist). "
    "Required.");
ABSL_FLAG(
    double, attr_timeout_sec, 3600,
    "How long the kernel may cache an inode's attributes.");
ABSL_FLAG(
    double, entry_timeout_sec, 3600,
    "How long the kernel may cache a directory entry (lookup result).");
ABSL_FLAG(
    bool, foreground, true,
    "Stay in the foreground instead of daemonizing.");
ABSL_FLAG(
    bool, allow_other, false,
    "Pass -o allow_other to the FUSE mount, letting users other than the "
    "one running dcfs access the mountpoint.");

namespace dcfs {
namespace {

absl::StatusOr<int> Main(int argc, char *argv[]) {
  absl::SetProgramUsageMessage("--source=<dir> --cache_db=<path> [flags] mountpoint");
  std::vector<char *> args = absl::ParseCommandLine(argc, argv);
  absl::InitializeLog();

  std::string source = absl::GetFlag(FLAGS_source);
  if (source.empty()) {
    return absl::InvalidArgumentError("--source is required");
  }
  std::string cache_db = absl::GetFlag(FLAGS_cache_db);
  if (cache_db.empty()) {
    return absl::InvalidArgumentError("--cache_db is required");
  }

  // args[0] is the program name; exactly one positional argument (the
  // mountpoint) should remain after flag parsing.
  if (args.size() != 2) {
    return absl::InvalidArgumentError(
        absl::StrCat(
          "expected exactly one mountpoint argument, got ", args.size() - 1));
  }
  const char *mountpoint = args[1];

  ABSL_ASSIGN_OR_RETURN(
      FileDescriptor source_fd,
      syscalls::openat(AT_FDCWD, source, O_PATH | O_DIRECTORY));

  ABSL_ASSIGN_OR_RETURN(
      sqlite3::Connection db, sqlite3::ConnectionFactory{.path = cache_db}.Open());
  MountFds mounts;
  Context ctx{db, mounts};

  ABSL_ASSIGN_OR_RETURN(RootIdentity root, backing::ProbeRoot(ctx, *source_fd));
  ABSL_RETURN_IF_ERROR(Migrate(db, root));
  // Refuses (FailedPrecondition) a cache database that belongs to a
  // different filesystem than --source.
  ABSL_RETURN_IF_ERROR(backing::InitRoot(ctx, std::move(source_fd)));
  ABSL_RETURN_IF_ERROR(backing::StartupPurge(ctx));

  DirCacheFS::Options opts{
      .attr_timeout = absl::Seconds(absl::GetFlag(FLAGS_attr_timeout_sec)),
      .entry_timeout = absl::Seconds(absl::GetFlag(FLAGS_entry_timeout_sec)),
  };
  DirCacheFS fs(ctx, opts);

  std::vector<std::string> fuse_arg_strings = {args[0], "-o", "default_permissions"};
  if (absl::GetFlag(FLAGS_allow_other)) {
    fuse_arg_strings.push_back("-o");
    fuse_arg_strings.push_back("allow_other");
  }
  std::vector<char *> fuse_arg_ptrs;
  fuse_arg_ptrs.reserve(fuse_arg_strings.size());
  for (std::string &arg : fuse_arg_strings) fuse_arg_ptrs.push_back(arg.data());

  struct fuse_args fuse_args =
      FUSE_ARGS_INIT(static_cast<int>(fuse_arg_ptrs.size()), fuse_arg_ptrs.data());
  absl::Cleanup cleanup_fuse_args = [&fuse_args]() {
    fuse_opt_free_args(&fuse_args);
  };

  struct fuse_lowlevel_ops ops = MakeFuseOps();
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
