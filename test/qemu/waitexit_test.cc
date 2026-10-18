// SPDX-License-Identifier: MIT
// Unit test of testutil waitexit with link-time fakes of
// pidfd_open and poll.

#include <fcntl.h>
#include <poll.h>
#include <sys/types.h>
#include <unistd.h>

#include <cerrno>
#include <vector>

#include "gtest/gtest.h"
#include "tools/testutil.h"

extern "C" int __real_pidfd_open(pid_t pid, unsigned int flags);
extern "C" int __real_poll(struct pollfd *fds, nfds_t nfds, int timeout);

namespace {

struct PollStep {
  int rc;
  short revents;
  int err;
};

bool g_fake_pidfd_open = false;
int g_pidfd_open_calls = 0;
pid_t g_last_pid = 0;
unsigned int g_last_flags = 0;
int g_fake_pidfd = -1;
int g_pidfd_open_errno = 0;

bool g_fake_poll = false;
int g_poll_calls = 0;
std::vector<PollStep> g_poll_steps;

void ResetFakes() {
  g_fake_pidfd_open = false;
  g_pidfd_open_calls = 0;
  g_last_pid = 0;
  g_last_flags = 0;
  if (g_fake_pidfd != -1) {
    close(g_fake_pidfd);
    g_fake_pidfd = -1;
  }
  g_pidfd_open_errno = 0;

  g_fake_poll = false;
  g_poll_calls = 0;
  g_poll_steps.clear();
}

}  // namespace

extern "C" int __wrap_pidfd_open(pid_t pid, unsigned int flags) {
  if (!g_fake_pidfd_open) {
    return __real_pidfd_open(pid, flags);
  }
  g_pidfd_open_calls++;
  g_last_pid = pid;
  g_last_flags = flags;
  if (g_pidfd_open_errno != 0) {
    errno = g_pidfd_open_errno;
    return -1;
  }
  if (g_fake_pidfd == -1) {
    g_fake_pidfd = open("/dev/null", O_RDONLY | O_CLOEXEC);
  }
  return dup(g_fake_pidfd);
}

extern "C" int __wrap_poll(struct pollfd *fds, nfds_t nfds, int timeout) {
  if (!g_fake_poll) {
    return __real_poll(fds, nfds, timeout);
  }
  g_poll_calls++;
  if (g_poll_steps.empty()) {
    for (nfds_t i = 0; i < nfds; ++i) {
      fds[i].revents = POLLIN;
    }
    return static_cast<int>(nfds);
  }
  PollStep step = g_poll_steps.front();
  if (g_poll_steps.size() > 1) {
    g_poll_steps.erase(g_poll_steps.begin());
  }
  if (step.rc == -1) {
    errno = step.err;
    return -1;
  }
  for (nfds_t i = 0; i < nfds; ++i) {
    fds[i].revents = step.revents;
  }
  return step.rc;
}

namespace {

TEST(WaitexitTest, NotReadyPollRetriesAndReadableReturnsZero) {
  ResetFakes();
  g_fake_pidfd_open = true;
  g_fake_poll = true;
  g_poll_steps = {
      PollStep{.rc = 0, .revents = 0, .err = 0},
      PollStep{.rc = 1, .revents = POLLIN, .err = 0},
  };

  char pid_str[] = "12345";
  char *pids[] = {pid_str};
  int rc = cmd_waitexit(pids, 1);

  EXPECT_EQ(rc, 0);
  EXPECT_EQ(g_pidfd_open_calls, 1);
  EXPECT_EQ(g_last_pid, 12345);
  EXPECT_EQ(g_poll_calls, 2);

  ResetFakes();
}

TEST(WaitexitTest, ProcessAlreadyGoneReturnsZero) {
  ResetFakes();
  g_fake_pidfd_open = true;
  g_fake_poll = true;
  g_pidfd_open_errno = ESRCH;

  char pid_str[] = "99999";
  char *pids[] = {pid_str};
  int rc = cmd_waitexit(pids, 1);

  EXPECT_EQ(rc, 0);
  EXPECT_EQ(g_pidfd_open_calls, 1);
  EXPECT_EQ(g_poll_calls, 0);

  ResetFakes();
}

TEST(WaitexitTest, NotAPidReturnsTwo) {
  ResetFakes();
  g_fake_pidfd_open = true;
  g_fake_poll = true;

  char invalid_pid[] = "not-a-pid";
  char *pids[] = {invalid_pid};
  int rc = cmd_waitexit(pids, 1);

  EXPECT_EQ(rc, 2);
  EXPECT_EQ(g_pidfd_open_calls, 0);
  EXPECT_EQ(g_poll_calls, 0);

  ResetFakes();
}

TEST(WaitexitTest, PollEintrRetries) {
  ResetFakes();
  g_fake_pidfd_open = true;
  g_fake_poll = true;
  g_poll_steps = {
      PollStep{.rc = -1, .revents = 0, .err = EINTR},
      PollStep{.rc = 1, .revents = POLLIN, .err = 0},
  };

  char pid_str[] = "12345";
  char *pids[] = {pid_str};
  int rc = cmd_waitexit(pids, 1);

  EXPECT_EQ(rc, 0);
  EXPECT_EQ(g_pidfd_open_calls, 1);
  EXPECT_EQ(g_poll_calls, 2);

  ResetFakes();
}

}  // namespace
