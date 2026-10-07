#ifndef DCFS_TESTONLY_FILES_H_
#define DCFS_TESTONLY_FILES_H_

#include <fcntl.h>

#include <sys/stat.h>

#include <algorithm>
#include <string>
#include <string_view>
#include <vector>

#include "absl/status/status_macros.h"
#include "absl/status/statusor.h"
#include "dcfs/fd.h"
#include "dcfs/syscalls_backing.h"

namespace dcfs::testonly {

// The whole contents of the file at `path`, read through the syscalls::
// wrappers (for /proc files and small test files).
inline absl::StatusOr<std::string> ReadFileToString(std::string_view path) {
  ABSL_ASSIGN_OR_RETURN(FileDescriptor fd,
                        syscalls::openat(AT_FDCWD, path, O_RDONLY));
  std::string contents;
  char buf[4096];
  while (true) {
    ABSL_ASSIGN_OR_RETURN(size_t n, syscalls::read(*fd, buf, sizeof(buf)));
    if (n == 0) return contents;
    contents.append(buf, n);
  }
}

// The names in the directory at `path` (without "." and ".."), sorted, read
// with getdents64.
inline absl::StatusOr<std::vector<std::string>> ListDirectory(
    std::string_view path) {
  ABSL_ASSIGN_OR_RETURN(
      FileDescriptor dir,
      syscalls::openat(AT_FDCWD, path, O_RDONLY | O_DIRECTORY));
  std::vector<std::string> names;
  std::vector<char> buf(32768);
  while (true) {
    ABSL_ASSIGN_OR_RETURN(
        ssize_t n, syscalls::getdents64(*dir, buf.data(), buf.size()));
    if (n == 0) break;
    for (ssize_t pos = 0; pos < n;) {
      const auto *entry =
          reinterpret_cast<const syscalls::linux_dirent64 *>(buf.data() + pos);
      pos += entry->d_reclen;
      const std::string_view name(entry->d_name);
      if (name != "." && name != "..") names.emplace_back(name);
    }
  }
  std::sort(names.begin(), names.end());
  return names;
}

// Every path below the directory `path` (symlinks are not followed), each
// directory before its contents.
inline absl::StatusOr<std::vector<std::string>> ListTree(
    std::string_view path) {
  ABSL_ASSIGN_OR_RETURN(std::vector<std::string> names, ListDirectory(path));
  std::vector<std::string> all;
  for (const std::string &name : names) {
    const std::string child = std::string(path) + "/" + name;
    all.push_back(child);
    ABSL_ASSIGN_OR_RETURN(
        struct stat st,
        syscalls::fstatat(AT_FDCWD, child, AT_SYMLINK_NOFOLLOW));
    if (S_ISDIR(st.st_mode)) {
      ABSL_ASSIGN_OR_RETURN(std::vector<std::string> below, ListTree(child));
      all.insert(all.end(), below.begin(), below.end());
    }
  }
  return all;
}

// Removes `path` and everything below it, best effort (for TearDown).
inline void RemoveAll(std::string_view path) {
  absl::StatusOr<std::vector<std::string>> tree = ListTree(path);
  if (tree.ok()) {
    for (auto it = tree->rbegin(); it != tree->rend(); ++it) {
      absl::StatusOr<struct stat> st =
          syscalls::fstatat(AT_FDCWD, *it, AT_SYMLINK_NOFOLLOW);
      const int flags = st.ok() && S_ISDIR(st->st_mode) ? AT_REMOVEDIR : 0;
      syscalls::unlinkat(AT_FDCWD, *it, flags).IgnoreError();
    }
  }
  syscalls::unlinkat(AT_FDCWD, path, AT_REMOVEDIR).IgnoreError();
  syscalls::unlinkat(AT_FDCWD, path, 0).IgnoreError();
}

// The size in bytes of the file at `path`.
inline absl::StatusOr<off_t> FileSize(std::string_view path) {
  ABSL_ASSIGN_OR_RETURN(struct stat st, syscalls::fstatat(AT_FDCWD, path));
  return st.st_size;
}

}  // namespace dcfs::testonly

#endif  // DCFS_TESTONLY_FILES_H_
