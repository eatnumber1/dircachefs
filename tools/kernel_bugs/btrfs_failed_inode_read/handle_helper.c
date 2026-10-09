/* Step 26.15: name_to_handle_at(2) and open_by_handle_at(2), which no stock
 * tool of the usual distributions issues, for reproduce.sh.
 *
 *   handle_helper handle <path>
 *       prints "<handle_type> <hex of the handle bytes>" for <path>.
 *   handle_helper open <mountpoint> <file>
 *       reads <file> (lines as `handle` printed them) and calls
 *       open_by_handle_at(<mountpoint>, handle, O_RDONLY) for each line,
 *       closing the result at once; prints "open <n>: ok" or "open <n>:
 *       <message> (errno N)" per line and "opened <ok> failed <failed>".
 *
 * Needs CAP_DAC_READ_SEARCH. Exit status: 0 done (failures of the opens are
 * the point and not an error); 1 any other error.
 *
 * Standalone: no dcfs code. Build: cc -static -o handle_helper
 * handle_helper.c
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

enum { kMaxHandle = 128 };

struct handle_buf {
  struct file_handle h;
  unsigned char bytes[kMaxHandle];
};

static int handle(const char *path) {
  struct handle_buf fh;
  int mount_id;
  unsigned int i;

  fh.h.handle_bytes = kMaxHandle;
  if (name_to_handle_at(AT_FDCWD, path, &fh.h, &mount_id, 0) == -1) {
    printf("name_to_handle_at %s: %s (errno %d)\n", path, strerror(errno),
           errno);
    return 1;
  }
  printf("%d ", fh.h.handle_type);
  for (i = 0; i < fh.h.handle_bytes; i++) printf("%02x", fh.bytes[i]);
  printf("\n");
  return 0;
}

static int parse_hex(const char *hex, unsigned char *out, unsigned int max) {
  unsigned int n = 0;

  while (hex[0] != '\0' && hex[0] != '\n' && hex[1] != '\0' && hex[1] != '\n') {
    char byte[3] = {hex[0], hex[1], '\0'};

    if (n == max) return -1;
    out[n++] = (unsigned char)strtoul(byte, NULL, 16);
    hex += 2;
  }
  return (int)n;
}

static int open_all(const char *mountpoint, const char *file) {
  char line[1024];
  FILE *in = fopen(file, "r");
  int mount_fd = open(mountpoint, O_RDONLY | O_DIRECTORY);
  int ok = 0, failed = 0, n = 0;

  if (in == NULL || mount_fd == -1) {
    printf("open %s or %s: %s (errno %d)\n", mountpoint, file, strerror(errno),
           errno);
    return 1;
  }
  while (fgets(line, sizeof(line), in) != NULL) {
    struct handle_buf fh;
    int type, len, fd;
    char *space = strchr(line, ' ');

    if (space == NULL) continue;
    type = atoi(line);
    len = parse_hex(space + 1, fh.bytes, kMaxHandle);
    if (len < 0) continue;
    fh.h.handle_type = type;
    fh.h.handle_bytes = (unsigned int)len;
    n++;
    fd = open_by_handle_at(mount_fd, &fh.h, O_RDONLY);
    if (fd == -1) {
      printf("open %d: %s (errno %d)\n", n, strerror(errno), errno);
      failed++;
    } else {
      printf("open %d: ok\n", n);
      close(fd);
      ok++;
    }
  }
  printf("opened %d failed %d\n", ok, failed);
  return 0;
}

int main(int argc, char **argv) {
  if (argc == 3 && strcmp(argv[1], "handle") == 0) return handle(argv[2]);
  if (argc == 4 && strcmp(argv[1], "open") == 0)
    return open_all(argv[2], argv[3]);
  fprintf(stderr, "usage: %s handle <path> | open <mountpoint> <file>\n",
          argv[0]);
  return 64;
}
