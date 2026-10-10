#ifndef DCFS_MOUNT_DCFS_H_
#define DCFS_MOUNT_DCFS_H_

// The pure parts of the mount.dcfs wrapper (docs/plan/phases/15, step 15.2):
// the command line mount(8) runs a mount helper with, the split of its
// options into dcfs's own (the `dcfs.` prefix, decision 6) and the
// underlying mount's, the helper's exit statuses, the report the daemon
// sends the wrapper waiting for it, and the mountinfo questions a remount
// asks. No syscalls here: main.cc and the modules it uses do the work.

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "absl/status/status.h"
#include "absl/status/statusor.h"

namespace dcfs {

// The name the dcfs binary is installed under for mount(8) (argv[0]
// dispatch); fsck.dcfs follows in step 15.5.
inline constexpr std::string_view kMountHelperName = "mount.dcfs";
// libmount looks for mount.<type> with the type of the mountinfo line
// (fuse.dcfs) when it remounts, so that name reaches the helper too.
inline constexpr std::string_view kMountFuseHelperName = "mount.fuse.dcfs";

// Whether `name` (argv[0] without its directory) is one of the helper names.
bool IsMountHelperName(std::string_view name);

// umount(8) runs `umount.<type>` for a FUSE mount (step 15.6b). Which type
// string it looks for depends on how it found the mount (libmount's
// mnt_context_prepare_helper): the mountinfo line's, fuse.dcfs, first, then
// that with the subtype stripped, fuse; but when it shortcuts through statfs
// (root unmounting an absolute directory without -c, -f, -l, -r or -d, and no
// utab entry) there is no subtype and it looks for umount.fuse alone. systemd
// runs `umount <where> -c`, which takes the mountinfo way. So:
//  - umount.fuse.dcfs is the default helper: it runs for dcfs mounts only, and
//    covers systemd's stops (restart, reboot) and `umount -c`;
//  - umount.fuse is opt-in: it runs for EVERY FUSE filesystem, and adds plain
//    interactive root `umount /path`.
// Both are dcfs's binary dispatching on argv[0]; for a mount that is not dcfs's
// (only possible as umount.fuse) the helper runs `umount -i` (no helper) and
// nothing more.
inline constexpr std::string_view kUmountFuseDcfsHelperName =
    "umount.fuse.dcfs";
inline constexpr std::string_view kUmountFuseHelperName = "umount.fuse";
bool IsUmountHelperName(std::string_view name);

// `umount.fuse.dcfs TARGET [OPTIONS]`, as umount(8) runs it (-V alone prints
// the version). Every option except the ones below goes on to the `umount -i`
// the helper runs, unread (-n, -f, -r, -v, -c, -t type, ...).
struct UmountArgs {
  std::string target;
  bool lazy = false;  // -l, --lazy: detach now, do not wait for the daemon
  // -N ns, --namespace ns: the mount is in another mount namespace, whose
  // mountinfo and /sys this process does not see: unmounted by `umount -i`
  // (which does the setns), not waited for.
  bool other_namespace = false;
  bool version = false;  // -V
  // The options in the order given, with their arguments, for `umount -i`.
  std::vector<std::string> forwarded;
};

// InvalidArgument (a usage error: exit status 1) names the mistake: no mount
// point, or more than one.
absl::StatusOr<UmountArgs> ParseUmountArgs(std::span<const std::string> args);

// What mountinfo says of the topmost mount at a path.
struct MountEntry {
  std::string device;  // "major:minor", the third field
  std::string fstype;  // the type after the "-": fuse.dcfs, ext4, ...
};

// The topmost mount at `mountpoint` in the text of /proc/self/mountinfo
// (octal escapes decoded), or nullopt if nothing is mounted there.
std::optional<MountEntry> TopmostMountEntry(std::string_view mountinfo,
                                            std::string_view mountpoint);

// The device number ("major:minor") of the topmost mount at `mountpoint` if it
// is a fuse.dcfs mount, else nullopt. The daemon holds a lock file named by
// it for as long as it runs (DaemonLockPath). "FuseDcfs" names the mount type
// it looks for, fuse.dcfs; any other mount at the path (ext4, another fuse
// type) gives nullopt, whatever its device.
std::optional<std::string> MountDeviceOfFuseDcfs(std::string_view mountinfo,
                                                 std::string_view mountpoint);

// Where the daemon of the mount on `device` ("major:minor") holds its lock:
// <dir>/<major>_<minor>.lock, <dir> /run/dcfs.
inline constexpr std::string_view kDaemonLockDir = "/run/dcfs";
std::string DaemonLockPath(std::string_view device,
                           std::string_view dir = kDaemonLockDir);

// `mount.dcfs SOURCE MOUNTPOINT [-sfnv] [-N ns] [-o OPTIONS]`, as mount(8)
// runs it (-V alone prints the version).
struct HelperArgs {
  std::string source;
  std::string mountpoint;
  bool sloppy = false;                         // -s
  bool fake = false;                           // -f: validate, mount nothing
  bool no_mtab = false;                        // -n
  int verbose = 0;                             // -v, once per use
  bool version = false;                        // -V
  std::optional<std::string> mount_namespace;  // -N
  // Every -o, split at commas, in order.
  std::vector<std::string> options;
  // SOURCE as written, which the FUSE mount shows as its source (decision
  // 11) whatever is done to `source` (made absolute).
  std::string spec;
};

// Parses the arguments after argv[0]. InvalidArgument names the mistake (a
// usage error: MarkUsageError).
absl::StatusOr<HelperArgs> ParseHelperArgs(std::span<const std::string> args);

// The options of a mount, split. Everything not prefixed `dcfs.` is for the
// underlying mount, verbatim, in order.
struct HelperOptions {
  // How the backing filesystem is reached (`dcfs.fstype=`).
  enum class Backing {
    kNative,  // mounted in a private namespace, by type or autodetected
    // `dcfs.fstype=bind`: SOURCE is a directory, opened in place in the
    // caller's namespace (the mount that holds it stays reachable and is kept
    // busy; step 15.9 renamed this form from none, and removed the detached
    // clone that bind used to mean).
    kDirectory,
  };
  Backing backing = Backing::kNative;
  // `dcfs.fstype=<type>` for kNative; absent: mount(8) autodetects.
  std::optional<std::string> native_type;
  std::vector<std::string> native_options;
  bool read_only = false;   // dcfs.ro: the dcfs mount, not the backing one
  bool foreground = false;  // dcfs.foreground
  bool remount = false;     // the remount option: only the dcfs mount changes
  // dcfs.cache_db (step 15.3 derives a default from the instance identity).
  std::optional<std::string> cache_db;
  // dcfs.fuse_opt, once per use: libfuse mount options.
  std::vector<std::string> fuse_options;
  // The remaining dcfs.<flag>[=value] options, each naming an Abseil flag
  // of the binary (sync_interval_sec, stderrthreshold, ...); a bare boolean
  // is "true".
  std::vector<std::pair<std::string, std::string>> flags;
};

// InvalidArgument for an unknown `dcfs.` option, a missing or bad value, and
// for native options the mount cannot honor (dcfs.fstype=bind makes no
// underlying mount, a remount does not change it: `ro` there is `dcfs.ro`);
// Unimplemented for dcfs.cache_dir (step 15.3).
absl::StatusOr<HelperOptions> SplitHelperOptions(
    std::span<const std::string> options);

// The native options of a mount that makes no native mount of its own (a
// `dcfs.fstype=bind`, or a remount) that dcfs does not honor: all but what
// libmount adds to a helper's options or fstab says for mount(8) itself
// (rw, defaults, nofail, _netdev, noauto, auto, the user options, x-*).
// Empty for a native mount, which hands them to mount(8).
std::vector<std::string> UnhonoredNativeOptions(const HelperOptions &options);

// The payload that marks a status as a usage mistake or a refusal to run
// (an unknown option, not root): those exit 1, every other failure 32.
inline constexpr std::string_view kUsageTypeUrl =
    "rus.har.mn/dcfs/status/usage";

// `status` marked as a usage mistake or refusal (OK stays OK).
absl::Status MarkUsageError(absl::Status status);

// The payload of a NativeMountError: mount(8)'s exit status, in decimal.
inline constexpr std::string_view kMountExitStatusTypeUrl =
    "rus.har.mn/dcfs/status/mount_exit_status";

// The status of a failed native mount(8) that keeps its exit status, which
// the wrapper then exits with (FailedPrecondition, `message` is mount's own
// text).
absl::Status NativeMountError(int exit_status, std::string_view message);

// The wrapper's exit status, in mount(8)'s terms (it returns a helper's
// verbatim, and 1 is "incorrect invocation or permissions"): 1 for a status
// marked by MarkUsageError, the native mount's own for a NativeMountError,
// 32 (mount failure) for any other failed start, whatever its code (an
// errno-derived InvalidArgument from mount(2) is a failed start).
int ExitStatusFor(const absl::Status &status);

// Sets the Abseil flag `name` (one of the dcfs.<flag> options) from `value`.
// InvalidArgument for a value the flag does not parse, Internal for a name
// that is no flag.
absl::Status ApplyFlagOption(const std::string &name, const std::string &value);

// What the daemon tells the wrapper over the startup channel: "ready" once
// it answers FUSE_INIT, or the failure's exit status and message.
struct StartupReport {
  bool ready = false;
  int exit_status = 0;
  std::string message;
};
std::string EncodeStartupReport(const StartupReport &report);
// "" (the daemon died before reporting) and unintelligible bytes are
// failures with a message saying so.
StartupReport DecodeStartupReport(std::string_view bytes);

// The mount(2) flags for remounting the dcfs mount at `mountpoint`: the
// per-mount flags it has now (nosuid, nodev, noexec, noatime, nodiratime)
// plus MS_REMOUNT, and MS_RDONLY when `read_only`. nullopt if it is not a
// dcfs mount.
std::optional<unsigned long> RemountFlags(std::string_view mountinfo,
                                          std::string_view mountpoint,
                                          bool read_only);

}  // namespace dcfs

#endif  // DCFS_MOUNT_DCFS_H_
