#include "bench/tree.h"

#include <fcntl.h>
#include <sys/stat.h>

#include <cerrno>
#include <cstdio>
#include <cstring>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "dcfs/fd.h"
#include "dcfs/status.h"
#include "dcfs/syscalls_backing.h"

namespace dcfs_bench {

std::string EntryPath(uint64_t i) {
  char buf[64];
  snprintf(
      buf, sizeof buf, "t/%03u/%02u/f%02u", static_cast<unsigned>(i / 10000),
      static_cast<unsigned>((i / 100) % 100), static_cast<unsigned>(i % 100));
  return buf;
}

std::string BigPath(uint64_t j) {
  char buf[64];
  snprintf(buf, sizeof buf, "big/f%06u", static_cast<unsigned>(j));
  return buf;
}

namespace {

bool MkdirP(int rootfd, const std::string &rel) {
  std::string cur;
  size_t pos = 0;
  while (pos <= rel.size()) {
    size_t next = rel.find('/', pos);
    if (next == std::string::npos) next = rel.size();
    cur = rel.substr(0, next);
    if (!cur.empty()) {
      absl::Status made = dcfs::syscalls::mkdirat(rootfd, cur, 0755);
      if (!made.ok() && dcfs::StatusToErrno(made) != EEXIST) {
        fprintf(stderr, "mkdir %s: %s\n", cur.c_str(),
                made.ToString().c_str());
        return false;
      }
    }
    pos = next + 1;
  }
  return true;
}

bool WriteFile(int rootfd, const std::string &rel) {
  absl::StatusOr<dcfs::FileDescriptor> fd = dcfs::syscalls::openat(
      rootfd, rel, O_WRONLY | O_CREAT | O_TRUNC, 0644);
  if (!fd.ok()) {
    fprintf(stderr, "create %s: %s\n", rel.c_str(),
            fd.status().ToString().c_str());
    return false;
  }
  char data[kFileBytes];
  memset(data, 'x', sizeof data);
  absl::StatusOr<size_t> n = dcfs::syscalls::write(**fd, data, sizeof data);
  const bool ok = n.ok() && *n == sizeof data;
  if (!ok) {
    fprintf(stderr, "write %s: wrote %zu of %zu bytes: %s\n", rel.c_str(),
            n.ok() ? *n : size_t{0}, sizeof data,
            n.status().ToString().c_str());
  }
  return ok;
}

}  // namespace

bool MakeTree(const std::string &root, uint64_t entries, uint64_t big) {
  absl::StatusOr<dcfs::FileDescriptor> root_fd =
      dcfs::syscalls::openat(AT_FDCWD, root, O_RDONLY | O_DIRECTORY);
  if (!root_fd.ok()) {
    fprintf(stderr, "open %s: %s\n", root.c_str(),
            root_fd.status().ToString().c_str());
    return false;
  }
  const int rootfd = **root_fd;
  bool ok = MkdirP(rootfd, "t") && MkdirP(rootfd, "big") &&
            MkdirP(rootfd, "deep/l1/l2/l3/l4/l5/l6") &&
            WriteFile(rootfd, DeepPath());
  std::string last_dir;
  for (uint64_t i = 0; ok && i < entries; ++i) {
    std::string p = EntryPath(i);
    std::string dir = p.substr(0, p.rfind('/'));
    if (dir != last_dir) {
      ok = MkdirP(rootfd, dir);
      last_dir = dir;
    }
    ok = ok && WriteFile(rootfd, p);
  }
  for (uint64_t j = 0; ok && j < big; ++j) ok = WriteFile(rootfd, BigPath(j));
  return ok;
}

}  // namespace dcfs_bench
