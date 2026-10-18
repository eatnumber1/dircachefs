// SPDX-License-Identifier: MIT
#ifndef TEST_QEMU_GUEST_GUEST_H_
#define TEST_QEMU_GUEST_GUEST_H_

// The helpers of a guest test written in C++: what a scenario that mounts
// dcfs, does things to it as some user, crashes it and compares trees needs.
// A test is a `qemu_cc_test` that links this library; `dcfs = True` puts the
// dcfs binary in the guest (kDcfsBinary). Nothing here waits for a time: the
// mount is awaited on the mount table and the daemon's exit, a child on its
// exit. Every call that can fail returns a Status that names what it did.

#include <sys/types.h>

#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "absl/base/nullability.h"
#include "absl/functional/function_ref.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/types/optional_ref.h"

namespace dcfs {

// Who a function runs as: setresgid, setgroups, then setresuid, in that order.
// It is not dcfs::Credentials, which also carries a umask this helper would
// silently ignore.
struct Identity {
  uid_t uid;
  gid_t gid;
  std::vector<gid_t> groups;
};

// Runs `fn` in a forked child (so a chdir, a changed identity or a held
// descriptor does not outlive it), optionally as `as`, and returns what `fn`
// returned, carried back over a socket with its Status whole (code, message
// and payloads, so StatusToErrno reads the same). The child is waited for
// before this returns. An error if the child could not be made or become
// `as`, or if it died before `fn` returned (the Status says how).
absl::StatusOr<std::string> InChild(
    absl::FunctionRef<absl::StatusOr<std::string>()> fn,
    absl::optional_ref<const Identity> as = std::nullopt);

// Where a dcfs mount is and how it is started: `dcfs.<flag>` options are
// the dcfs mount options of the mount helper (docs/design.md, mount helper).
struct MountSpec {
  std::string source;      // the backing directory
  std::string mountpoint;  // an existing directory
  std::string cache_db;    // the cache database, in a directory that exists
  std::vector<std::string> flags;  // e.g. "max_file_size=1G", no "dcfs." prefix
};

// Where guest/init puts the dcfs binary of a `qemu_cc_test` with `dcfs = True`
// (its data is under /test/data).
inline constexpr std::string_view kDcfsBinary = "/test/data/dcfs";

// A running dcfs daemon (foreground, a child of the test) and its mount.
class DcfsMount {
 public:
  // Starts mount.dcfs (kDcfsBinary) and returns once the mount is in the
  // table and the daemon answers a stat of it. If the daemon exits first, the
  // error carries its exit and its stderr.
  static absl::StatusOr<std::unique_ptr<DcfsMount> absl_nonnull> Start(
      MountSpec spec);

  // A mount that was not Unmount()ed or Crash()ed has its daemon killed and
  // reaped, so a failed test leaves no process behind.
  ~DcfsMount();
  DcfsMount(const DcfsMount &) = delete;
  DcfsMount &operator=(const DcfsMount &) = delete;

  // `umount` of the mountpoint, then waits for the daemon to exit; its exit
  // status must be 0. An error if the daemon is already gone.
  absl::Status Unmount();

  // SIGKILLs the daemon, waits for it and detaches the dead mount (`umount
  // -l`): the daemon dies at an arbitrary point of whatever it was doing. An
  // error if the daemon is already gone.
  absl::Status Crash();

  // What the daemon wrote to stdout and stderr so far.
  absl::StatusOr<std::string> Log() const;

 private:
  DcfsMount(MountSpec spec, pid_t pid, std::string log_path);

  static absl::StatusOr<std::unique_ptr<DcfsMount> absl_nonnull> Spawn(
      MountSpec spec);
  absl::StatusOr<pid_t> Running() const;
  // The error for a daemon that ended as `wait_status` says `when`, with its
  // log.
  absl::Status DaemonEnded(int wait_status, std::string_view when) const;

  MountSpec spec_;
  std::optional<pid_t> pid_;  // empty once reaped
  std::string log_path_;
};

// The guest's disk (TEST_TMPDIR), without the trailing slash.
std::string TestTmp();

// Creates the directory `path` (mode 0755).
absl::Status Mkdir(std::string_view path);

// Writes `contents` to `path`, which is created (mode 0644) or truncated.
absl::Status WriteFile(std::string_view path, std::string_view contents = "");

// One line per entry under `dir`, sorted by path, with the path escaped:
// "<path> <type> <size> <mode> <links>" and, for a regular file, the checksum
// of its contents. What two trees must agree on to be the same tree: the
// served one and the backing filesystem's.
absl::StatusOr<std::vector<std::string>> Snapshot(std::string_view dir);

// OK if the trees at `a` and `b` have the same Snapshot; otherwise the lines
// that differ, each marked with the side it is on.
absl::Status CompareTrees(std::string_view a, std::string_view b);

// The shared check of a scenario compared with the backing filesystem's own
// answer: a gtest failure that shows the backing's value and dcfs's.
void ExpectSameAsBacking(std::string_view backing, std::string_view served);

}  // namespace dcfs

#endif  // TEST_QEMU_GUEST_GUEST_H_
