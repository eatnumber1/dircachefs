/* Copied from fuse-generation-qemu/guest/fhtest.c; keep in sync by hand. */
/*
 * fhtest - file handle test client for the FUSE generation work.
 *
 * Subcommands:
 *   fhtest getversion <path>
 *       Print the inode generation (FS_IOC_GETVERSION) of <path>.
 *   fhtest handle <path>
 *       Print the file handle of <path> as "type len hexbytes".
 *   fhtest gen <path>
 *       Print fh[2] (the generation word of FILEID_INO64_GEN handles).
 *   fhtest open <mount> <type> <hexbytes>
 *       open_by_handle_at() the given handle, read up to 64 bytes, print
 *       "OK <n> <data>" or "ERR <errno-name>".
 *   fhtest bumpgen <hexbytes>
 *       Print the handle bytes with fh[2] incremented (doctored handle).
 *
 * All output is single-line so the guest test script can capture it.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <linux/fs.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <unistd.h>

#define MAX_HANDLE_SZ 128

/* struct file_handle ends in a flexible array member; give it room with
   a union so the layout is valid C rather than a GNU extension. */
union fh_buf {
  struct file_handle h;
  unsigned char bytes[sizeof(struct file_handle) + MAX_HANDLE_SZ];
};

static const char *errname(int err) {
  switch (err) {
    case ESTALE:
      return "ESTALE";
    case ENOENT:
      return "ENOENT";
    case EIO:
      return "EIO";
    case EINVAL:
      return "EINVAL";
    case EOPNOTSUPP:
      return "EOPNOTSUPP";
    case EBADF:
      return "EBADF";
    case EPERM:
      return "EPERM";
    default:
      return strerror(err);
  }
}

static int cmd_getversion(const char *path) {
  long gen = 0;
  int fd = open(path, O_RDONLY | O_NONBLOCK);

  if (fd == -1) {
    printf("ERR %s\n", errname(errno));
    return 1;
  }
  if (ioctl(fd, FS_IOC_GETVERSION, &gen) == -1) {
    printf("ERR %s\n", errname(errno));
    close(fd);
    return 1;
  }
  close(fd);
  printf("%lu\n", (unsigned long)(uint32_t)gen);
  return 0;
}

static int get_handle(const char *path, union fh_buf *fh, int *mount_id) {
  fh->h.handle_bytes = MAX_HANDLE_SZ;
  if (name_to_handle_at(AT_FDCWD, path, &fh->h, mount_id, 0) == -1) {
    printf("ERR %s\n", errname(errno));
    return -1;
  }
  return 0;
}

static void print_handle(const union fh_buf *fh) {
  unsigned int i;

  printf("%d %u ", fh->h.handle_type, fh->h.handle_bytes);
  for (i = 0; i < fh->h.handle_bytes; i++) printf("%02x", fh->h.f_handle[i]);
  printf("\n");
}

static int cmd_handle(const char *path) {
  union fh_buf fh;
  int mount_id;

  if (get_handle(path, &fh, &mount_id)) return 1;
  print_handle(&fh);
  return 0;
}

static int cmd_gen(const char *path) {
  union fh_buf fh;
  int mount_id;
  uint32_t *words;

  if (get_handle(path, &fh, &mount_id)) return 1;
  if (fh.h.handle_bytes < 12) {
    printf("ERR short-handle\n");
    return 1;
  }
  words = (uint32_t *)fh.h.f_handle;
  printf("%u\n", words[2]);
  return 0;
}

static int parse_hex(const char *hex, union fh_buf *fh) {
  size_t len = strlen(hex);
  unsigned int i;

  if (len % 2 || len / 2 > MAX_HANDLE_SZ) {
    printf("ERR bad-hex\n");
    return -1;
  }
  fh->h.handle_bytes = len / 2;
  for (i = 0; i < fh->h.handle_bytes; i++) {
    unsigned int byte;

    if (sscanf(hex + 2 * i, "%2x", &byte) != 1) {
      printf("ERR bad-hex\n");
      return -1;
    }
    fh->h.f_handle[i] = byte;
  }
  return 0;
}

static int cmd_open(const char *mount, const char *type, const char *hex) {
  union fh_buf fh;
  int mount_fd, fd;
  char buf[65];
  ssize_t n;

  fh.h.handle_type = atoi(type);
  if (parse_hex(hex, &fh)) return 1;

  mount_fd = open(mount, O_RDONLY | O_DIRECTORY);
  if (mount_fd == -1) {
    printf("ERR %s\n", errname(errno));
    return 1;
  }

  fd = open_by_handle_at(mount_fd, &fh.h, O_RDONLY);
  if (fd == -1) {
    printf("ERR %s\n", errname(errno));
    close(mount_fd);
    return 1;
  }

  n = read(fd, buf, sizeof(buf) - 1);
  if (n < 0) {
    printf("ERR %s\n", errname(errno));
    close(fd);
    close(mount_fd);
    return 1;
  }
  buf[n] = '\0';
  /* strip trailing newline for single-line output */
  if (n > 0 && buf[n - 1] == '\n') buf[n - 1] = '\0';
  printf("OK %zd %s\n", n, buf);
  close(fd);
  close(mount_fd);
  return 0;
}

static int cmd_bumpgen(const char *hex) {
  union fh_buf fh;
  uint32_t *words;
  unsigned int i;

  if (parse_hex(hex, &fh)) return 1;
  if (fh.h.handle_bytes < 12) {
    printf("ERR short-handle\n");
    return 1;
  }
  words = (uint32_t *)fh.h.f_handle;
  words[2]++;
  for (i = 0; i < fh.h.handle_bytes; i++) printf("%02x", fh.h.f_handle[i]);
  printf("\n");
  return 0;
}

int main(int argc, char *argv[]) {
  if (argc >= 3 && strcmp(argv[1], "getversion") == 0)
    return cmd_getversion(argv[2]);
  if (argc >= 3 && strcmp(argv[1], "handle") == 0) return cmd_handle(argv[2]);
  if (argc >= 3 && strcmp(argv[1], "gen") == 0) return cmd_gen(argv[2]);
  if (argc >= 5 && strcmp(argv[1], "open") == 0)
    return cmd_open(argv[2], argv[3], argv[4]);
  if (argc >= 3 && strcmp(argv[1], "bumpgen") == 0) return cmd_bumpgen(argv[2]);

  fprintf(stderr,
          "usage: fhtest getversion|handle|gen <path>\n"
          "       fhtest open <mount> <type> <hexbytes>\n"
          "       fhtest bumpgen <hexbytes>\n");
  return 2;
}
