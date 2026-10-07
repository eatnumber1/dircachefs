// dcfs benchmarks (phase 10): google/benchmark cases run inside the QEMU
// guest (test/qemu/guest/bench_*.sh start it), plus two helper subcommands
// the guest scripts use:
//
//   dcfs_bench mktree ROOT ENTRIES BIG     create the benchmark tree
//   dcfs_bench dm-delay NAME DEV MS        dm "delay" device over DEV; prints
//                                          the device node
//
// Anything else is a benchmark run; this binary's own flags (below) are
// removed before google/benchmark sees the command line.
//
// Targets: the same operations run on the backing filesystem directly
// ("backing"), through dcfs with the default one-hour kernel timeouts
// ("dcfs": the kernel answers repeats itself) and through dcfs with both
// timeouts zero ("dcfs0": every operation reaches dcfs and its cache). With
// --slow_src there are three more, "slow_*", over a backing filesystem on a
// device-mapper delay device. Cycling operations walk the entries in an
// order that touches a new inode block each time and drop the kernel's
// caches (untimed) whenever they wrap, so the direct cases on the slow
// device pay its latency and the cached dcfs cases must not.
#include <benchmark/benchmark.h>
#include <fcntl.h>
#include <sys/stat.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "bench/dm_delay.h"
#include "bench/process.h"
#include "bench/tree.h"
#include "dcfs/fd.h"
#include "dcfs/syscalls.h"

namespace dcfs_bench {
namespace {

struct Config {
  std::string dcfs = "/bin/dcfs";
  std::string src = "/src";
  std::string slow_src;
  std::string mnt_dir = "/mnt";
  std::string cache_dir = "/cache";
  uint64_t entries = 100000;
  uint64_t slow_entries = 2000;
  uint64_t big = 10000;
  uint64_t dirty = 10000;
  bool smoke = false;
};
Config cfg;

struct Target {
  std::string name;
  std::string root;  // where the operations run
  uint64_t entries;
};
std::vector<Target> targets;
std::vector<std::unique_ptr<DcfsProcess>> daemons;
std::map<std::string, DcfsProcess *> by_name;

uint64_t Stride(uint64_t n) {
  // Coprime to n, larger than an inode block's worth of entries (a 4 KiB
  // block holds 16 256-byte ext4 inodes), so consecutive operations hit
  // different blocks.
  uint64_t s = 97;
  while (n % s == 0) s += 2;
  return s;
}

namespace syscalls = dcfs::syscalls;
using dcfs::FileDescriptor;

// Calls `visit(name)` for every entry of the directory `dir_fd` except "."
// and "..", read with getdents64. Returns false if the directory cannot be
// read.
template <typename Visit>
bool ForEachEntry(int dir_fd, Visit visit) {
  std::vector<char> buf(32768);
  while (true) {
    absl::StatusOr<ssize_t> n =
        syscalls::getdents64(dir_fd, buf.data(), buf.size());
    if (!n.ok()) return false;
    if (*n == 0) return true;
    for (ssize_t pos = 0; pos < *n;) {
      const auto *entry =
          reinterpret_cast<const syscalls::linux_dirent64 *>(buf.data() + pos);
      pos += entry->d_reclen;
      const std::string_view name(entry->d_name);
      if (name == "." || name == "..") continue;
      visit(name);
    }
  }
}

// The objects at and below directory `dir_fd`, each lstat'ed as nftw's
// FTW_PHYS walk does; adds them to `walked`.
void WalkDir(int dir_fd, uint64_t &walked) {
  std::vector<std::string> subdirs;
  ForEachEntry(dir_fd, [&](std::string_view name) {
    ++walked;
    absl::StatusOr<struct stat> st =
        syscalls::fstatat(dir_fd, name, AT_SYMLINK_NOFOLLOW);
    if (st.ok() && S_ISDIR(st->st_mode)) subdirs.emplace_back(name);
  });
  for (const std::string &name : subdirs) {
    absl::StatusOr<FileDescriptor> sub = syscalls::openat(
        dir_fd, name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW);
    if (sub.ok()) WalkDir(**sub, walked);
  }
}

// `find ROOT` through the filesystem under test; returns the number of
// objects found.
uint64_t Walk(const std::string &root) {
  uint64_t walked = 1;  // ROOT itself
  absl::StatusOr<FileDescriptor> dir =
      syscalls::openat(AT_FDCWD, root, O_RDONLY | O_DIRECTORY);
  if (dir.ok()) WalkDir(**dir, walked);
  return walked;
}

bool StartDcfs(
    const std::string &name, const std::string &src,
    const std::vector<std::string> &flags, bool fresh_db = true) {
  auto d = std::make_unique<DcfsProcess>();
  std::string mnt = cfg.mnt_dir + "/" + name;
  std::string db = cfg.cache_dir + "/" + name + ".db";
  if (fresh_db) {
    for (const char *suffix : {"", "-wal", "-shm"}) {
      syscalls::unlinkat(AT_FDCWD, db + suffix, 0).IgnoreError();
    }
  }
  syscalls::mkdirat(AT_FDCWD, mnt, 0755).IgnoreError();
  if (!d->Start(cfg.dcfs, src, db, mnt, flags) || !d->WaitMounted(60)) {
    fprintf(stderr, "dcfs %s did not mount (see /tmp/dcfs-bench.log)\n",
            name.c_str());
    return false;
  }
  by_name[name] = d.get();
  daemons.push_back(std::move(d));
  return true;
}

bool Setup() {
  struct Backing {
    std::string prefix, src;
    uint64_t entries;
  };
  std::vector<Backing> backings = {{"", cfg.src, cfg.entries}};
  if (!cfg.slow_src.empty()) {
    backings.push_back({"slow_", cfg.slow_src, cfg.slow_entries});
  }
  for (const auto &b : backings) {
    uint64_t big = std::min(cfg.big, b.entries);
    if (!MakeTree(b.src, b.entries, big)) return false;
    syscalls::sync();
    targets.push_back({b.prefix + "backing", b.src, b.entries});
    for (const char *kind : {"dcfs", "dcfs0"}) {
      std::string name = b.prefix + kind;
      std::vector<std::string> flags;
      if (std::string(kind) == "dcfs0") {
        flags = {"--attr_timeout_sec=0", "--entry_timeout_sec=0"};
      }
      if (!StartDcfs(name, b.src, flags)) return false;
      std::string mnt = cfg.mnt_dir + "/" + name;
      Walk(mnt);  // warm: dcfs's database now has every entry.
      targets.push_back({name, mnt, b.entries});
    }
  }
  return true;
}

// -- cycling operations -------------------------------------------------

template <typename Op>
void Cycle(benchmark::State &state, const Target &t, Op op) {
  const uint64_t n = t.entries;
  const uint64_t stride = Stride(n);
  uint64_t i = 0, count = 0;
  DropCaches(3);
  for (auto _ : state) {
    std::string path = t.root + "/" + EntryPath((i * stride) % n);
    if (!op(path)) {
      state.SkipWithError(("operation failed on " + path).c_str());
      return;
    }
    ++i;
    if (++count % n == 0) {
      state.PauseTiming();
      DropCaches(3);
      state.ResumeTiming();
    }
  }
}

void BM_Stat(benchmark::State &state, const Target *t) {
  Cycle(state, *t, [](const std::string &p) {
    return syscalls::fstatat(AT_FDCWD, p).ok();
  });
}

void BM_OpenClose(benchmark::State &state, const Target *t) {
  Cycle(state, *t, [](const std::string &p) {
    return syscalls::openat(AT_FDCWD, p, O_RDONLY).ok();
  });
}

void BM_SmallRead(benchmark::State &state, const Target *t) {
  Cycle(state, *t, [](const std::string &p) {
    absl::StatusOr<FileDescriptor> fd =
        syscalls::openat(AT_FDCWD, p, O_RDONLY);
    if (!fd.ok()) return false;
    char buf[4096];
    absl::StatusOr<size_t> r = syscalls::pread(**fd, buf, sizeof buf, 0);
    return r.ok() && *r == kFileBytes;
  });
}

// An eight-component path walk, cold in the kernel (and the backing
// filesystem's) caches each time.
void BM_Lookup(benchmark::State &state, const Target *t) {
  std::string p = t->root + "/" + DeepPath();
  for (auto _ : state) {
    state.PauseTiming();
    DropCaches(3);
    state.ResumeTiming();
    if (!syscalls::fstatat(AT_FDCWD, p).ok()) {
      state.SkipWithError("stat failed");
      return;
    }
  }
}

// readdir of the large directory, cold.
void BM_Readdir(benchmark::State &state, const Target *t) {
  std::string p = t->root + "/big";
  uint64_t seen = 0;
  for (auto _ : state) {
    state.PauseTiming();
    DropCaches(3);
    state.ResumeTiming();
    absl::StatusOr<FileDescriptor> d =
        syscalls::openat(AT_FDCWD, p, O_RDONLY | O_DIRECTORY);
    if (!d.ok()) {
      state.SkipWithError("opening the directory failed");
      return;
    }
    seen = 2;  // "." and ".."
    if (!ForEachEntry(**d, [&](std::string_view) { ++seen; })) {
      state.SkipWithError("getdents64 failed");
      return;
    }
  }
  state.counters["entries"] = static_cast<double>(seen);
}

// -- whole-daemon cases --------------------------------------------------

// Time from exec to the first stat served, on the warm database of the
// "dcfs" instance (cfg.entries entries).
void BM_Startup(benchmark::State &state) {
  DcfsProcess *d = by_name["dcfs"];
  std::string db = cfg.cache_dir + "/dcfs.db";
  std::string mnt = d->mnt();
  d->Stop();
  for (auto _ : state) {
    DcfsProcess p;
    bool ok = p.Start(cfg.dcfs, cfg.src, db, mnt, {}) && p.WaitMounted(120) &&
              syscalls::fstatat(AT_FDCWD, mnt + "/" + DeepPath()).ok();
    state.PauseTiming();
    p.Stop();
    state.ResumeTiming();
    if (!ok) {
      state.SkipWithError("startup failed");
      return;
    }
  }
  state.counters["entries"] = static_cast<double>(cfg.entries);
}

// Time to restart after SIGKILL with `dirty` mutations not yet synced.
void BM_Recovery(benchmark::State &state) {
  std::string mnt = cfg.mnt_dir + "/recovery";
  std::string db = cfg.cache_dir + "/recovery.db";
  syscalls::mkdirat(AT_FDCWD, mnt, 0755).IgnoreError();
  syscalls::unlinkat(AT_FDCWD, db, 0).IgnoreError();
  int round = 0;
  for (auto _ : state) {
    state.PauseTiming();
    DcfsProcess p;
    bool ok = p.Start(
                  cfg.dcfs, cfg.src, db, mnt,
                  {"--sync_interval_sec=100000"}) &&
              p.WaitMounted(120);
    if (ok) {
      std::string dir = mnt + "/recovery" + std::to_string(round++);
      ok = syscalls::mkdirat(AT_FDCWD, dir, 0755).ok();
      for (uint64_t i = 0; ok && i < cfg.dirty; ++i) {
        ok = syscalls::openat(AT_FDCWD, dir + "/f" + std::to_string(i),
                              O_WRONLY | O_CREAT, 0644)
                 .ok();
      }
    }
    p.Crash();
    state.ResumeTiming();
    DcfsProcess q;
    ok = ok && q.Start(cfg.dcfs, cfg.src, db, mnt, {}) &&
         q.WaitMounted(300) && syscalls::fstatat(AT_FDCWD, mnt).ok();
    state.PauseTiming();
    q.Stop();
    state.ResumeTiming();
    if (!ok) {
      state.SkipWithError("recovery run failed");
      return;
    }
  }
  state.counters["dirty"] = static_cast<double>(cfg.dirty);
}

// Memory: RSS after `find`, and after the kernel drops its inode cache.
void BM_Memory(benchmark::State &state) {
  for (auto _ : state) {
    state.PauseTiming();
    if (!StartDcfs("memory", cfg.src, {})) {
      state.SkipWithError("mount failed");
      return;
    }
    DcfsProcess *d = by_name["memory"];
    uint64_t rss0 = RssBytes(d->pid());
    state.ResumeTiming();
    uint64_t found = Walk(d->mnt());
    state.PauseTiming();
    uint64_t rss1 = RssBytes(d->pid());
    DropCaches(2);
    SleepMicros(2'000'000);
    uint64_t rss2 = RssBytes(d->pid());
    d->Stop();
    state.counters["rss_mount_MiB"] = static_cast<double>(rss0) / 1048576;
    state.counters["rss_find_MiB"] = static_cast<double>(rss1) / 1048576;
    state.counters["rss_dropped_MiB"] = static_cast<double>(rss2) / 1048576;
    state.counters["bytes_per_inode"] =
        static_cast<double>(rss1 - rss0) / static_cast<double>(found);
    state.counters["objects"] = static_cast<double>(found);
    state.ResumeTiming();
  }
}

void Register() {
  using Fn = void (*)(benchmark::State &, const Target *);
  struct Case {
    const char *name;
    Fn fn;
    int full_iterations;  // 0: let google/benchmark choose
  };
  const Case cases[] = {
      {"Stat", BM_Stat, 0},         {"OpenClose", BM_OpenClose, 0},
      {"SmallRead", BM_SmallRead, 0}, {"Lookup", BM_Lookup, 200},
      {"Readdir", BM_Readdir, 20},
  };
  for (const Case &c : cases) {
    for (const Target &t : targets) {
      auto *b = benchmark::RegisterBenchmark(
          (std::string(c.name) + "/" + t.name).c_str(), c.fn, &t);
      b->Unit(benchmark::kMicrosecond);
      if (cfg.smoke) {
        b->Iterations(1);
      } else if (c.full_iterations != 0) {
        b->Iterations(c.full_iterations);
      }
    }
  }
  auto whole = [](const char *name, void (*fn)(benchmark::State &), int n) {
    auto *b = benchmark::RegisterBenchmark(name, fn);
    b->Unit(benchmark::kMillisecond)->Iterations(cfg.smoke ? 1 : n);
  };
  whole("Startup", BM_Startup, 3);
  whole("Recovery", BM_Recovery, 3);
  whole("Memory", BM_Memory, 1);
}

// Removes `--name=value` flags this file owns from argv; true if `name`
// matched.
bool TakeFlag(const char *arg, const char *name, std::string *value) {
  size_t n = strlen(name);
  if (strncmp(arg, "--", 2) != 0 || strncmp(arg + 2, name, n) != 0) {
    return false;
  }
  if (arg[2 + n] == '=') {
    *value = arg + 3 + n;
    return true;
  }
  if (arg[2 + n] == '\0') {
    value->clear();
    return true;
  }
  return false;
}

void ParseOwnFlags(int *argc, char **argv) {
  int out = 1;
  for (int i = 1; i < *argc; ++i) {
    std::string v;
    if (TakeFlag(argv[i], "dcfs", &v)) cfg.dcfs = v;
    else if (TakeFlag(argv[i], "src", &v)) cfg.src = v;
    else if (TakeFlag(argv[i], "slow_src", &v)) cfg.slow_src = v;
    else if (TakeFlag(argv[i], "mnt_dir", &v)) cfg.mnt_dir = v;
    else if (TakeFlag(argv[i], "cache_dir", &v)) cfg.cache_dir = v;
    else if (TakeFlag(argv[i], "entries", &v)) cfg.entries = strtoull(v.c_str(), nullptr, 10);
    else if (TakeFlag(argv[i], "slow_entries", &v)) cfg.slow_entries = strtoull(v.c_str(), nullptr, 10);
    else if (TakeFlag(argv[i], "big", &v)) cfg.big = strtoull(v.c_str(), nullptr, 10);
    else if (TakeFlag(argv[i], "dirty", &v)) cfg.dirty = strtoull(v.c_str(), nullptr, 10);
    else if (TakeFlag(argv[i], "smoke", &v)) cfg.smoke = true;
    else argv[out++] = argv[i];
  }
  *argc = out;
}

}  // namespace
}  // namespace dcfs_bench

int main(int argc, char **argv) {
  if (argc == 5 && strcmp(argv[1], "mktree") == 0) {
    return dcfs_bench::MakeTree(argv[2], strtoull(argv[3], nullptr, 10),
                    strtoull(argv[4], nullptr, 10))
               ? 0
               : 1;
  }
  if (argc == 5 && strcmp(argv[1], "dm-delay") == 0) {
    std::string node = dcfs_bench::CreateDelayDevice(argv[2], argv[3], atoi(argv[4]));
    if (node.empty()) return 1;
    printf("%s\n", node.c_str());
    return 0;
  }
  dcfs_bench::ParseOwnFlags(&argc, argv);
  benchmark::Initialize(&argc, argv);
  int rc = 0;
  if (!dcfs_bench::Setup()) {
    rc = 1;
  } else {
    dcfs_bench::Register();
    benchmark::RunSpecifiedBenchmarks();
  }
  for (auto &d : dcfs_bench::daemons) d->Stop();
  benchmark::Shutdown();
  return rc;
}
