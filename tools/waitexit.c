/* SPDX-License-Identifier: MIT */
#define _GNU_SOURCE
#include <errno.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/pidfd.h>
#include <unistd.h>

#include "tools/testutil.h"

static void print_err(int err) {
  const char *name = strerrorname_np(err);

  printf("ERR %s\n", name ? name : "UNKNOWN");
}

/* The event source for a process: a pidfd, readable once it has exited.
 * Returns -1 with *gone set when there is none to watch because it is already
 * gone (or no pid was given: *gone stays 0). */
int watch_pid(const char *pid_str, int *gone) {
  long pid = pid_str == NULL ? 0 : atol(pid_str);
  int fd;

  *gone = 0;
  if (pid <= 0) return -1;
  fd = pidfd_open((pid_t)pid, 0);
  if (fd == -1) *gone = 1;
  return fd;
}

int cmd_waitexit(char *const pids[], int count) {
  int k;

  for (k = 0; k < count; k++) {
    struct pollfd p;
    int gone, pfd = watch_pid(pids[k], &gone);

    if (gone) continue;
    if (pfd == -1) {
      fprintf(stderr, "testutil waitexit: not a pid: %s\n", pids[k]);
      return 2;
    }
    p = (struct pollfd){.fd = pfd, .events = POLLIN};
    for (;;) {
      p.revents = 0;
      int rc = poll(&p, 1, -1);
      if (rc == -1) {
        if (errno == EINTR) continue;
        print_err(errno);
        close(pfd);
        return 2;
      }
      if (rc > 0 && (p.revents & (POLLIN | POLLHUP | POLLERR))) {
        break;
      }
    }
    close(pfd);
  }
  return 0;
}
