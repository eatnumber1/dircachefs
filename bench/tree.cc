#include "bench/tree.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstring>

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
    if (!cur.empty() && mkdirat(rootfd, cur.c_str(), 0755) != 0 &&
        errno != EEXIST) {
      fprintf(stderr, "mkdir %s: %s\n", cur.c_str(), strerror(errno));
      return false;
    }
    pos = next + 1;
  }
  return true;
}

bool WriteFile(int rootfd, const std::string &rel) {
  int fd = openat(rootfd, rel.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
  if (fd < 0) {
    fprintf(stderr, "create %s: %s\n", rel.c_str(), strerror(errno));
    return false;
  }
  char data[kFileBytes];
  memset(data, 'x', sizeof data);
  bool ok = write(fd, data, sizeof data) == static_cast<ssize_t>(sizeof data);
  if (!ok) fprintf(stderr, "write %s: %s\n", rel.c_str(), strerror(errno));
  close(fd);
  return ok;
}

}  // namespace

bool MakeTree(const std::string &root, uint64_t entries, uint64_t big) {
  int rootfd = open(root.c_str(), O_RDONLY | O_DIRECTORY);
  if (rootfd < 0) {
    fprintf(stderr, "open %s: %s\n", root.c_str(), strerror(errno));
    return false;
  }
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
  close(rootfd);
  return ok;
}

}  // namespace dcfs_bench
