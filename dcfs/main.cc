#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <sys/file.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <utility>
#include <vector>

#include "absl/base/log_severity.h"
#include "absl/cleanup/cleanup.h"
#include "absl/flags/flag.h"
#include "absl/flags/parse.h"
#include "absl/flags/reflection.h"
#include "absl/flags/usage_config.h"
#include "absl/flags/usage.h"
#include "absl/log/globals.h"
#include "absl/log/log_entry.h"
#include "absl/log/log_sink.h"
#include "absl/log/initialize.h"
#include "absl/log/log_sink_registry.h"
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
#include "absl/strings/str_replace.h"
#include "absl/strings/string_view.h"
#include "absl/strings/ascii.h"
#include "absl/time/time.h"
#include "dcfs/absolute_paths.h"
#include "dcfs/backing.h"
#include "dcfs/backing_capture.h"
#include "dcfs/context.h"
#include "dcfs/dir_cache_fs.h"
#include "dcfs/fd.h"
#include "dcfs/mount_dcfs.h"
#include "dcfs/mount_options.h"
#include "dcfs/remount.h"
#include "dcfs/file_handle.h"
#include "dcfs/metadata_cache.h"
#include "dcfs/fuse_ops.h"
#include "dcfs/migrate.h"
#include "dcfs/mount_fds.h"
#include "dcfs/mounts_below.h"
#include "dcfs/protocol_events.h"
#include "dcfs/session_loop.h"
#include "dcfs/startup_channel.h"
#include "dcfs/sqlite.h"
#include "dcfs/status.h"
#include "dcfs/syscalls.h"
#include "dcfs/syscalls_backing.h"
#include "dcfs/syslog_sink.h"
#include "dcfs/umount_helper.h"
#include "dcfs/version.h"
#include "fuse_lowlevel.h"

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
    bool, allow_other, false,
    "Pass -o allow_other to the FUSE mount, letting users other than the "
    "one running dcfs access the mountpoint. (A mount option: "
    "dcfs.allow_other.)");

namespace dcfs {
namespace {

// A command-line mistake of the plain `dcfs` binary (everything else is
// started as mount.dcfs): the usage message, then the error.
absl::Status UsageError(std::string_view message) {
  std::cerr << absl::ProgramUsageMessage() << "\n";
  return InvalidArgumentErrorBuilder() << message;
}

// A /proc/sys value (its first 64 bytes), without
// surrounding whitespace. Read by path: startup is the one time dcfs uses
// paths.
absl::StatusOr<std::string> ReadProcValue(const char *path) {
  ABSL_ASSIGN_OR_RETURN(FileDescriptor fd,
                        syscalls::openat(AT_FDCWD, path, O_RDONLY));
  std::string buf(64, '\0');
  ABSL_ASSIGN_OR_RETURN(size_t n, syscalls::pread(*fd, buf.data(), buf.size(), 0));
  buf.resize(n);
  return std::string(absl::StripAsciiWhitespace(buf));
}

// The kernel's random per-boot UUID, so StartRun can tell a machine crash
// (a new boot id) from a daemon crash in its log. Read by path: startup is
// the one time dcfs uses paths.
absl::StatusOr<std::string> ReadBootId() {
  return ReadProcValue("/proc/sys/kernel/random/boot_id");
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
  return FailedPreconditionErrorBuilder()
         << path << " grants more access than the backing root directory ("
         << reason << "): " << path << " is mode "
         << absl::StrFormat("0%o", m & 07777) << " uid " << st.st_uid
         << " gid " << st.st_gid << "; the backing root is mode "
         << absl::StrFormat("0%o", rm & 07777) << " uid " << root.st_uid
         << " gid " << root.st_gid;
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
  absl::StatusOr<FileDescriptor> opened =
      syscalls::openat(AT_FDCWD, path, flags, 0600);
  if (!opened.ok()) {
    const int err = StatusToErrno(opened.status());
    if (!create && err == ENOENT) {
      return FileDescriptor();
    }
    if (err == ELOOP) {
      return FailedPreconditionErrorBuilder()
             << path
             << " is a symlink; refusing to open a cache file through a "
                "symlink (another local user could have pointed it anywhere)";
    }
    return opened.status();
  }
  FileDescriptor result = *std::move(opened);
  ABSL_ASSIGN_OR_RETURN(struct stat st, syscalls::fstat(*result));
  if (!S_ISREG(st.st_mode)) {
    return FailedPreconditionErrorBuilder() << path << " is not a regular file";
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
bool IsDcfsFlagFile(std::string_view filename) {
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

// Raises RLIMIT_NOFILE as far as the kernel allows (fs.nr_open, which root
// may also set as the hard limit). DirCacheFS holds one descriptor per file
// written during the run until the kernel forgets the file -- design.md
// "mmap after close" (held-fd workaround) -- as many as the kernel's inode
// cache keeps, which the default soft limit of 1024 would cap. A failure
// only means fewer held descriptors (those files' FORGET then marks their
// attributes unknown instead), so it is logged, not fatal.
void RaiseFileLimit() {
  absl::StatusOr<struct rlimit> current = syscalls::getrlimit(RLIMIT_NOFILE);
  if (!current.ok()) {
    LOG(WARNING) << current.status();
    return;
  }
  const struct rlimit limit = *current;
  rlim_t want = limit.rlim_max;
  if (absl::StatusOr<std::string> value = ReadProcValue("/proc/sys/fs/nr_open");
      value.ok()) {
    uint64_t nr_open = 0;
    if (absl::SimpleAtoi(*value, &nr_open) && nr_open > want) {
      want = static_cast<rlim_t>(nr_open);
    }
  } else {
    LOG(WARNING) << "fs.nr_open: " << value.status();
  }
  struct rlimit raised = {.rlim_cur = want, .rlim_max = want};
  absl::Status first = syscalls::setrlimit(RLIMIT_NOFILE, raised);
  if (first.ok()) return;
  // Not to fs.nr_open (no CAP_SYS_RESOURCE, as in a container): as far
  // as the hard limit allows, and say so either way, since the held
  // descriptors' cap (DirCacheFS::DefaultMaxHeldFds) follows from it.
  raised = {.rlim_cur = limit.rlim_max, .rlim_max = limit.rlim_max};
  absl::Status second = syscalls::setrlimit(RLIMIT_NOFILE, raised);
  if (second.ok()) {
    LOG(WARNING) << "could not raise RLIMIT_NOFILE to fs.nr_open (" << want
                 << "): " << first << "; raised the soft limit "
                 << "to the hard limit, " << limit.rlim_max;
  } else {
    LOG(WARNING) << "could not raise RLIMIT_NOFILE (soft " << limit.rlim_cur
                 << ", hard " << limit.rlim_max << ") to fs.nr_open (" << want
                 << "): " << first << ", nor to the hard limit: " << second;
  }
}

// Everything one mount needs, as the wrapper parsed it.
struct MountRequest {
  const HelperArgs &args;
  const HelperOptions &options;
  // Told when dcfs answers FUSE_INIT, or fails to start.
  StartupReporter &reporter;
};

// The text of the last ERROR logged while it lives: DirCacheFS::Init logs
// why it refuses the kernel's FUSE_INIT (libfuse gives it no way to say so),
// and the wrapper should print that, not just "fuse_session_loop: -71".
class LastErrorSink : public absl::LogSink {
 public:
  LastErrorSink() { absl::AddLogSink(this); }
  ~LastErrorSink() override { absl::RemoveLogSink(this); }
  void Send(const absl::LogEntry &entry) override {
    if (entry.log_severity() >= absl::LogSeverity::kError) {
      text = std::string(entry.text_message());
    }
  }
  std::string text;
};

// The daemon: everything after the wrapper has parsed and forked. Returns
// the exit status of a clean run, or why dcfs did not start or failed.
absl::StatusOr<int> RunDaemon(const MountRequest &request) {
  const HelperArgs &args = request.args;
  const HelperOptions &options = request.options;
  if (!options.cache_db.has_value()) {
    // Step 15.3 derives a default from the instance identity.
    return MarkUsageError(InvalidArgumentErrorBuilder()
                          << "dcfs.cache_db is required (the cache "
                             "database's path)");
  }
  const std::string cache_db = *options.cache_db;
  const char *mountpoint = args.mountpoint.c_str();
  absl::StatusOr<MountOptions> mount_opts =
      BuildMountOptions(absl::GetFlag(FLAGS_allow_other), options.fuse_options);
  if (!mount_opts.ok()) return MarkUsageError(mount_opts.status());
  LOG(INFO) << "dcfs " << kVersion
            << " starting: source=" << args.spec
            << " cache_db=" << cache_db << " mountpoint=" << mountpoint
            << " mount_options=" << absl::StrJoin(mount_opts->options, ",")
            << " attr_timeout_sec=" << absl::GetFlag(FLAGS_attr_timeout_sec)
            << " entry_timeout_sec=" << absl::GetFlag(FLAGS_entry_timeout_sec)
            << " sync_interval_sec=" << absl::GetFlag(FLAGS_sync_interval_sec)
            << " foreground=" << options.foreground;

  // The backing tree is reached through `backing.root` alone from here on:
  // never through a path again, which is what makes mounting dcfs back over
  // SOURCE itself (a supported configuration) safe. `backing.tree` keeps a
  // captured clone alive.
  ABSL_ASSIGN_OR_RETURN(OpenedBacking backing_tree, OpenBacking(args, options));
  FileDescriptor source_fd = std::move(backing_tree.root);
  ABSL_ASSIGN_OR_RETURN(struct stat backing_root, syscalls::fstat(*source_fd));

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
      absl::StatusOr<struct stat> st = syscalls::fstatat(AT_FDCWD, parent);
      if (!st.ok()) {
        if (StatusToErrno(st.status()) != ENOENT) {
          return absl::StatusBuilder(st.status())
                 << "cache database directory " << parent;
        }
        ABSL_RETURN_IF_ERROR(syscalls::mkdirat(AT_FDCWD, parent, 0700))
            << "creating cache database directory " << parent;
      } else if ((st->st_mode & (S_IRWXG | S_IRWXO)) != 0) {
        LOG(WARNING) << "cache database directory " << parent
                     << " is group- or world-accessible (mode "
                     << absl::StrFormat("0%o", st->st_mode & 07777)
                     << "); it holds a cache as sensitive as the source and "
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
    if (absl::Status locked = syscalls::flock(*db_lock, LOCK_EX | LOCK_NB);
        !locked.ok()) {
      if (StatusToErrno(locked) == EWOULDBLOCK) {
        return FailedPreconditionErrorBuilder()
               << "Cache database " << cache_db
               << " is in use by another dcfs process; two daemons cannot "
                  "share one cache database";
      }
      return absl::StatusBuilder(locked) << "dcfs.cache_db=" << cache_db;
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
  // Observes nothing, except in the testonly checking and recording builds
  // (see dcfs/protocol_events.h).
  Observe(ctx, &MainProtocolEvents());
  // How long the kernel lets a lazytime access time stay in memory (step
  // 23.8): atime-only dirty rows drive a sync point once that old.
  if (absl::StatusOr<std::string> value =
          ReadProcValue("/proc/sys/vm/dirtytime_expire_seconds");
      value.ok()) {
    int64_t seconds = 0;
    if (absl::SimpleAtoi(*value, &seconds) && seconds > 0) {
      ctx.dirty.atime_expiry = absl::Seconds(seconds);
    }
  } else {
    LOG(WARNING) << "vm.dirtytime_expire_seconds: " << value.status()
                 << "; atime-only dirty rows drive a sync point after "
                 << absl::FormatDuration(ctx.dirty.atime_expiry);
  }

  ABSL_ASSIGN_OR_RETURN(RootIdentity root, backing::ProbeRoot(ctx, *source_fd));
  ABSL_RETURN_IF_ERROR(Migrate(db, root));
  // backing::InitRoot() also refuses a cache database built for a different
  // filesystem, but can't be given a --cache_db path or an actionable
  // suggestion to put in its error (it only sees fds, not flags); do that
  // check here instead, before InitRoot, so the message names both.
  ABSL_ASSIGN_OR_RETURN(DeviceId stored_device, GetSourceDeviceId(db));
  if (stored_device != root.device_id) {
    return FailedPreconditionErrorBuilder()
           << "Cache database " << cache_db << " was created for filesystem "
           << stored_device.ToString() << ", but SOURCE is on "
           << root.device_id.ToString()
           << "; delete the database to start a cold cache";
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
      return FailedPreconditionErrorBuilder()
             << "Cache database " << cache_db
             << " was created for a different source directory (inode "
             << stored_root.backing_ino << ", generation "
             << stored_root.backing_gen << ") than SOURCE (inode "
             << root.backing_ino << ", generation " << root.backing_gen
             << "); delete the database to start a cold cache";
    }
  }
  // Before anything reads the cache: after an unclean shutdown, forget
  // whatever the dirty set says a power loss may have made wrong.
  ABSL_ASSIGN_OR_RETURN(std::string boot_id, ReadBootId());
  ABSL_RETURN_IF_ERROR(backing::Startup(ctx, std::move(source_fd), boot_id));

  DirCacheFS::Options opts{
      .attr_timeout = absl::Seconds(absl::GetFlag(FLAGS_attr_timeout_sec)),
      .entry_timeout = absl::Seconds(absl::GetFlag(FLAGS_entry_timeout_sec)),
      .max_read = mount_opts->max_read,
      .sync_interval = absl::Seconds(absl::GetFlag(FLAGS_sync_interval_sec)),
      // default_permissions (and allow_other, if requested) ahead of
      // --fuse_opt's; Init() checks the first is there.
      .mount_options = mount_opts->options,
  };

  DirCacheFS fs(ctx, opts);
  // What the kernel shows for the mount (mountinfo, df, findmnt): the spec
  // as written, with the type fuse.dcfs. libfuse's own escape for a comma.
  std::string fsname = absl::StrCat("fsname=", absl::StrReplaceAll(
      args.spec, {{"\\", "\\\\"}, {",", "\\,"}}));
  std::vector<std::string> fuse_option_list = {
      "-o", absl::StrJoin(opts.mount_options, ","), "-o", fsname, "-o",
      "subtype=dcfs"};
  if (options.read_only) {
    fuse_option_list.push_back("-o");
    fuse_option_list.push_back("ro");
  }
  std::vector<std::string> fuse_arg_strings = {std::string(kMountHelperName)};
  fuse_arg_strings.insert(fuse_arg_strings.end(), fuse_option_list.begin(),
                          fuse_option_list.end());
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
    return InternalErrorBuilder() << "fuse_session_new failed";
  }

  if (fuse_set_signal_handlers(session) != 0) {
    fuse_session_destroy(session);
    return InternalErrorBuilder() << "fuse_set_signal_handlers failed";
  }

  // Until the INIT is answered, the last ERROR is why a refused one failed.
  std::optional<LastErrorSink> last_error;
  last_error.emplace();
  // How mountinfo will name the mount point, taken now: once the mount is
  // there, resolving the path would ask this daemon, which is not serving yet.
  absl::StatusOr<std::string> canonical = CanonicalMountpoint(mountpoint);
  if (!canonical.ok()) {
    fuse_remove_signal_handlers(session);
    fuse_session_destroy(session);
    return canonical.status();
  }
  if (fuse_session_mount(session, mountpoint) != 0) {
    fuse_remove_signal_handlers(session);
    fuse_session_destroy(session);
    return InternalErrorBuilder()
           << "fuse_session_mount(" << mountpoint << ") failed";
  }
  // The lock the umount helper waits on, held until this process exits
  // (dcfs/umount_helper.h).
  absl::StatusOr<DaemonLock> daemon_lock = [&]() -> absl::StatusOr<DaemonLock> {
    ABSL_ASSIGN_OR_RETURN(std::string mountinfo, ReadMountinfo());
    return HoldDaemonLock(mountinfo, *canonical);
  }();
  if (!daemon_lock.ok()) {
    fuse_session_unmount(session);
    fuse_remove_signal_handlers(session);
    fuse_session_destroy(session);
    return daemon_lock.status();
  }

  // libfuse's loop, plus draining /dev/fuse at a checkpoint so that a
  // request sees its own FUSE_INTERRUPT (dcfs/session_loop.h). The wrapper
  // waiting for this daemon is told when the kernel's FUSE_INIT has been
  // answered: from then on dcfs is serving.
  SessionLoop loop(session);
  loop.SetOnInit([&request, &last_error] {
    last_error.reset();
    request.reporter.Ready();
  });
  ctx.interrupts = &loop;
  int rc = loop.Run();
  ctx.interrupts = &NoInterrupts();

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
    LOG(ERROR) << "closing cache database: " << close_status;
  }
  // The cache database's own lock first, then the helper's lock file: a daemon
  // that starts on a fresh file the moment this one is gone must not find the
  // database still locked. The helper's lock stays held until the process
  // exits.
  if (absl::Status unlocked = db_lock.Close(); !unlocked.ok()) {
    LOG(ERROR) << "closing the cache database's lock: " << unlocked;
  }
  RemoveDaemonLockFile(*daemon_lock);

  // SessionLoop::Run() returns 0 when the kernel connection was closed
  // (e.g. the mount was unmounted externally) or a signal handler stopped
  // the loop, or a negative -errno on an actual error -- only the last of
  // those is a failure.
  if (rc == -EPROTO && last_error.has_value() && !last_error->text.empty()) {
    return InternalErrorBuilder()
           << "dcfs refused the kernel's FUSE_INIT: " << last_error->text;
  }
  if (rc < 0) {
    return InternalErrorBuilder() << "fuse_session_loop: " << rc;
  }
  return EXIT_SUCCESS;
}

// Why dcfs refuses to run as anyone but root (README, "Why root"; decision
// 12): each of these needs a privilege no user has.
absl::Status RequireRoot(std::string_view program) {
  if (syscalls::getuid() == 0) return absl::OkStatus();
  return MarkUsageError(PermissionDeniedErrorBuilder()
         << program << " must run as root (fstab's user option does not "
            "work): FUSE passthrough, the private mount namespace and "
            "open_tree need CAP_SYS_ADMIN, open_by_handle_at needs "
            "CAP_DAC_READ_SEARCH, and acting with each caller's credentials "
            "needs setfsuid, setfsgid and setgroups");
}

// The wrapper: `mount.dcfs SOURCE MOUNTPOINT [-sfnv] [-N ns] [-o OPTIONS]`.
// Returns its exit status.
int MountHelperMain(int argc, char *argv[]) {
  const std::vector<std::string> words(argv + 1, argv + argc);
  absl::StatusOr<HelperArgs> args = ParseHelperArgs(words);
  if (!args.ok()) {
    LOG(ERROR) << args.status() << "; usage: mount.dcfs SOURCE MOUNTPOINT "
               << "[-sfnv] [-N ns] [-o OPTIONS]";
    return 1;
  }
  if (args->version) {
    std::cout << "mount.dcfs (dcfs " << kVersion << ")" << std::endl;
    return 0;
  }
  absl::StatusOr<HelperOptions> options = SplitHelperOptions(args->options);
  absl::Status checked = options.status();
  if (checked.ok()) checked = RequireRoot(kMountHelperName);
  if (checked.ok() && args->mount_namespace.has_value()) {
    checked = MarkUsageError(
        UnimplementedErrorBuilder()
        << "Option -N is not supported: mount.dcfs mounts in its own "
           "namespace");
  }
  if (checked.ok()) {
    for (const auto &[name, value] : options->flags) {
      checked = ApplyFlagOption(name, value);
      if (!checked.ok()) break;
    }
  }
  if (checked.ok() && !options->remount) {
    checked = MakePathsAbsolute(*args, *options);
  }
  if (checked.ok() && !options->remount) {
    // A mount point that is not a directory (a file mount point is refused,
    // decision 9).
    absl::StatusOr<struct stat> st = syscalls::fstatat(AT_FDCWD, args->mountpoint);
    if (!st.ok()) {
      checked = absl::StatusBuilder(st.status())
                << "MOUNTPOINT " << args->mountpoint;
    } else if (!S_ISDIR(st->st_mode)) {
      checked = MarkUsageError(
          InvalidArgumentErrorBuilder()
          << "MOUNTPOINT " << args->mountpoint
          << " is not a directory: a file cannot be a dcfs mount point");
    }
  }
  if (!checked.ok()) {
    LOG(ERROR) << checked;
    return ExitStatusFor(checked);
  }
  if (args->fake) return 0;
  if (options->remount) {
    // libmount merges fstab's options into a remount; the underlying mount
    // is not reachable from here, so those are what the line always said.
    if (std::vector<std::string> ignored = UnhonoredNativeOptions(*options);
        !ignored.empty()) {
      LOG(WARNING) << "a remount of dcfs changes only the dcfs mount "
                      "(dcfs.ro); ignoring the underlying mount's options "
                   << absl::StrJoin(ignored, ", ");
    }
    absl::Status remounted = RemountDcfs(args->mountpoint, options->read_only);
    if (!remounted.ok()) LOG(ERROR) << remounted;
    return remounted.ok() ? 0 : ExitStatusFor(remounted);
  }

  // Fork first, before the database, the mount or any thread: the daemon is
  // the child; this process waits for its report and exits with it.
  StartupReporter reporter;
  if (!options->foreground) {
    // Returns in the daemon (the child) only; the wrapper exits inside.
    absl::StatusOr<StartupReporter> forked = ForkDaemon();
    if (!forked.ok()) {
      LOG(ERROR) << forked.status();
      return 1;
    }
    reporter = *std::move(forked);
  }
  // A daemon logs to syslog alone (its stderr is /dev/null), and the one
  // knob, the stderr threshold, governs it; a foreground dcfs logs to stderr.
  std::optional<SyslogSink> syslog_sink;
  if (!options->foreground) {
    syslog_sink.emplace(absl::StderrThreshold());
    absl::SetStderrThreshold(absl::LogSeverityAtLeast::kInfinity);
    absl::AddLogSink(&*syslog_sink);
  }
  absl::Cleanup remove_sink = [&syslog_sink] {
    if (syslog_sink.has_value()) absl::RemoveLogSink(&*syslog_sink);
  };

  absl::StatusOr<int> ran =
      RunDaemon({.args = *args, .options = *options, .reporter = reporter});
  if (ran.ok()) return *ran;
  if (reporter.pending()) {
    // Not started: the waiting wrapper prints it and exits with its status.
    reporter.Fail(ran.status());
  } else {
    LOG(ERROR) << ran.status();
  }
  return ExitStatusFor(ran.status());
}

// umount.fuse.dcfs and umount.fuse: `umount.fuse.dcfs TARGET [OPTIONS]`, as
// umount(8) runs it. Unmounts (umount -i, with the options) and waits for the
// daemon of a dcfs mount (dcfs/umount_helper.h). Returns umount(8)'s exit
// status, or 1 for a usage mistake.
int UmountHelperMain(int argc, char *argv[]) {
  const std::vector<std::string> words(argv + 1, argv + argc);
  absl::StatusOr<UmountArgs> args = ParseUmountArgs(words);
  if (!args.ok()) {
    LOG(ERROR) << args.status()
               << "; usage: umount.fuse.dcfs MOUNTPOINT [OPTIONS]";
    return 1;
  }
  if (args->version) {
    std::cout << "umount.fuse.dcfs (dcfs " << kVersion << ")" << std::endl;
    return 0;
  }
  absl::Status done = UmountAndWait(*args);
  if (done.ok()) return 0;
  if (!done.message().empty()) LOG(ERROR) << done;
  return ExitStatusFor(done);
}

// The plain `dcfs` binary: --help and --version, and a pointer to the
// wrapper (decision 8: dcfs is mounted as mount.dcfs; there is no --source).
int PlainMain(int argc, char *argv[]) {
  absl::SetProgramUsageMessage(
      "dcfs is started by mount(8) as mount.dcfs: mount -t dcfs -o "
      "dcfs.fstype=none,dcfs.cache_db=PATH SOURCE MOUNTPOINT (see README)");
  std::vector<char *> args = absl::ParseCommandLine(argc, argv);
  absl::Status usage = UsageError(
      "dcfs mounts through its mount.dcfs name: run mount.dcfs SOURCE "
      "MOUNTPOINT -o OPTIONS, or mount -t dcfs");
  LOG(ERROR) << usage << (args.size() > 1 ? " (arguments ignored)" : "");
  return 1;
}

int Main(int argc, char *argv[]) {
  // argv[0] dispatch (decision 5): mount(8) runs mount.dcfs, umount(8)
  // umount.fuse.dcfs (or umount.fuse, for every FUSE mount: dcfs/mount_dcfs.h).
  const std::string_view program = argc > 0 ? argv[0] : "";
  const size_t slash = program.rfind('/');
  const std::string_view name =
      slash == std::string_view::npos ? program : program.substr(slash + 1);
  // The umount helpers first, before any process setup: they run for other
  // users and other filesystems' unmounts (as umount.fuse), whose child
  // `umount -i` must see the umask it was run with, and a user who unmounts
  // their sshfs must not get a dcfs warning about a file limit.
  if (IsUmountHelperName(name)) {
    absl::InitializeLog();
    return UmountHelperMain(argc, argv);
  }
  // The backing create(2)-family syscalls (backing.h's MkdirAt/MknodAt/
  // CreateAt) run with the caller's umask, switched to around each one
  // (AsCaller; the kernel sends it with the request, see
  // FUSE_CAP_DONT_MASK in DirCacheFS::Init). The daemon's own umask is set
  // to 0 so that whatever it inherited from its launching shell (typically
  // 022) can never alter a mode on the backing filesystem -- found via
  // pjdfstest before the per-request umask existed (open/02.t, open/03.t:
  // `open(..., 0642)` landing as 0640 on the backing file).
  syscalls::umask(0);
  InstallFlagsUsageConfig();
  // WARNING and above go to stderr by default (Abseil's own default is
  // ERROR), so that e.g. out-of-band changes to the backing filesystem
  // (backing::ReconcileAttrs) are visible in the daemon's log. Set before
  // the options are applied, so an explicit dcfs.stderrthreshold wins.
  absl::SetStderrThreshold(absl::LogSeverityAtLeast::kWarning);
  absl::InitializeLog();
  RaiseFileLimit();
  if (IsMountHelperName(name)) return MountHelperMain(argc, argv);
  return PlainMain(argc, argv);
}

}  // namespace
}  // namespace dcfs

int main(int argc, char *argv[]) { return dcfs::Main(argc, argv); }
