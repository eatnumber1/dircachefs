#include "dcfs/fsck.h"

#include <fcntl.h>
#include <signal.h>
#include <sys/file.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

#include "absl/log/log.h"
#include "absl/status/status.h"
#include "absl/status/status_macros.h"
#include "absl/status/statusor.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/str_join.h"
#include "absl/strings/str_split.h"
#include "dcfs/escape.h"
#include "dcfs/fd.h"
#include "dcfs/migrate.h"
#include "dcfs/mount_dcfs.h"
#include "dcfs/sqlite.h"
#include "dcfs/status.h"
#include "dcfs/syscalls.h"
#include "dcfs/syscalls_backing.h"
#include "dcfs/syscalls_process.h"
#include "dcfs/version.h"

namespace dcfs {
namespace {

// findmnt(8) (util-linux) is run by this path, as /bin/mount is in the capture;
// /bin is a symlink to /usr/bin on merged-usr systems.
constexpr char kFindmntProgram[] = "/bin/findmnt";
// Where util-linux's fsck looks for fsck.<type> (and where blkid is).
constexpr std::string_view kCheckerDirs[] = {
    "/sbin", "/usr/sbin", "/sbin/fs.d", "/sbin/fs", "/etc/fs", "/etc"};

constexpr char kUsage[] =
    "usage: fsck.dcfs [-apynf] [-C[FD]] [-o OPTIONS] DEVICE";

// The first `name` in the usual places for fsck's checkers, or empty.
std::string FindProgram(std::string_view name) {
  for (std::string_view dir : kCheckerDirs) {
    std::string path = absl::StrCat(dir, "/", name);
    if (syscalls::fstatat(AT_FDCWD, path).ok()) return path;
  }
  return "";
}

// Everything `fd` yields until its end.
std::string ReadAll(int fd) {
  std::string text;
  char buf[1024];
  while (true) {
    absl::StatusOr<size_t> n = syscalls::read(fd, buf, sizeof(buf));
    if (!n.ok() || *n == 0) break;
    text.append(buf, *n);
  }
  return text;
}

// Runs `path` as `argv0 words...`, its standard output collected in `captured`
// when it is given (standard input and error are ours). The exit status, as
// fsck(8) would see it from a checker: the program's own; kFsckCancelled if it
// died of SIGINT; kFsckOperational for any other signal.
absl::StatusOr<int> RunProgram(const std::string &path,
                               const std::string &argv0,
                               const std::vector<std::string> &words,
                               std::string *captured) {
  std::pair<FileDescriptor, FileDescriptor> pipe;
  if (captured != nullptr) {
    ASSIGN_OR_RETURN(pipe, syscalls::socketpair(AF_UNIX, SOCK_STREAM, 0));
  }
  ASSIGN_OR_RETURN(pid_t child, syscalls::fork());
  if (child == 0) {
    if (captured != nullptr &&
        !syscalls::dup2(*pipe.second, STDOUT_FILENO).ok()) {
      syscalls::_exit(126);
    }
    std::vector<char *> argv;
    argv.push_back(const_cast<char *>(argv0.c_str()));
    for (const std::string &word : words) {
      argv.push_back(const_cast<char *>(word.c_str()));
    }
    argv.push_back(nullptr);
    syscalls::execv(path.c_str(), argv.data()).IgnoreError();
    syscalls::_exit(127);
  }
  if (captured != nullptr) {
    pipe.second.Close().IgnoreError();
    *captured = ReadAll(*pipe.first);
  }
  int wait_status = 0;
  while (true) {
    absl::StatusOr<pid_t> reaped = syscalls::waitpid(child, &wait_status, 0);
    if (reaped.ok()) break;
    if (StatusToErrno(reaped.status()) != EINTR) {
      return absl::StatusBuilder(reaped.status()) << "while waiting for " << path;
    }
  }
  if (WIFEXITED(wait_status)) return WEXITSTATUS(wait_status);
  return WTERMSIG(wait_status) == SIGINT ? kFsckCancelled : kFsckOperational;
}

// The options of the fstab line whose source is `device`, as findmnt prints
// them (comma-separated).
absl::StatusOr<std::string> FstabOptions(const std::string &device) {
  std::string out;
  absl::StatusOr<int> ran = RunProgram(
      kFindmntProgram, "findmnt",
      {"--fstab", "--noheadings", "--raw", "--output", "OPTIONS", "--source",
       device},
      &out);
  if (!ran.ok()) return ran.status();
  const std::string first(absl::StripAsciiWhitespace(
      std::string_view(out).substr(0, out.find('\n'))));
  if (*ran != 0 || first.empty()) {
    return NotFoundErrorBuilder()
           << "no line in /etc/fstab (as findmnt reads it) has the source "
           << EscapeBytes(device)
           << "; name the dcfs options with -o to check a filesystem by hand";
  }
  return first;
}

// The type of the filesystem on `device`, as blkid says.
absl::StatusOr<std::string> DetectType(const std::string &device) {
  const std::string blkid = FindProgram("blkid");
  if (blkid.empty()) {
    return NotFoundErrorBuilder()
           << "dcfs.fstype is not set and blkid is not installed to find the "
              "type of "
           << EscapeBytes(device);
  }
  std::string out;
  ASSIGN_OR_RETURN(
      int ran,
      RunProgram(blkid, "blkid", {"-o", "value", "-s", "TYPE", device}, &out));
  const std::string type(absl::StripAsciiWhitespace(out));
  if (ran != 0 || type.empty()) {
    return NotFoundErrorBuilder()
           << "blkid finds no filesystem type on " << EscapeBytes(device)
           << "; set dcfs.fstype";
  }
  return type;
}

// The backing filesystem's fsck, with the flags fsck was given, on `device`.
// Its status is the result.
int CheckBacking(const std::string &type, const std::string &device,
                 const std::vector<std::string> &flags) {
  if (type.find('/') != std::string::npos) {
    LOG(ERROR) << "dcfs.fstype=" << EscapeBytes(type) << " is not a type";
    return kFsckOperational;
  }
  const std::string name = absl::StrCat("fsck.", type);
  const std::string path = FindProgram(name);
  if (path.empty()) {
    LOG(ERROR) << name << " not found: nothing can check the " << type
               << " filesystem of " << EscapeBytes(device);
    return kFsckOperational;
  }
  std::vector<std::string> words = flags;
  words.push_back(device);
  absl::StatusOr<int> ran = RunProgram(path, name, words, nullptr);
  if (!ran.ok()) {
    LOG(ERROR) << ran.status();
    return kFsckOperational;
  }
  if (*ran == 127) {
    LOG(ERROR) << name << " could not be run (" << path << ")";
    return kFsckOperational;
  }
  return *ran;
}

// What looking at an open cache database found.
struct Finding {
  enum class Kind { kOk, kCorrupt, kNewer, kBusy } kind = Kind::kOk;
  std::string detail;               // for kCorrupt, kNewer and kBusy
  std::vector<std::string> notes;   // for kOk: things worth saying
};

// The first few rows of a query's first column.
absl::StatusOr<std::vector<std::string>> FirstRows(sqlite3::Connection &db,
                                                   std::string_view sql,
                                                   size_t most) {
  ASSIGN_OR_RETURN(sqlite3::Statement * stmt, db.Prepared(sql));
  std::vector<std::string> rows;
  RETURN_IF_ERROR(stmt->ForEachRow([&](sqlite3::Statement &row) {
    if (rows.size() < most) rows.push_back(row.Column<std::string>(0));
    return absl::OkStatus();
  }));
  return rows;
}

Finding Corrupt(std::string detail) {
  return Finding{.kind = Finding::Kind::kCorrupt, .detail = std::move(detail)};
}

// Opens the database read-only (a check changes nothing: a repair deletes the
// whole thing) and looks: SQLite's integrity_check, the schema version, the
// dirty set.
Finding Inspect(const std::string &path) {
  absl::StatusOr<sqlite3::Connection> opened =
      sqlite3::ConnectionFactory{.path = path, .flags = SQLITE_OPEN_READONLY}
          .Open();
  if (!opened.ok()) {
    if (absl::IsUnavailable(opened.status())) {
      return Finding{.kind = Finding::Kind::kBusy,
                     .detail = absl::StrCat("is locked: ",
                                            opened.status().message())};
    }
    return Corrupt(absl::StrCat("cannot be opened: ", opened.status().message()));
  }
  sqlite3::Connection &db = *opened;

  absl::StatusOr<std::vector<std::string>> integrity =
      FirstRows(db, "PRAGMA integrity_check", 3);
  if (!integrity.ok()) {
    return Corrupt(absl::StrCat("integrity_check failed: ",
                                integrity.status().message()));
  }
  if (integrity->size() != 1 || integrity->front() != "ok") {
    return Corrupt(absl::StrCat("integrity_check found: ",
                                absl::StrJoin(*integrity, "; ")));
  }

  absl::StatusOr<int> version = GetSchemaVersion(db);
  if (!version.ok()) {
    return Corrupt(absl::StrCat("has no readable schema version: ",
                                version.status().message()));
  }
  Finding found;
  if (*version > kSchemaVersion) {
    return Finding{
        .kind = Finding::Kind::kNewer,
        .detail = absl::StrCat("has schema version ", *version,
                               ", newer than the ", kSchemaVersion,
                               " this dcfs knows: it was written by a newer "
                               "dcfs and is not corrupt; use that dcfs, or "
                               "delete the file to start cold")};
  }
  if (*version < kSchemaVersion) {
    found.notes.push_back(absl::StrCat(
        "the cache database has schema version ", *version,
        "; the next mount will upgrade it to ", kSchemaVersion));
  }

  // One pass over the dirty set: how many rows, and the first few that name
  // an inode which does not exist.
  size_t dirty = 0;
  std::vector<std::string> missing;
  absl::Status read = [&]() -> absl::Status {
    ASSIGN_OR_RETURN(
        sqlite3::Statement * stmt,
        db.Prepared("SELECT inode, inode NOT IN (SELECT id FROM inodes) "
                    "FROM dirty ORDER BY inode"));
    return stmt->ForEachRow([&](sqlite3::Statement &row) {
      ++dirty;
      if (row.Column<int>(1) != 0 && missing.size() < 3) {
        missing.push_back(row.Column<std::string>(0));
      }
      return absl::OkStatus();
    });
  }();
  if (!read.ok()) {
    return Corrupt(absl::StrCat("dirty set unreadable: ", read.message()));
  }
  if (!missing.empty()) {
    return Corrupt(absl::StrCat("the dirty set names inodes that do not exist (",
                                absl::StrJoin(missing, ", "), ")"));
  }
  absl::StatusOr<bool> clean = GetCleanShutdown(db);
  if (!clean.ok()) {
    return Corrupt(absl::StrCat("the clean-shutdown flag is unreadable: ",
                                clean.status().message()));
  }
  if (*clean && dirty != 0) {
    return Corrupt(absl::StrCat(
        "marks a clean shutdown with ", dirty,
        " dirty entries (a clean shutdown has an empty dirty set)"));
  }
  if (!*clean && dirty != 0) {
    found.notes.push_back(absl::StrCat(
        "the last run did not end cleanly: ", dirty,
        " dirty entries, which the next mount recovers by itself"));
  }
  return found;
}

}  // namespace

bool IsFsckHelperName(std::string_view name) { return name == kFsckHelperName; }

int CombineFsckStatus(int a, int b) { return a | b; }

absl::StatusOr<FsckArgs> ParseFsckArgs(std::span<const std::string> args) {
  FsckArgs parsed;
  bool repair = false;
  bool report_only = false;
  bool have_device = false;
  for (size_t i = 0; i < args.size(); ++i) {
    const std::string &arg = args[i];
    if (arg == "-V") {
      parsed.version = true;
    } else if (arg == "-o") {
      if (i + 1 >= args.size()) {
        return InvalidArgumentErrorBuilder() << "-o needs its options";
      }
      parsed.options = args[++i];
    } else if (arg == "-n") {
      report_only = true;
      parsed.backing_flags.push_back(arg);
    } else if (arg == "-a" || arg == "-p" || arg == "-y") {
      repair = true;
      parsed.backing_flags.push_back(arg);
    } else if (arg.size() > 1 && arg[0] == '-') {
      parsed.backing_flags.push_back(arg);  // -f, -C3, -T, ...: the backing's
    } else if (have_device) {
      return InvalidArgumentErrorBuilder()
             << "fsck.dcfs checks one device at a time, not " << arg << " too";
    } else {
      parsed.device = arg;
      have_device = true;
    }
  }
  if (!have_device && !parsed.version) {
    return InvalidArgumentErrorBuilder() << "a device to check is needed";
  }
  // fsck(8) never changes what -n told it not to.
  parsed.mode =
      repair && !report_only ? FsckMode::kRepair : FsckMode::kReport;
  return parsed;
}

CacheReport CheckCacheDatabase(const std::string &path, FsckMode mode) {
  CacheReport report;
  const std::string shown = EscapeBytes(path);
  absl::StatusOr<FileDescriptor> opened =
      syscalls::openat(AT_FDCWD, path, O_RDWR | O_NOFOLLOW);
  if (!opened.ok()) {
    if (StatusToErrno(opened.status()) == ENOENT) {
      report.lines.push_back(absl::StrCat(
          "no cache database at ", shown, ": the next mount starts cold"));
      return report;
    }
    report.status = kFsckOperational;
    report.lines.push_back(absl::StrCat("cannot open the cache database ",
                                        shown, ": ", opened.status().message()));
    return report;
  }
  // The daemon's own lock (dcfs/main.cc): held while one runs. Reported, never
  // waited for; held here until a rebuild is done.
  if (absl::Status locked = syscalls::flock(**opened, LOCK_EX | LOCK_NB);
      !locked.ok()) {
    report.status = kFsckOperational;
    report.lines.push_back(
        StatusToErrno(locked) == EWOULDBLOCK
            ? absl::StrCat("the cache database ", shown,
                           " is in use by a running dcfs: unmount the "
                           "filesystem before checking it")
            : absl::StrCat("cannot lock the cache database ", shown, ": ",
                           locked.message()));
    return report;
  }

  Finding found = Inspect(path);
  switch (found.kind) {
    case Finding::Kind::kOk:
      report.lines = std::move(found.notes);
      report.lines.push_back(absl::StrCat("cache database ", shown, ": clean"));
      return report;
    case Finding::Kind::kBusy:
      report.status = kFsckOperational;
      report.lines.push_back(
          absl::StrCat("the cache database ", shown, " ", found.detail));
      return report;
    case Finding::Kind::kNewer:
      report.status = kFsckUncorrected;
      report.lines.push_back(
          absl::StrCat("the cache database ", shown, " ", found.detail));
      return report;
    case Finding::Kind::kCorrupt:
      break;
  }
  if (mode == FsckMode::kReport) {
    report.status = kFsckUncorrected;
    report.lines.push_back(absl::StrCat(
        "the cache database ", shown, " is corrupt (", found.detail,
        "); fsck -y (or -a) deletes it, and the next mount starts cold"));
    return report;
  }
  // A cache is rebuilt, not repaired: nothing in it is the only copy of
  // anything.
  for (const char *suffix : {"", "-wal", "-shm"}) {
    absl::Status removed =
        syscalls::unlinkat(AT_FDCWD, absl::StrCat(path, suffix), 0);
    if (!removed.ok() && StatusToErrno(removed) != ENOENT) {
      report.status = kFsckOperational;
      report.lines.push_back(absl::StrCat("cannot delete the corrupt cache ",
                                          "database ", shown, suffix, ": ",
                                          removed.message()));
      return report;
    }
  }
  report.status = kFsckCorrected;
  report.lines.push_back(absl::StrCat(
      "the cache database ", shown, " was corrupt (", found.detail,
      "); deleted: the next mount starts cold"));
  return report;
}

int FsckMain(std::span<const std::string> words) {
  absl::StatusOr<FsckArgs> args = ParseFsckArgs(words);
  if (!args.ok()) {
    LOG(ERROR) << args.status().message() << "; " << kUsage;
    return kFsckUsage;
  }
  if (args->version) {
    std::cout << "fsck.dcfs (dcfs " << kVersion << ")" << std::endl;
    return kFsckOk;
  }

  // The line's dcfs options: -o, or the fstab line of the device.
  std::string options;
  if (args->options.has_value()) {
    options = *args->options;
  } else {
    absl::StatusOr<std::string> found = FstabOptions(args->device);
    if (!found.ok()) {
      LOG(ERROR) << found.status().message();
      return kFsckOperational;
    }
    options = *std::move(found);
  }
  const std::vector<std::string> option_list =
      absl::StrSplit(options, ',', absl::SkipEmpty());
  absl::StatusOr<HelperOptions> split = SplitHelperOptions(option_list);
  if (!split.ok()) {
    LOG(ERROR) << split.status().message();
    return kFsckOperational;
  }

  int status = kFsckOk;
  // 1. The backing filesystem.
  if (split->backing == HelperOptions::Backing::kNative) {
    std::string type;
    if (split->native_type.has_value()) {
      type = *split->native_type;
    } else if (absl::StatusOr<std::string> detected = DetectType(args->device);
               detected.ok()) {
      type = *std::move(detected);
    } else {
      LOG(ERROR) << detected.status().message();
      return kFsckOperational;
    }
    status = CombineFsckStatus(
        status, CheckBacking(type, args->device, args->backing_flags));
  } else {
    std::cout << EscapeBytes(args->device)
              << ": dcfs.fstype=bind serves a directory: there is no device "
                 "to check"
              << std::endl;
  }

  // 2. The cache database.
  if (!split->cache_db.has_value()) {
    LOG(ERROR) << "dcfs.cache_db is not among the options, so there is no "
                  "cache database to check";
    return CombineFsckStatus(status, kFsckOperational);
  }
  const CacheReport cache = CheckCacheDatabase(*split->cache_db, args->mode);
  for (const std::string &line : cache.lines) {
    (cache.status == kFsckOk ? std::cout : std::cerr) << "fsck.dcfs: " << line
                                                       << std::endl;
  }
  return CombineFsckStatus(status, cache.status);
}

}  // namespace dcfs
