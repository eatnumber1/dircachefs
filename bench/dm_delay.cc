#include "bench/dm_delay.h"

#include <fcntl.h>
#include <linux/dm-ioctl.h>
#include <linux/fs.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>

#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <utility>
#include <vector>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "bench/process.h"
#include "dcfs/fd.h"
#include "dcfs/syscalls.h"

namespace dcfs_bench {
namespace {

using dcfs::FileDescriptor;
namespace syscalls = dcfs::syscalls;

absl::StatusOr<FileDescriptor> OpenControl() {
  absl::StatusOr<FileDescriptor> fd =
      syscalls::openat(AT_FDCWD, "/dev/mapper/control", O_RDWR);
  if (fd.ok()) return fd;
  // devtmpfs makes the node for the misc device on its own; this is the
  // fallback for a guest where it did not.
  std::ifstream misc("/proc/misc");
  unsigned minor;
  std::string name;
  while (misc >> minor >> name) {
    if (name == "device-mapper") {
      syscalls::mkdirat(AT_FDCWD, "/dev/mapper", 0755).IgnoreError();
      absl::Status made = syscalls::mknodat(
          AT_FDCWD, "/dev/mapper/control", S_IFCHR | 0600, makedev(10, minor));
      if (!made.ok()) return made;
      return syscalls::openat(AT_FDCWD, "/dev/mapper/control", O_RDWR);
    }
  }
  return fd.status();
}

void Init(dm_ioctl *io, size_t size, const std::string &name) {
  memset(io, 0, size);
  io->version[0] = 4;
  io->version[1] = 0;
  io->version[2] = 0;
  io->data_size = static_cast<uint32_t>(size);
  io->data_start = sizeof(dm_ioctl);
  snprintf(io->name, sizeof io->name, "%s", name.c_str());
}

bool Exists(const char *path) {
  return syscalls::fstatat(AT_FDCWD, path).ok();
}

// Reports a failed step on stderr; true when `rc` is ok.
template <typename T>
bool Check(const absl::StatusOr<T> &rc, const char *what) {
  if (!rc.ok()) fprintf(stderr, "%s: %s\n", what, rc.status().ToString().c_str());
  return rc.ok();
}

}  // namespace

std::string CreateDelayDevice(
    const std::string &name, const std::string &device, int delay_ms) {
  uint64_t bytes = 0;
  {
    absl::StatusOr<FileDescriptor> dev =
        syscalls::openat(AT_FDCWD, device, O_RDONLY);
    if (!Check(dev, ("open " + device).c_str())) return "";
    if (!Check(syscalls::ioctl(**dev, BLKGETSIZE64, &bytes),
               ("BLKGETSIZE64 " + device).c_str())) {
      return "";
    }
  }
  absl::StatusOr<FileDescriptor> control_fd = OpenControl();
  if (!Check(control_fd, "no /dev/mapper/control")) return "";
  const int control = **control_fd;

  std::vector<char> buf(16384);
  auto *io = reinterpret_cast<dm_ioctl *>(buf.data());

  Init(io, buf.size(), name);
  if (!Check(syscalls::ioctl(control, DM_DEV_CREATE, io), "DM_DEV_CREATE")) {
    return "";
  }
  const uint64_t dev = io->dev;

  Init(io, buf.size(), name);
  io->target_count = 1;
  auto *spec = reinterpret_cast<dm_target_spec *>(buf.data() + io->data_start);
  spec->sector_start = 0;
  spec->length = bytes / 512;
  spec->status = 0;
  snprintf(spec->target_type, sizeof spec->target_type, "delay");
  char *params = reinterpret_cast<char *>(spec + 1);
  int n = snprintf(params, 256, "%s 0 %d", device.c_str(), delay_ms);
  size_t used = sizeof(dm_target_spec) + static_cast<size_t>(n) + 1;
  used = (used + 7) & ~static_cast<size_t>(7);
  spec->next = static_cast<uint32_t>(used);
  if (!Check(syscalls::ioctl(control, DM_TABLE_LOAD, io), "DM_TABLE_LOAD")) {
    return "";
  }

  Init(io, buf.size(), name);
  // flags 0: resume
  if (!Check(syscalls::ioctl(control, DM_DEV_SUSPEND, io),
             "DM_DEV_SUSPEND (resume)")) {
    return "";
  }
  control_fd = absl::InternalError("closed");  // closes the control fd

  char node[64];
  snprintf(node, sizeof node, "/dev/dm-%u", minor(dev));
  for (int i = 0; i < 100 && !Exists(node); ++i) SleepMicros(50000);
  if (!Exists(node)) {
    syscalls::mknodat(AT_FDCWD, node, S_IFBLK | 0600, dev).IgnoreError();
  }
  return node;
}

}  // namespace dcfs_bench
