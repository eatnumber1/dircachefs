#include "bench/dm_delay.h"

#include <fcntl.h>
#include <linux/dm-ioctl.h>
#include <linux/fs.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <unistd.h>

#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <vector>

namespace dcfs_bench {
namespace {

int OpenControl() {
  int fd = open("/dev/mapper/control", O_RDWR);
  if (fd >= 0) return fd;
  // devtmpfs makes the node for the misc device on its own; this is the
  // fallback for a guest where it did not.
  std::ifstream misc("/proc/misc");
  unsigned minor;
  std::string name;
  while (misc >> minor >> name) {
    if (name == "device-mapper") {
      mkdir("/dev/mapper", 0755);
      if (mknod("/dev/mapper/control", S_IFCHR | 0600, makedev(10, minor)) !=
          0) {
        break;
      }
      return open("/dev/mapper/control", O_RDWR);
    }
  }
  fprintf(stderr, "no /dev/mapper/control: %s\n", strerror(errno));
  return -1;
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

}  // namespace

std::string CreateDelayDevice(
    const std::string &name, const std::string &device, int delay_ms) {
  int dev_fd = open(device.c_str(), O_RDONLY);
  if (dev_fd < 0) {
    fprintf(stderr, "open %s: %s\n", device.c_str(), strerror(errno));
    return "";
  }
  uint64_t bytes = 0;
  int rc = ioctl(dev_fd, BLKGETSIZE64, &bytes);
  close(dev_fd);
  if (rc != 0) {
    fprintf(stderr, "BLKGETSIZE64 %s: %s\n", device.c_str(), strerror(errno));
    return "";
  }
  int control = OpenControl();
  if (control < 0) return "";

  std::vector<char> buf(16384);
  auto *io = reinterpret_cast<dm_ioctl *>(buf.data());

  Init(io, buf.size(), name);
  if (ioctl(control, DM_DEV_CREATE, io) != 0) {
    fprintf(stderr, "DM_DEV_CREATE: %s\n", strerror(errno));
    close(control);
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
  if (ioctl(control, DM_TABLE_LOAD, io) != 0) {
    fprintf(stderr, "DM_TABLE_LOAD: %s\n", strerror(errno));
    close(control);
    return "";
  }

  Init(io, buf.size(), name);
  if (ioctl(control, DM_DEV_SUSPEND, io) != 0) {  // flags 0: resume
    fprintf(stderr, "DM_DEV_SUSPEND (resume): %s\n", strerror(errno));
    close(control);
    return "";
  }
  close(control);

  char node[64];
  snprintf(node, sizeof node, "/dev/dm-%u", minor(dev));
  for (int i = 0; i < 100 && access(node, F_OK) != 0; ++i) usleep(50000);
  if (access(node, F_OK) != 0) mknod(node, S_IFBLK | 0600, dev);
  return node;
}

}  // namespace dcfs_bench
