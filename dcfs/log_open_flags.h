#ifndef DCFS_LOG_OPEN_FLAGS_H_
#define DCFS_LOG_OPEN_FLAGS_H_

#include <fcntl.h>

#include <string>
#include <string_view>
#include <vector>

#include "absl/container/flat_hash_map.h"
#include "absl/strings/str_format.h"
#include "absl/strings/str_join.h"

namespace dcfs {

// Wraps open(2) flags so that logging or StrCat-ing one prints the names of
// the flags set (`O_RDWR | O_CREAT`) instead of a number:
//
//   LOG(INFO) << "opening with " << LogOpenFlags(flags);
//
// The names come out in no particular order. AbslStringify is a hidden
// friend (docs/style.md 1.2): defined in the class, found by ADL.
class LogOpenFlags {
 public:
  explicit LogOpenFlags(int flags) : flags_(flags) {}

  template <typename Sink>
  friend void AbslStringify(Sink &sink, const LogOpenFlags &l) {
    static const absl::flat_hash_map<int, const std::string> kFlagsToNames {
#define F(n) {n, #n}
#ifdef O_ACCMODE
        F(O_ACCMODE),
#endif  // O_ACCMODE
#ifdef O_RDONLY
        F(O_RDONLY),
#endif  // O_RDONLY
#ifdef O_WRONLY
        F(O_WRONLY),
#endif  // O_WRONLY
#ifdef O_RDWR
        F(O_RDWR),
#endif  // O_RDWR
#ifdef O_CREAT
        F(O_CREAT),
#endif  // O_CREAT
#ifdef O_EXCL
        F(O_EXCL),
#endif  // O_EXCL
#ifdef O_NOCTTY
        F(O_NOCTTY),
#endif  // O_NOCTTY
#ifdef O_TRUNC
        F(O_TRUNC),
#endif  // O_TRUNC
#ifdef O_APPEND
        F(O_APPEND),
#endif  // O_APPEND
#ifdef O_NONBLOCK
        F(O_NONBLOCK),
#endif  // O_NONBLOCK
#ifdef O_DSYNC
        F(O_DSYNC),
#endif  // O_DSYNC
#ifdef FASYNC
        F(FASYNC),
#endif  // FASYNC
#ifdef O_DIRECT
        F(O_DIRECT),
#endif  // O_DIRECT
#ifdef O_LARGEFILE
        F(O_LARGEFILE),
#endif  // O_LARGEFILE
#ifdef O_DIRECTORY
        F(O_DIRECTORY),
#endif  // O_DIRECTORY
#ifdef O_NOFOLLOW
        F(O_NOFOLLOW),
#endif  // O_NOFOLLOW
#ifdef O_NOATIME
        F(O_NOATIME),
#endif  // O_NOATIME
#ifdef O_CLOEXEC
        F(O_CLOEXEC),
#endif  // O_CLOEXEC
#undef F
    };

    std::vector<std::string_view> flag_names;
    int flags = l.flags_;
    for (const auto &[flag, name] : kFlagsToNames) {
      if ((flags & flag) == 0) continue;
      flag_names.emplace_back(name);
    }
    absl::Format(&sink, "%s", absl::StrJoin(flag_names, " | "));
  }

 private:
  int flags_ = 0;
};

}  // namespace dcfs

#endif  // DCFS_LOG_OPEN_FLAGS_H_
