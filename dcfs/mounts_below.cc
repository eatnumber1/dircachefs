#include "dcfs/mounts_below.h"

// Mount enumeration method chosen: /proc/self/mountinfo, not
// listmount(2)/statmount(2). Both would give the same answer for this
// startup-only, one-shot check, but mountinfo needs nothing beyond a
// read(2) of a file every Linux kernel has always exposed, while
// listmount/statmount are Linux 6.8+ and glibc may not wrap them yet
// (calling them would mean syscall(2) plus the raw UAPI structs from
// <linux/mount.h>). Since this check runs exactly once per daemon startup,
// there is no performance reason to prefer the newer, enumeration-by-id
// interface over parsing a small text file.

#include <fcntl.h>
#include <sys/stat.h>

#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <string_view>
#include <vector>

#include "absl/status/status.h"
#include "absl/status/status_macros.h"
#include "absl/status/statusor.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/str_join.h"
#include "absl/strings/str_split.h"
#include "dcfs/escape.h"
#include "dcfs/fd.h"
#include "dcfs/status.h"
#include "dcfs/syscalls.h"
#include "dcfs/syscalls_backing.h"

namespace dcfs {
namespace {

// Canonicalizes `path` (resolves "." / ".." / symlinks, relative to the
// current working directory, exactly as the openat(AT_FDCWD, path, ...)
// that opens --source does) so it can be compared byte-for-byte against
// the already-canonical paths /proc/self/mountinfo reports.
absl::StatusOr<std::string> Canonicalize(std::string_view path) {
  return syscalls::realpath(path);
}

// The whole contents of `fd`, read from its current offset to EOF.
absl::StatusOr<std::string> ReadAll(int fd) {
  std::string contents;
  char buf[65536];
  while (true) {
    ABSL_ASSIGN_OR_RETURN(size_t n, syscalls::read(fd, buf, sizeof(buf)));
    if (n == 0) return contents;
    contents.append(buf, n);
  }
}

}  // namespace

// Un-escapes the octal sequences (\040 space, \011 tab, \134 backslash,
// \012 newline) the kernel uses for the path fields (mount point, root) of
// /proc/self/mountinfo, so that a mount point containing one of those
// characters can't be mistaken for a field separator.
std::string UnescapeMountinfoPath(std::string_view field) {
  std::string result;
  result.reserve(field.size());
  for (size_t i = 0; i < field.size();) {
    if (field[i] == '\\' && i + 3 < field.size() && field[i + 1] >= '0' &&
        field[i + 1] <= '7' && field[i + 2] >= '0' && field[i + 2] <= '7' &&
        field[i + 3] >= '0' && field[i + 3] <= '7') {
      int value = (field[i + 1] - '0') * 64 + (field[i + 2] - '0') * 8 +
                  (field[i + 3] - '0');
      result.push_back(static_cast<char>(value));
      i += 4;
    } else {
      result.push_back(field[i]);
      ++i;
    }
  }
  return result;
}

std::vector<std::string> MountPointsBelow(std::string_view mountinfo,
                                          std::string_view source) {
  // The root filesystem is its own special case: every other mount's point
  // trivially "starts with" "/", so the ordinary `source + "/"` prefix
  // (which would double the slash) is skipped in favor of "/" itself.
  const std::string prefix = source == "/" ? "/" : absl::StrCat(source, "/");

  std::vector<std::string> below;
  for (std::string_view line : absl::StrSplit(mountinfo, '\n')) {
    if (line.empty()) continue;
    // Fields: (1) mount id, (2) parent id, (3) major:minor, (4) root,
    // (5) mount point, (6) mount options, (7...) optional fields, "-",
    // fstype, mount source, super options. Only field 5 is needed, and its
    // position is fixed regardless of how many optional fields precede the
    // "-" separator, so the line is split just far enough to reach it.
    std::vector<std::string_view> fields =
        absl::StrSplit(line, ' ', absl::SkipEmpty());
    if (fields.size() < 5) continue;  // Malformed; ignore rather than fail.
    std::string mount_point = UnescapeMountinfoPath(fields[4]);
    if (mount_point == source) continue;  // A mount ON the source: fine.
    if (mount_point.size() > prefix.size() &&
        std::string_view(mount_point).substr(0, prefix.size()) == prefix) {
      below.push_back(std::move(mount_point));
    }
  }
  return below;
}

absl::StatusOr<std::vector<std::string>> MountsBelow(
    std::string_view source_path) {
  ABSL_ASSIGN_OR_RETURN(std::string source, Canonicalize(source_path));
  ABSL_ASSIGN_OR_RETURN(
      FileDescriptor mountinfo,
      syscalls::openat(AT_FDCWD, "/proc/self/mountinfo", O_RDONLY));
  ABSL_ASSIGN_OR_RETURN(std::string contents, ReadAll(*mountinfo));
  return MountPointsBelow(contents, source);
}

namespace {

// Whether the comma-separated `options` include "ro", or, with
// `emergency`, ext4's "emergency_ro" (Linux 6.15: an error made it
// read-only without marking the superblock so).
bool HasRo(std::string_view options, bool emergency = false) {
  for (std::string_view option : absl::StrSplit(options, ',')) {
    if (option == "ro" || (emergency && option == "emergency_ro")) {
      return true;
    }
  }
  return false;
}

}  // namespace

bool ForcedReadOnlyIn(std::string_view mountinfo, uint64_t mount_id) {
  const std::string id = absl::StrCat(mount_id);
  for (std::string_view line : absl::StrSplit(mountinfo, '\n')) {
    // Fields: (1) mount id, ..., (6) mount options, (7...) optional fields,
    // "-", fstype, mount source, super options.
    std::vector<std::string_view> fields =
        absl::StrSplit(line, ' ', absl::SkipEmpty());
    if (fields.size() < 6 || fields[0] != id) continue;
    for (size_t i = 6; i + 3 < fields.size(); ++i) {
      if (fields[i] != "-") continue;
      return !HasRo(fields[5]) && HasRo(fields[i + 3], /*emergency=*/true);
    }
    return false;
  }
  return false;
}

absl::StatusOr<bool> ForcedReadOnly(int fd) {
  ABSL_ASSIGN_OR_RETURN(struct statx stx,
                        syscalls::statx(fd, "", AT_EMPTY_PATH, STATX_MNT_ID));
  if ((stx.stx_mask & STATX_MNT_ID) == 0) return false;
  ABSL_ASSIGN_OR_RETURN(
      FileDescriptor mountinfo,
      syscalls::openat(AT_FDCWD, "/proc/self/mountinfo", O_RDONLY));
  ABSL_ASSIGN_OR_RETURN(std::string contents, ReadAll(*mountinfo));
  return ForcedReadOnlyIn(contents, stx.stx_mnt_id);
}

absl::Status RefuseMountsBelow(std::string_view source_path) {
  ABSL_ASSIGN_OR_RETURN(std::vector<std::string> below,
                        MountsBelow(source_path));
  if (below.empty()) return absl::OkStatus();
  return FailedPreconditionErrorBuilder()
         << "dcfs does not yet support filesystems mounted below SOURCE: "
            "their inode numbers would collide under one st_dev; unmount "
            "them or point SOURCE elsewhere. Mounted below "
         << source_path << ": " << absl::StrJoin(below, ", ");
}

absl::Status RefuseIfForcedReadOnly(int fd, std::string_view source) {
  ABSL_ASSIGN_OR_RETURN(bool forced_read_only, ForcedReadOnly(fd));
  if (!forced_read_only) return absl::OkStatus();
  return FailedPreconditionErrorBuilder()
         << "SOURCE " << source
         << " is on a filesystem whose superblock is read-only under a "
            "read-write mount: after an error, what it shows may not be on "
            "its disk, or another mount of it was remounted read-only; "
            "refusing to start: after an error, unmount it, check it and "
            "mount it again; otherwise remount it read-write, or mount "
            "SOURCE read-only";
}

}  // namespace dcfs
