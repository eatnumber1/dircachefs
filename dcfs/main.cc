#include <cstdlib>
#include <fcntl.h>
#include <iostream>
#include <string>
#include <sys/stat.h>
#include <utility>
#include <vector>

#include "absl/base/log_severity.h"
#include "absl/cleanup/cleanup.h"
#include "absl/flags/flag.h"
#include "absl/flags/parse.h"
#include "absl/flags/usage.h"
#include "absl/log/globals.h"
#include "absl/log/initialize.h"
#include "absl/log/log.h"
#include "absl/random/random.h"
#include "absl/status/status.h"
#include "absl/status/status_builder.h"
#include "absl/status/status_macros.h"
#include "absl/status/statusor.h"
#include "absl/strings/match.h"
#include "absl/strings/numbers.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/str_join.h"
#include "absl/strings/ascii.h"
#include "absl/time/time.h"
#include "dcfs/backing.h"
#include "dcfs/context.h"
#include "dcfs/dir_cache_fs.h"
#include "dcfs/fd.h"
#include "dcfs/fuse_ops.h"
#include "dcfs/migrate.h"
#include "dcfs/mount_fds.h"
#include "dcfs/mounts_below.h"
#include "dcfs/sqlite.h"
#include "dcfs/syscalls.h"
#include "fuse_lowlevel.h"

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
    double, sync_interval_sec, 5,
    "While mutations have left cache entries dirty (see README \"Crash "
    "robustness\"), the first request after this many seconds since the "
    "last sync point syncfs()es the backing filesystems and marks them "
    "clean. Bounds how much is re-read after a power loss or crash.");
ABSL_FLAG(
    bool, foreground, true,
    "Stay in the foreground instead of daemonizing.");
ABSL_FLAG(
    bool, allow_other, false,
    "Pass -o allow_other to the FUSE mount, letting users other than the "
    "one running dcfs access the mountpoint.");
ABSL_FLAG(
    std::vector<std::string>, fuse_opt, {},
    "Comma-separated FUSE/kernel mount options, passed through to libfuse "
    "as \"-o <opts>\" (e.g. --fuse_opt=max_read=65536). Abseil has no "
    "single-dash flag syntax, so -o is spelled --fuse_opt here; repeating "
    "the flag replaces the previous value rather than accumulating "
    "(ordinary Abseil vector<string> flag semantics), so combine several "
    "options in one --fuse_opt=a,b instead of repeating the flag. Our own "
    "default_permissions (and allow_other, when --allow_other is set) are "
    "always added on top of these.");

namespace dcfs {
namespace {

// Prints the usage message set by absl::SetProgramUsageMessage() and
// returns an InvalidArgument status for `message`. Used for command-line
// mistakes (a missing required flag, the wrong number of positional
// arguments) -- as opposed to a valid-looking command line that fails once
// we try to act on it (a bad --source, a foreign cache database, ...),
// where printing the usage banner again would just be noise.
absl::Status UsageError(absl::string_view message) {
  std::cerr << absl::ProgramUsageMessage() << "\n";
  return absl::InvalidArgumentError(message);
}

// The kernel's random per-boot UUID, so StartRun can tell a machine crash
// (a new boot id) from a daemon crash in its log. Read by path: startup is
// the one time dcfs uses paths.
absl::StatusOr<std::string> ReadBootId() {
  constexpr char kPath[] = "/proc/sys/kernel/random/boot_id";
  ABSL_ASSIGN_OR_RETURN(FileDescriptor fd,
                        syscalls::openat(AT_FDCWD, kPath, O_RDONLY));
  std::string buf(64, '\0');
  ABSL_ASSIGN_OR_RETURN(size_t n, syscalls::pread(*fd, buf.data(), buf.size(), 0));
  buf.resize(n);
  return std::string(absl::StripAsciiWhitespace(buf));
}

absl::StatusOr<int> Main(int argc, char *argv[]) {
  // backing.h's MkdirAt/MknodAt/SymlinkAt/Open(O_CREAT) all document that
  // "the kernel applies umask to `mode` before it reaches us, so it is
  // passed straight through" -- true of the FUSE request this process
  // receives, but every one of those backing calls is a real create(2)
  // family syscall this process issues on the real backing filesystem,
  // and the kernel applies *this process's own* umask there too, a second
  // time. Left at whatever this daemon inherited from its launching shell
  // (typically 022), that silently clears bits from an already-final mode
  // -- found via pjdfstest (e.g. open/02.t, open/03.t: `open(..., 0642)`
  // landing as 0640 on the backing file). umask(0) makes this process's
  // own umask a no-op, so a mode already finalized upstream survives
  // verbatim.
  umask(0);
  absl::SetProgramUsageMessage(
      "--source=<dir> --cache_db=<path> [flags] mountpoint");
  // WARNING and above go to stderr by default (Abseil's own default is
  // ERROR), so that e.g. out-of-band changes to the backing filesystem
  // (backing::ReconcileAttrs) are visible in the daemon's log. Set before
  // parsing, so an explicit --stderrthreshold still wins.
  absl::SetStderrThreshold(absl::LogSeverityAtLeast::kWarning);
  std::vector<char *> args = absl::ParseCommandLine(argc, argv);
  absl::InitializeLog();

  if (absl::GetFlag(FLAGS_source).empty()) {
    return UsageError("--source is required");
  }
  std::string cache_db = absl::GetFlag(FLAGS_cache_db);
  if (cache_db.empty()) {
    return UsageError("--cache_db is required");
  }

  // args[0] is the program name; exactly one positional argument (the
  // mountpoint) should remain after flag parsing.
  if (args.size() != 2) {
    return UsageError(
        absl::StrCat(
          "expected exactly one mountpoint argument, got ", args.size() - 1));
  }
  const char *mountpoint = args[1];

  // `source` (the --source path string) is scoped to this block alone: once
  // source_fd is open, every later use of the source filesystem goes
  // through that fd (or objects reopened from cached file handles), never
  // through the path again -- that's what makes mounting dcfs back over
  // --source itself (a supported configuration) safe.
  FileDescriptor source_fd;
  {
    std::string source = absl::GetFlag(FLAGS_source);
    // A real (non-O_PATH) fd: this ends up registered as the source
    // filesystem's mount fd (see backing::InitRoot), and open_by_handle_at's
    // mount fd argument is resolved via the kernel's non-raw fd class
    // (fs/fhandle.c get_path_from_fd()), which rejects O_PATH descriptors
    // with EBADF. O_DIRECTORY also gives a clear ENOTDIR up front if
    // --source isn't a directory.
    absl::StatusOr<FileDescriptor> opened =
        syscalls::openat(AT_FDCWD, source, O_RDONLY | O_DIRECTORY);
    if (!opened.ok()) {
      return absl::StatusBuilder(opened.status())
          << " (--source=" << source << ")";
    }
    source_fd = *std::move(opened);

    // Amendment 12: dcfs requires exactly one backing filesystem below
    // --source (backing inode numbers, shown to users as st_ino, are only
    // unambiguous within one st_dev), so refuse to start if anything is
    // already mounted below it. This is a policy check only -- identity
    // never depends on it -- so it uses the path string, not source_fd; a
    // boundary that appears later (a mount after startup, or a btrfs
    // subvolume, which this check cannot see) is instead refused at
    // runtime (see backing::ProbeChild/PopulateDirectory).
    ABSL_ASSIGN_OR_RETURN(std::vector<std::string> below, MountsBelow(source));
    if (!below.empty()) {
      return absl::FailedPreconditionError(absl::StrCat(
          "dcfs does not yet support filesystems mounted below --source: "
          "their inode numbers would collide under one st_dev; unmount "
          "them or point --source elsewhere. Mounted below ",
          source, ": ", absl::StrJoin(below, ", ")));
    }
  }

  ABSL_ASSIGN_OR_RETURN(
      sqlite3::Connection db, sqlite3::ConnectionFactory{.path = cache_db}.Open());
  MountFds mounts;
  absl::BitGen bitgen;
  Context ctx{db, mounts, bitgen};

  ABSL_ASSIGN_OR_RETURN(RootIdentity root, backing::ProbeRoot(ctx, *source_fd));
  ABSL_RETURN_IF_ERROR(Migrate(db, root));
  // backing::InitRoot() also refuses a cache database built for a different
  // filesystem, but can't be given a --cache_db path or an actionable
  // suggestion to put in its error (it only sees fds, not flags); do that
  // check here instead, before InitRoot, so the message names both.
  ABSL_ASSIGN_OR_RETURN(DeviceId stored_device, GetSourceDeviceId(db));
  if (stored_device != root.device_id) {
    return absl::FailedPreconditionError(absl::StrCat(
        "cache database ", cache_db, " was created for filesystem ",
        stored_device.ToString(), ", but --source is on ",
        root.device_id.ToString(),
        "; delete the database to start a cold cache"));
  }
  // Before anything reads the cache: after an unclean shutdown, forget
  // whatever the dirty set says a power loss may have made wrong.
  ABSL_ASSIGN_OR_RETURN(std::string boot_id, ReadBootId());
  ABSL_RETURN_IF_ERROR(backing::StartRun(ctx, boot_id));
  ABSL_RETURN_IF_ERROR(backing::InitRoot(ctx, std::move(source_fd)));
  ABSL_RETURN_IF_ERROR(backing::StartupPurge(ctx));

  DirCacheFS::Options opts{
      .attr_timeout = absl::Seconds(absl::GetFlag(FLAGS_attr_timeout_sec)),
      .entry_timeout = absl::Seconds(absl::GetFlag(FLAGS_entry_timeout_sec)),
      .sync_interval = absl::Seconds(absl::GetFlag(FLAGS_sync_interval_sec)),
  };

  // default_permissions (and allow_other, if requested) are always added,
  // ahead of whatever the caller passed via --fuse_opt.
  std::vector<std::string> mount_opts = {"default_permissions"};
  if (absl::GetFlag(FLAGS_allow_other)) {
    mount_opts.push_back("allow_other");
  }
  for (const std::string &opt : absl::GetFlag(FLAGS_fuse_opt)) {
    mount_opts.push_back(opt);
    // See DirCacheFS::Options::max_read: DirCacheFS::Init() needs this
    // value too, to satisfy libfuse's do_init() consistency check.
    if (unsigned int max_read;
        absl::StartsWith(opt, "max_read=") &&
        absl::SimpleAtoi(absl::string_view(opt).substr(9), &max_read)) {
      opts.max_read = max_read;
    }
  }

  DirCacheFS fs(ctx, opts);
  std::vector<std::string> fuse_arg_strings = {
      args[0], "-o", absl::StrJoin(mount_opts, ",")};
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

  if (fuse_set_signal_handlers(session) != 0) {
    fuse_session_destroy(session);
    return absl::InternalError("fuse_set_signal_handlers failed");
  }

  if (fuse_session_mount(session, mountpoint) != 0) {
    fuse_remove_signal_handlers(session);
    fuse_session_destroy(session);
    return absl::InternalError(
        absl::StrCat("fuse_session_mount(", mountpoint, ") failed"));
  }

  // Non-zero (the default) keeps this process in the foreground; zero
  // forks to the background. Mounting has already happened by this point,
  // so this is purely about who owns the controlling terminal from here on.
  fuse_daemonize(absl::GetFlag(FLAGS_foreground) ? 1 : 0);

  int rc = fuse_session_loop(session);

  // Shutdown order matters: unmount first, so the kernel stops sending new
  // requests and fusermount's mount table entry is gone, then tear down the
  // session, and only after that sync the backing filesystems, checkpoint,
  // mark the shutdown clean, and close the cache database -- nothing should
  // still be able to write to it once we start closing it.
  fuse_session_unmount(session);
  fuse_remove_signal_handlers(session);
  fuse_session_destroy(session);

  absl::Status finish_status = backing::FinishRun(ctx);
  if (!finish_status.ok()) {
    LOG(WARNING) << "clean shutdown incomplete, the next start will recover "
                    "the dirty set: "
                 << finish_status;
  }
  absl::Status close_status = db.Close();
  if (!close_status.ok()) {
    LOG(WARNING) << "closing cache database: " << close_status;
  }

  // fuse_session_loop() returns 0 when the kernel connection was closed
  // (e.g. the mount was unmounted externally), a positive signal number
  // when fuse_set_signal_handlers()'s handler stopped the loop, or a
  // negative -errno on an actual error -- only the last of those is a
  // failure.
  if (rc < 0) {
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
