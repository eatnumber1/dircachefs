// SPDX-License-Identifier: MIT
#ifndef TEST_QEMU_GUEST_GUEST_H_
#define TEST_QEMU_GUEST_GUEST_H_

// The helpers of a guest test written in C++ (step 25.34): what a scenario
// that mounts dcfs, does things to it as some user, crashes it and compares
// trees needs, and nothing else. They replace guest/lib.sh and the parts of
// guest/fault_lib.sh that scripts use; a test is a `qemu_cc_test` that links
// this library (qemu_cc_test's `dcfs = True` puts the dcfs binary in the guest,
// kDcfsBinary).
//
// The rules the helpers follow:
//  - Every call that can fail returns Status or StatusOr (style 1.6), naming
//    what it did and the answer it got.
//  - A command (mount, umount) is run with syscalls::system (an argv, no
//    shell; its exit status is a Status that names the command); what is under
//    test is called as a system call.
//  - Nothing waits for a time: the mount is awaited on the mount table and the
//    daemon's own exit, a child on its exit. (A test whose subject is time
//    writes its own wait, and says why.)
//  - Names are bytes: every name that is printed is escaped.
//
// The guest tests are the first user of the checks that name a check: a
// scenario is a gtest test, `RemovedCwd` for the shell's `removed-cwd`.

#include <sys/types.h>

#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "absl/functional/function_ref.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"

namespace dcfs {

// Who a function runs as: setresgid, setgroups, then setresuid, in that order.
struct Identity {
  uid_t uid;
  gid_t gid;
  std::vector<gid_t> groups;
};

// Runs `fn` in a forked child (so a chdir, a changed identity or a held
// descriptor does not outlive it), optionally as `as`, and returns what `fn`
// returned, carried back through a pipe. The child is waited for before this
// returns: whatever it held is released. An error if the child could not be
// made or set up, or if it died before `fn` returned (the Status says how).
absl::StatusOr<std::string> InChild(
    absl::FunctionRef<absl::StatusOr<std::string>()> fn,
    const std::optional<Identity> &as = std::nullopt);

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
  DcfsMount() : pid_(0) {}

  // Starts mount.dcfs (kDcfsBinary) and returns once the mount is in
  // the table. If the daemon exits first, the error carries its exit and its
  // stderr: nothing is waited for on a timer.
  static absl::StatusOr<std::unique_ptr<DcfsMount>> Start(MountSpec spec);

  // A destroyed mount that was not Unmount()ed or Crash()ed has its daemon
  // killed and reaped, so a failed test leaves no process behind.
  ~DcfsMount();
  DcfsMount(const DcfsMount &) = delete;
  DcfsMount &operator=(const DcfsMount &) = delete;

  // `umount` of the mountpoint, then waits for the daemon to exit; its exit
  // status must be 0.
  absl::Status Unmount();

  // SIGKILLs the daemon, waits for it and detaches the dead mount (`umount
  // -l`): the daemon dies at an arbitrary point of whatever it was doing.
  absl::Status Crash();

  // What the daemon wrote to stderr so far.
  absl::StatusOr<std::string> Log() const;

  const MountSpec &spec() const { return spec_; }
  pid_t pid() const { return pid_; }

 private:
  DcfsMount(MountSpec spec, pid_t pid, std::string log_path);

  MountSpec spec_;
  pid_t pid_;  // 0 once reaped
  std::string log_path_;
};

// Writes `contents` to `path`, which is created (mode 0644) or truncated.
absl::Status WriteFile(std::string_view path, std::string_view contents);

// One line per entry under `dir` (not lost+found), sorted by path, with the
// path escaped: "<path> <type> <size> <mode> <links>" and, for a regular
// file, the hash of its contents. What two trees must agree on to be the same
// tree: the served one and the backing filesystem's. Attributes are read
// before any contents, so a read moves no attribute that was listed.
absl::StatusOr<std::vector<std::string>> Snapshot(std::string_view dir);

// OK if the trees at `a` and `b` have the same Snapshot; otherwise the lines
// that differ, each marked with the side it is on.
absl::Status ExpectSameTree(std::string_view a, std::string_view b);

// The shared check of a scenario compared with the backing filesystem's own
// answer: a gtest failure that names `what`, the backing's value and dcfs's.
void ExpectSameAsBacking(std::string_view what, const std::string &backing,
                         const std::string &served);

}  // namespace dcfs

#endif  // TEST_QEMU_GUEST_GUEST_H_
