#include "dfs/fuse.h"

#include <cstdlib>
#include <iostream>
#include <utility>
#include <cstdint>
#include <unistd.h>
#include <fcntl.h>

#include "fuse/fuse_kernel.h"
#include "dfs/syscalls.h"
#include "absl/status/status.h"
#include "absl/log/check.h"
#include "absl/log/log.h"
#include "absl/strings/str_format.h"
#include "absl/strings/str_join.h"
#include "absl/strings/str_cat.h"

namespace dfs {

FuseMount::FuseMount(Mount mount, FileDescriptor fuse_fd)
    : mount_(std::move(mount)), fuse_fd_(std::move(fuse_fd)) {}

absl::StatusOr<FuseMount> FuseMount::Create(
      std::string mountpoint, Options options) {
  absl::StatusOr<FileDescriptor> fuse_fd =
    syscalls::open("/dev/fuse", O_CLOEXEC | O_RDWR);
  if (!fuse_fd.ok()) return std::move(fuse_fd).status();

  absl::StatusOr<struct stat> dirstat = syscalls::stat(mountpoint.c_str());
  if (!dirstat.ok()) return std::move(dirstat).status();

  absl::flat_hash_set<std::string> mount_options =
    std::move(options.mount_options);
  mount_options.merge(absl::flat_hash_set<std::string>{
      absl::StrCat("fd=", *(*fuse_fd)),
      absl::StrFormat("rootmode=%o", (S_IFMT & dirstat->st_mode)),
      absl::StrCat("user_id=", getuid()),
      absl::StrCat("group_id=", getgid()),
  });
  if (!options.fsname.empty()) {
    mount_options.insert(absl::StrCat("subtype=", options.fsname));
  }

  std::string opts = absl::StrJoin(mount_options, ",");
  LOG(INFO) << "Options are " << opts;
  absl::StatusOr<Mount> mount = syscalls::mount(
        options.mount_source.c_str(), /*target=*/std::move(mountpoint),
        /*filesystemtype=*/"fuse",
        /*flags=*/0, opts.c_str());
  if (!mount.ok()) return std::move(mount).status();

  FuseMount fuse_mount(*std::move(mount), *std::move(fuse_fd));

  if (absl::Status st = fuse_mount.Handshake(); !st.ok()) return st;

  return fuse_mount;
}

absl::Status FuseMount::Handshake() {
  char buf[FUSE_MIN_READ_BUFFER];
  absl::StatusOr<size_t> nb = syscalls::read(*fuse_fd_, buf, sizeof(buf));
  if (!nb.ok()) return std::move(nb).status();
  CHECK_NE(*nb, 0ul);

  auto &header = *reinterpret_cast<fuse_in_header*>(buf);

  CHECK_EQ(header.opcode, FUSE_INIT);
  //CHECK_GE(header.len, sizeof(fuse_in_header) + sizeof(fuse_init_in));
  LOG(INFO) << "Got header";

  auto &init_in = *reinterpret_cast<fuse_init_in*>(buf + sizeof(fuse_in_header));
  CHECK_EQ(init_in.major, 7u);
  CHECK_EQ(init_in.minor, 34u);
  //absl::StatusOr<fuse_init_in> init_in = ReadInit(fd);
  //if (!init_in.ok()) return std::move(init_in).status();
  LOG(INFO) << "init_in.max_readahead = " << init_in.max_readahead;
  LOG(INFO) << "init_in.flags = " << init_in.flags;
  return absl::OkStatus();
}

#if 0
  absl::StatusOr<fuse_in_header> ReadHeader(int fd) {
    // FUSE_MIN_READ_BUFFER
    // TODO allow configuring buffer size up to
    // https://github.com/libfuse/libfuse/blob/36c2250d1098253f74e670be09f35c2dde642b2a/lib/fuse_lowlevel.c#L2010
    fuse_in_header header;
    absl::StatusOr<size_t> nb = syscalls::read(fd, &header, sizeof(header));
    if (!nb.ok()) return std::move(nb).status();
    CHECK_EQ(*nb, sizeof(header)) << "Didn't read a full fuse_in_header";
    return header;
  }

  absl::StatusOr<fuse_init_in> ReadInit(int fd) {
    absl::StatusOr<fuse_in_header> header = ReadHeader(fd);
    if (!header.ok()) return std::move(header).status();
    CHECK_EQ(header->len, sizeof(fuse_in_header) + sizeof(fuse_init_in));
    CHECK_EQ(header->opcode, FUSE_INIT);
    LOG(INFO) << "Got header";

    fuse_init_in init;
    absl::StatusOr<size_t> nb = syscalls::read(fd, &init, sizeof(init));
    if (!nb.ok()) return std::move(nb).status();
    CHECK_EQ(*nb, sizeof(init)) << "Didn't read a full fuse_init_in";
    return init;
  }
#endif

}  // namespace dfs
