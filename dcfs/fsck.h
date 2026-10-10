#ifndef DCFS_FSCK_H_
#define DCFS_FSCK_H_

// fsck.dcfs (plan step 15.5; russ, 2026-10-09: "add support for the sixth
// field of fstab, so you can do fsck via fstab on a filesystem that's set up
// via dcfs"). `fsck -A` and systemd's systemd-fsck@.service run
// `fsck.<type> [FLAGS] DEVICE` before they mount a line whose passno is not
// 0. It gets only the device and fsck's flags, so the helper finds its fstab
// line (`findmnt --fstab`, as a child, like the capture's /bin/mount) for
// the `dcfs.` options, then:
//  1. the backing filesystem: for a native `dcfs.fstype` it runs that type's
//     fsck on the device with the flags it was given and relays its status
//     (a passno on a dcfs line means what it means on a plain one); for
//     `bind` (a directory) there is no device, and it says so;
//  2. dcfs's own cache database (CheckCacheDatabase): that no daemon holds
//     it, SQLite's integrity_check, the schema version, and the dirty set's
//     sanity. A cache is rebuilt, not repaired: with -a, -p or -y a corrupt
//     or unreadable one is deleted and the next mount starts cold; with -n
//     (or no mode flag: there is nobody to ask) it is only reported.
// The exit status is fsck(8)'s, the backing's and the cache's combined as
// fsck(8) combines them (bitwise or). No timer: a database a daemon holds is
// reported, not waited for.

#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "absl/status/statusor.h"

namespace dcfs {

inline constexpr std::string_view kFsckHelperName = "fsck.dcfs";
// Whether `name` (argv[0] without its directory) is fsck.dcfs.
bool IsFsckHelperName(std::string_view name);

// fsck(8)'s exit status bits.
inline constexpr int kFsckOk = 0;
inline constexpr int kFsckCorrected = 1;
inline constexpr int kFsckReboot = 2;
inline constexpr int kFsckUncorrected = 4;
inline constexpr int kFsckOperational = 8;
inline constexpr int kFsckUsage = 16;
inline constexpr int kFsckCancelled = 32;

// How fsck(8) combines the statuses of several checkers: the bits are or'ed.
int CombineFsckStatus(int a, int b);

enum class FsckMode {
  kReport,  // -n, or no mode flag: find and say, change nothing
  kRepair,  // -a, -p, -y
};

// `fsck.dcfs [-apynf] [-C[FD]] [-o OPTIONS] [-V] DEVICE`.
struct FsckArgs {
  std::string device;
  FsckMode mode = FsckMode::kReport;
  bool version = false;  // -V alone
  // `-o OPTIONS`: the dcfs options of the filesystem (what fstab's fourth
  // field says), for a check run by hand with no fstab line; fstab is not
  // consulted then.
  std::optional<std::string> options;
  // Every other flag, in order, as given: the backing's fsck gets them.
  std::vector<std::string> backing_flags;
};

// InvalidArgument (a usage error: kFsckUsage) for no device, more than one,
// or -o without its argument.
[[nodiscard]] absl::StatusOr<FsckArgs> ParseFsckArgs(
    std::span<const std::string> args);

// What CheckCacheDatabase found.
struct CacheReport {
  int status = kFsckOk;
  // One line each, in the order found, for the user.
  std::vector<std::string> lines;
};

// Checks the cache database at `path` (never creates it: no file is nothing
// to check) and, in kRepair mode, deletes it (with its -wal and -shm) when it
// is corrupt or unreadable. Status: kFsckOk; kFsckCorrected for a database
// deleted; kFsckUncorrected for one that is corrupt and left (kReport), or
// that a newer dcfs wrote (never deleted: it is not corrupt); kFsckOperational
// for one a daemon holds (flock) or that cannot be looked at.
[[nodiscard]] CacheReport CheckCacheDatabase(const std::string &path,
                                             FsckMode mode);

// fsck.dcfs, as a process: `words` are the arguments after argv[0]. Returns
// the exit status (fsck(8)'s).
int FsckMain(std::span<const std::string> words);

}  // namespace dcfs

#endif  // DCFS_FSCK_H_
