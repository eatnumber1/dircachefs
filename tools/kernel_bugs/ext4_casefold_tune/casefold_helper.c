/* Step 26.15: the two ioctls reproduce.sh needs that no stock tool issues.
 *
 *   casefold_helper enable <mountpoint>
 *       EXT4_IOC_SET_TUNE_SB_PARAM (Linux 6.18+) setting the casefold
 *       feature on the mounted ext4 filesystem <mountpoint> is on, as
 *       `tune2fs -O casefold` does on an unmounted one.
 *   casefold_helper mark <dir>
 *       FS_IOC_SETFLAGS adding FS_CASEFOLD_FL to the empty directory <dir>,
 *       as `chattr +F <dir>` (e2fsprogs 1.45+) does.
 *
 * Exit status: 0 done; 2 the kernel refused with EOPNOTSUPP, EINVAL or
 * ENOTTY (the ioctl or flag is not supported: not the bug); 1 any other
 * error. The error is printed.
 *
 * Standalone: no dcfs code. Build: cc -static -o casefold_helper
 * casefold_helper.c
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <linux/fs.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

/* Copied from the kernel's <linux/ext4.h> UAPI header (include/uapi/linux/
 * ext4.h): many C libraries' headers predate it. */
struct ext4_tune_sb_params {
  uint32_t set_flags;
  uint32_t checkinterval;
  uint16_t errors_behavior;
  uint16_t mnt_count;
  uint16_t max_mnt_count;
  uint16_t raid_stride;
  uint64_t last_check_time;
  uint64_t reserved_blocks;
  uint64_t blocks_count;
  uint32_t default_mnt_opts;
  uint32_t reserved_uid;
  uint32_t reserved_gid;
  uint32_t raid_stripe_width;
  uint16_t encoding;
  uint16_t encoding_flags;
  uint8_t def_hash_alg;
  uint8_t pad_1;
  uint16_t pad_2;
  uint32_t feature_compat;
  uint32_t feature_incompat;
  uint32_t feature_ro_compat;
  uint32_t set_feature_compat_mask;
  uint32_t set_feature_incompat_mask;
  uint32_t set_feature_ro_compat_mask;
  uint32_t clear_feature_compat_mask;
  uint32_t clear_feature_incompat_mask;
  uint32_t clear_feature_ro_compat_mask;
  uint8_t mount_opts[64];
  uint8_t pad[68];
};
/* The struct above has pad[68], as 6.19-rc1 and the stable kernels since
 * (6.18.x too) have it; Linux 6.18.0 had pad[64] ("ext4: fix
 * ext4_tune_sb_params padding"), which changes the size, and so the number, of
 * the ioctls. They are built from the size so that both can be tried. */
#define TUNE_OLD_PAD_SIZE (sizeof(struct ext4_tune_sb_params) - 4)
#define EXT4_IOC_GET_TUNE_SB_PARAM(size) _IOC(_IOC_READ, 'f', 45, size)
#define EXT4_IOC_SET_TUNE_SB_PARAM(size) _IOC(_IOC_WRITE, 'f', 46, size)
#define EXT4_TUNE_FL_EDIT_FEATURES 0x00004000
#define EXT4_FEATURE_INCOMPAT_CASEFOLD 0x20000
#ifndef FS_CASEFOLD_FL
#define FS_CASEFOLD_FL 0x40000000
#endif

/* Prints the error as "<what>: <message> (errno N)" and returns the exit
 * status for it. */
static int fail(const char *what) {
  int err = errno;
  printf("%s: %s (errno %d)\n", what, strerror(err), err);
  return err == EOPNOTSUPP || err == EINVAL || err == ENOTTY ? 2 : 1;
}

static int enable(const char *mountpoint) {
  struct ext4_tune_sb_params params;
  size_t size = sizeof(params);
  int rc;
  int fd = open(mountpoint, O_RDONLY | O_DIRECTORY);

  memset(&params, 0, sizeof(params));
  if (fd == -1) return fail("open");
  rc = ioctl(fd, EXT4_IOC_GET_TUNE_SB_PARAM(size), &params);
  if (rc == -1 && errno == ENOTTY) {
    size = TUNE_OLD_PAD_SIZE;
    rc = ioctl(fd, EXT4_IOC_GET_TUNE_SB_PARAM(size), &params);
  }
  if (rc == -1) return fail("EXT4_IOC_GET_TUNE_SB_PARAM");
  params.set_flags = EXT4_TUNE_FL_EDIT_FEATURES;
  params.set_feature_compat_mask = 0;
  params.set_feature_incompat_mask = EXT4_FEATURE_INCOMPAT_CASEFOLD;
  params.set_feature_ro_compat_mask = 0;
  params.clear_feature_compat_mask = 0;
  params.clear_feature_incompat_mask = 0;
  params.clear_feature_ro_compat_mask = 0;
  if (ioctl(fd, EXT4_IOC_SET_TUNE_SB_PARAM(size), &params) == -1)
    return fail("EXT4_IOC_SET_TUNE_SB_PARAM");
  close(fd);
  return 0;
}

static int mark(const char *dir) {
  int flags = 0;
  int fd = open(dir, O_RDONLY | O_DIRECTORY);

  if (fd == -1) return fail("open");
  if (ioctl(fd, FS_IOC_GETFLAGS, &flags) == -1) return fail("FS_IOC_GETFLAGS");
  flags |= FS_CASEFOLD_FL;
  if (ioctl(fd, FS_IOC_SETFLAGS, &flags) == -1) return fail("FS_IOC_SETFLAGS");
  close(fd);
  return 0;
}

int main(int argc, char **argv) {
  if (argc == 3 && strcmp(argv[1], "enable") == 0) return enable(argv[2]);
  if (argc == 3 && strcmp(argv[1], "mark") == 0) return mark(argv[2]);
  fprintf(stderr, "usage: %s enable <mountpoint> | mark <dir>\n", argv[0]);
  return 64;
}
