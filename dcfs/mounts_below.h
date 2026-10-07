#ifndef DCFS_MOUNTS_BELOW_H_
#define DCFS_MOUNTS_BELOW_H_

#include <string>
#include <string_view>
#include <vector>

#include "absl/status/statusor.h"

// A standalone startup policy check (see amendment 12 in the plan and the
// README's Limitations): dcfs requires exactly one backing filesystem below
// --source, since backing inode numbers (shown to users as st_ino) are only
// unambiguous within one st_dev. This never touches dcfs's own identity
// machinery (Context, the cache, MountFds) -- it runs once, before any of
// that is even opened, purely to decide whether to refuse to start.
namespace dcfs {

// The mount points, as absolute paths, of every mount in this process's
// mount namespace whose mount point lies strictly below `source_path`
// (i.e. starts with the canonicalized `source_path` plus "/"). A mount
// point equal to `source_path` itself -- a bind mount or another
// filesystem mounted directly onto the source, which is what --source
// always names -- is not "below" it and is not reported. Empty (not an
// error) when nothing is mounted below `source_path`.
//
// Reads /proc/self/mountinfo rather than using listmount(2)/statmount(2)
// (Linux 6.8+; see the header comment in mounts_below.cc for why): both
// give the same answer here, and mountinfo needs no new syscalls this
// glibc/kernel combination might lack a wrapper for.
absl::StatusOr<std::vector<std::string>> MountsBelow(
    std::string_view source_path);

// The parsing half of MountsBelow, on the text of /proc/self/mountinfo and
// an already canonical `source` (so it can be tested on canned input):
// every mount point lying strictly below `source`, in file order, with the
// kernel's octal escapes (\040 space, \011 tab, \012 newline, \134
// backslash) decoded. A line with fewer than the five leading fields is
// ignored, not an error.
std::vector<std::string> MountPointsBelow(std::string_view mountinfo,
                                          std::string_view source);

}  // namespace dcfs

#endif  // DCFS_MOUNTS_BELOW_H_
