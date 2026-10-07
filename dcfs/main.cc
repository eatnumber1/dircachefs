#include <cerrno>
#include <cstdlib>
#include <fcntl.h>
#include <iostream>
#include <string>
#include <sys/file.h>
#include <sys/stat.h>
#include <utility>
#include <vector>

#include "absl/base/log_severity.h"
#include "absl/cleanup/cleanup.h"
#include "absl/flags/flag.h"
#include "absl/flags/parse.h"
#include "absl/flags/usage_config.h"
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
#include "absl/strings/str_format.h"
#include "absl/strings/str_join.h"
#include "absl/strings/string_view.h"
#include "absl/strings/ascii.h"
#include "absl/time/time.h"
#include "dcfs/backing.h"
#include "dcfs/context.h"
#include "dcfs/dir_cache_fs.h"
#include "dcfs/fd.h"
#include "dcfs/file_handle.h"
#include "dcfs/metadata_cache.h"
#include "dcfs/fuse_ops.h"
#include "dcfs/migrate.h"
#include "dcfs/mount_fds.h"
#include "dcfs/mounts_below.h"
#include "dcfs/protocol_events.h"
#include "dcfs/sqlite.h"
#include "dcfs/status.h"
#include "dcfs/syscalls.h"
#include "dcfs/version.h"
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

// The cache files must grant no access beyond what the backing root
// directory (--source's root) grants: the owner must be root or the backing
// root's owner; group read/write only if the group is the directory's group
// and the directory grants its group read/write respectively; other
// read/write only if the directory grants others the same. A violation is
// an error naming both permission sets. (Creation is 0600, which always
// passes.)
absl::Status CheckNoMoreAccessThanRoot(const std::string &path,
                                       const struct stat &st,
                                       const struct stat &root) {
  std::string reason;
  const mode_t m = st.st_mode;
  const mode_t rm = root.st_mode;
  if (st.st_uid != 0 && st.st_uid != root.st_uid) {
    reason = absl::StrCat("owned by uid ", st.st_uid,
                          ", which is neither root nor the backing root's "
                          "owner");
  } else if ((m & (S_IRGRP | S_IWGRP)) != 0 && st.st_gid != root.st_gid) {
    reason = absl::StrCat("grants group access to gid ", st.st_gid,
                          ", not the backing root's group");
  } else if ((m & S_IRGRP) != 0 && (rm & S_IRGRP) == 0) {
    reason = "grants group read the backing root does not";
  } else if ((m & S_IWGRP) != 0 && (rm & S_IWGRP) == 0) {
    reason = "grants group write the backing root does not";
  } else if ((m & S_IROTH) != 0 && (rm & S_IROTH) == 0) {
    reason = "grants others read the backing root does not";
  } else if ((m & S_IWOTH) != 0 && (rm & S_IWOTH) == 0) {
    reason = "grants others write the backing root does not";
  }
  if (reason.empty()) return absl::OkStatus();
  return absl::FailedPreconditionError(absl::StrCat(
      path, " grants more access than the backing root directory (", reason,
      "): ", path, " is mode ", absl::StrFormat("0%o", m & 07777), " uid ",
      st.st_uid, " gid ", st.st_gid, "; the backing root is mode ",
      absl::StrFormat("0%o", rm & 07777), " uid ", root.st_uid, " gid ",
      root.st_gid));
}

// Opens `path` -- the cache database itself, or one of its -wal/-shm
// companions -- hardened against a hostile cache directory (one writable
// by, or already containing files from, another local user): O_NOFOLLOW so
// a symlink left in `path`'s place is never followed (ELOOP, turned into a
// clear refusal below; the link's target is never touched, since it's
// never opened), plus a check that whatever is actually there is a plain
// file owned by root -- not one another user pre-created to read or tamper
// with the cache, or to have dcfs write through as root.
//
// If `create`, a missing file is created mode 0600 (O_CREAT); otherwise a
// missing file is not an error at all -- an invalid (unopened)
// FileDescriptor is returned -- since this is also used to check an
// existing -wal/-shm before SQLite gets a chance to open them, and most of
// the time there won't be one yet (SQLite creates them itself, inheriting
// the main file's mode, already fixed up by the time SQLite opens it).
//
// Existing files must also pass CheckNoMoreAccessThanRoot against
// `backing_root` (the --source root directory's stat), else startup is
// refused.
//
// An existing file passing that but not mode 0600 -- a cache
// database (or -wal/-shm) made before this check existed -- is fchmod'd to
// 0600, with a WARNING; this is the only case that changes something
// instead of refusing to start.
absl::StatusOr<FileDescriptor> OpenHardenedCacheFile(
    const std::string &path, bool create, const struct stat &backing_root) {
  int flags = O_RDWR | O_CLOEXEC | O_NOFOLLOW;
  if (create) flags |= O_CREAT;
  int fd = ::open(path.c_str(), flags, 0600);
  if (fd == -1) {
    if (!create && errno == ENOENT) {
      return FileDescriptor();
    }
    if (errno == ELOOP) {
      return absl::FailedPreconditionError(absl::StrCat(
          path,
          " is a symlink; refusing to open a cache file through a symlink "
          "(another local user could have pointed it anywhere)"));
    }
    return dcfs::ErrnoToStatus(errno, absl::StrCat("open ", path));
  }
  FileDescriptor result(fd);
  ABSL_ASSIGN_OR_RETURN(struct stat st, syscalls::fstat(*result));
  if (!S_ISREG(st.st_mode)) {
    return absl::FailedPreconditionError(
        absl::StrCat(path, " is not a regular file"));
  }
  ABSL_RETURN_IF_ERROR(CheckNoMoreAccessThanRoot(path, st, backing_root));
  if ((st.st_mode & 07777) != 0600) {
    ABSL_RETURN_IF_ERROR(syscalls::fchmod(*result, 0600));
    LOG(WARNING) << path << " was mode "
                 << absl::StrFormat("0%o", st.st_mode & 07777)
                 << "; tightened to 0600 (it holds cache data as sensitive "
                    "as --source)";
  }
  return result;
}

// Abseil's --help lists only flags defined in files it calls "main" files,
// by default those named after the program (dcfs.cc); our flags live in
// dcfs/*.cc, so under the installed name `dcfs` none would match. Claim
// every file under a dcfs/ directory instead.
bool IsDcfsFlagFile(absl::string_view filename) {
  return absl::StartsWith(filename, "dcfs/") ||
         absl::StrContains(filename, "/dcfs/");
}

void InstallFlagsUsageConfig() {
  absl::FlagsUsageConfig config;
  config.contains_help_flags = IsDcfsFlagFile;
  config.contains_helpshort_flags = IsDcfsFlagFile;
  config.version_string = [] { return absl::StrCat("dcfs ", kVersion, "\n"); };
  absl::SetFlagsUsageConfig(config);
}

absl::StatusOr<int> Main(int argc, char *argv[]) {
  // The backing create(2)-family syscalls (backing.h's MkdirAt/MknodAt/
  // CreateAt) run with the caller's umask, switched to around each one
  // (AsCaller; the kernel sends it with the request, see
  // FUSE_CAP_DONT_MASK in DirCacheFS::Init). The daemon's own umask is set
  // to 0 so that whatever it inherited from its launching shell (typically
  // 022) can never alter a mode on the backing filesystem -- found via
  // pjdfstest before the per-request umask existed (open/02.t, open/03.t:
  // `open(..., 0642)` landing as 0640 on the backing file).
  umask(0);
  InstallFlagsUsageConfig();
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
  struct stat backing_root;  // the --source root directory, for the cache
                             // files' permission check
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
    ABSL_ASSIGN_OR_RETURN(backing_root, syscalls::fstat(*source_fd));

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

  // The cache database holds metadata as sensitive as the backing tree's --
  // every cached name, attribute, xattr and symlink target, including those
  // of directories a reader cannot list -- so its directory must not be
  // readable by anyone but root. Create a missing one 0700; warn (but still
  // start) about an existing one that is group- or world-accessible. Done
  // with plain path-based calls, like the --cache_db open just below: this
  // is the one place dcfs still deals in a path for something other than
  // --source.
  {
    size_t slash = cache_db.find_last_of('/');
    if (slash != std::string::npos) {
      std::string parent = slash == 0 ? "/" : cache_db.substr(0, slash);
      struct stat st;
      if (::stat(parent.c_str(), &st) == -1) {
        if (errno != ENOENT) {
          return dcfs::ErrnoToStatus(errno, absl::StrCat("stat ", parent));
        }
        if (::mkdir(parent.c_str(), 0700) == -1) {
          return dcfs::ErrnoToStatus(
              errno, absl::StrCat("creating cache database directory ",
                                   parent));
        }
      } else if ((st.st_mode & (S_IRWXG | S_IRWXO)) != 0) {
        LOG(WARNING) << "cache database directory " << parent
                     << " is group- or world-accessible (mode "
                     << absl::StrFormat("0%o", st.st_mode & 07777)
                     << "); it holds a cache as sensitive as --source and "
                        "should be readable only by root (mode 0700)";
      }
    }
  }

  // One daemon per cache database (audit-crash F7): every write-through
  // mutation's three phases, and the in-memory state that goes with them
  // (writable opens, fill guards), assume this process is the only writer;
  // SQLite would serialize two daemons' transactions but not their
  // protocols. An exclusive flock(2) on the database file itself, held for
  // the life of the process (the kernel drops it when the process dies,
  // however it dies), makes a second daemon refuse to start. It does not
  // interfere with SQLite's own locking, which uses fcntl(2) locks.
  //
  // OpenHardenedCacheFile (see its own comment) both creates it 0600 (not
  // 0644: see the directory comment above) and refuses a hostile existing
  // one (a symlink, or a file some other user owns).
  FileDescriptor db_lock;
  {
    ABSL_ASSIGN_OR_RETURN(db_lock,
                          OpenHardenedCacheFile(cache_db, /*create=*/true, backing_root));
    if (::flock(*db_lock, LOCK_EX | LOCK_NB) == -1) {
      if (errno == EWOULDBLOCK) {
        return absl::FailedPreconditionError(absl::StrCat(
            "cache database ", cache_db,
            " is in use by another dcfs process; two daemons cannot share "
            "one cache database"));
      }
      return dcfs::ErrnoToStatus(errno,
                                 absl::StrCat("flock --cache_db=", cache_db));
    }
  }

  // SQLite gives a -wal/-shm file it creates itself the main database
  // file's mode, but only when it creates them: one left over from a cache
  // database made before this check existed (or, as above, pre-created by
  // another user as a symlink or a file they own) needs the same hardening
  // as the main file, checked here -- before ConnectionFactory::Open()
  // below gives SQLite a chance to open it instead.
  for (const char *suffix : {"-wal", "-shm"}) {
    ABSL_ASSIGN_OR_RETURN(
        FileDescriptor companion,
        OpenHardenedCacheFile(absl::StrCat(cache_db, suffix),
                               /*create=*/false, backing_root));
    // Nothing more to do with it than the check (and the possible fchmod)
    // OpenHardenedCacheFile just did: SQLite opens the real thing itself.
  }

  ABSL_ASSIGN_OR_RETURN(
      sqlite3::Connection db, sqlite3::ConnectionFactory{.path = cache_db}.Open());
  MountFds mounts;
  absl::BitGen bitgen;
  Context ctx{db, mounts, bitgen};
  // Records nothing, except in the testonly recording build (see
  // dcfs/protocol_events.h).
  ctx.events = &MainProtocolEvents();

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
  // Nor one built for another directory on the same filesystem (audit-crash
  // F4): --source pointed elsewhere, or the source directory replaced while
  // dcfs was down. The root row keeps the backing identity it was created
  // with (Migrate seeds it from ProbeRoot; InitRoot adds the handle), and
  // reusing it would serve the old directory's cached tree -- whose handles
  // still decode on this filesystem -- under the new one.
  {
    ABSL_ASSIGN_OR_RETURN(cache::CachedAttr stored_root,
                          cache::GetAttr(ctx, cache::kRootInode));
    absl::StatusOr<FileHandle> stored_handle =
        cache::GetHandle(ctx, cache::kRootInode);
    if (!stored_handle.ok() && !absl::IsNotFound(stored_handle.status())) {
      return stored_handle.status();
    }
    ABSL_ASSIGN_OR_RETURN(FileHandle handle,
                          FileHandle::FromFd(*source_fd, root.device_id));
    const bool same =
        stored_root.backing_ino == root.backing_ino &&
        (stored_root.backing_gen == 0 || root.backing_gen == 0 ||
         stored_root.backing_gen == root.backing_gen) &&
        (!stored_handle.ok() || *stored_handle == handle);
    if (!same) {
      return absl::FailedPreconditionError(absl::StrCat(
          "cache database ", cache_db,
          " was created for a different source directory (inode ",
          stored_root.backing_ino, ", generation ", stored_root.backing_gen,
          ") than --source (inode ", root.backing_ino, ", generation ",
          root.backing_gen, "); delete the database to start a cold cache"));
    }
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
